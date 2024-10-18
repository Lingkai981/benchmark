#include <vector>
#include <string>
#include <fstream>
#include <test.hpp>
typedef float distance_type;

struct vertex_data : test::IS_POD_TYPE {
  distance_type dist;
  vertex_data(distance_type dist = std::numeric_limits<distance_type>::max()) :
    dist(dist) { }
}; // end of vertex data

struct edge_data : test::IS_POD_TYPE {
  distance_type dist;
  edge_data(distance_type dist = 1) : dist(dist) { }
}; // end of edge data

typedef test::distributed_graph<vertex_data, edge_data> graph_type;

inline graph_type::vertex_type
get_other_vertex(const graph_type::edge_type& edge,
                 const graph_type::vertex_type& vertex) {
  return vertex.id() == edge.source().id()? edge.target() : edge.source();
}

bool DIRECTED_SSSP = false;

struct min_distance_type : test::IS_POD_TYPE {
  distance_type dist;
  min_distance_type(distance_type dist =
                    std::numeric_limits<distance_type>::max()) : dist(dist) { }
  min_distance_type& operator+=(const min_distance_type& other) {
    dist = std::min(dist, other.dist);
    return *this;
  }
};

class sssp :
  public test::ivertex_program<graph_type,
                                   test::empty,
                                   min_distance_type>,
  public test::IS_POD_TYPE {
  distance_type min_dist;
  bool changed;
public:
  void init(icontext_type& context, const vertex_type& vertex,
            const min_distance_type& msg) {
    min_dist = msg.dist;
  }

  edge_dir_type gather_edges(icontext_type& context,
                             const vertex_type& vertex) const {
    return test::NO_EDGES;
  }; // end of gather_edges

  void apply(icontext_type& context, vertex_type& vertex,
             const test::empty& empty) {
    changed = false;
    if(vertex.data().dist > min_dist) {
      changed = true;
      vertex.data().dist = min_dist;
    }
  }

  edge_dir_type scatter_edges(icontext_type& context,
                             const vertex_type& vertex) const {
    if(changed)
      return DIRECTED_SSSP? test::OUT_EDGES : test::ALL_EDGES;
    else return test::NO_EDGES;
  }; // end of scatter_edges

  void scatter(icontext_type& context, const vertex_type& vertex,
               edge_type& edge) const {
    const vertex_type other = get_other_vertex(edge, vertex);
    distance_type newd = vertex.data().dist + edge.data().dist;
    if (other.data().dist > newd) {
      const min_distance_type msg(newd);
      context.signal(other, msg);
    }
  } // end of scatter

}; // end of shortest path vertex program

struct shortest_path_writer {
  std::string save_vertex(const graph_type::vertex_type& vtx) {
    std::stringstream strm;
    strm << vtx.id() << "\t" << vtx.data().dist << "\n";
    return strm.str();
  }
  std::string save_edge(graph_type::edge_type e) { return ""; }
}; // end of shortest_path_writer



struct max_deg_vertex_reducer: public test::IS_POD_TYPE {
  size_t degree;
  test::vertex_id_type vid;
  max_deg_vertex_reducer& operator+=(const max_deg_vertex_reducer& other) {
    if (degree < other.degree) {
      (*this) = other;
    }
    return (*this);
  }
};

max_deg_vertex_reducer find_max_deg_vertex(const graph_type::vertex_type vtx) {
  max_deg_vertex_reducer red;
  red.degree = vtx.num_in_edges() + vtx.num_out_edges();
  red.vid = vtx.id();
  return red;
}

int main(int argc, char** argv) {
  // Initialize control plain using mpi
  test::mpi_tools::init(argc, argv);
  test::distributed_control dc;
  global_logger().set_log_level(LOG_INFO);

  // Parse command line options -----------------------------------------------
  test::command_line_options
    clopts("Single Source Shortest Path Algorithm.");
  std::string graph_dir;
  std::string format = "adj";
  std::string exec_type = "synchronous";
  size_t powerlaw = 0;
  std::vector<unsigned int> sources;
  bool max_degree_source = false;
  clopts.attach_option("graph", graph_dir,
                       "The graph file.  If none is provided "
                       "then a toy graph will be created");
  clopts.add_positional("graph");
  clopts.attach_option("format", format,
                       "graph format");
  clopts.attach_option("source", sources,
                       "The source vertices");
  clopts.attach_option("max_degree_source", max_degree_source,
                       "Add the vertex with maximum degree as a source");

  clopts.add_positional("source");

  clopts.attach_option("directed", DIRECTED_SSSP,
                       "Treat edges as directed.");

  clopts.attach_option("engine", exec_type,
                       "The engine type synchronous or asynchronous");


  clopts.attach_option("powerlaw", powerlaw,
                       "Generate a synthetic powerlaw out-degree graph. ");
  std::string saveprefix;
  clopts.attach_option("saveprefix", saveprefix,
                       "If set, will save the resultant pagerank to a "
                       "sequence of files with prefix saveprefix");

  if(!clopts.parse(argc, argv)) {
    dc.cout() << "Error in parsing command line arguments." << std::endl;
    return EXIT_FAILURE;
  }


  // Build the graph ----------------------------------------------------------
  graph_type graph(dc, clopts);
  if(powerlaw > 0) { // make a synthetic graph
    dc.cout() << "Loading synthetic Powerlaw graph." << std::endl;
    graph.load_synthetic_powerlaw(powerlaw, false, 2, 100000000);
  } else if (graph_dir.length() > 0) { // Load the graph from a file
    dc.cout() << "Loading graph in format: "<< format << std::endl;
    graph.load_format(graph_dir, format);
  } else {
    dc.cout() << "graph or powerlaw option must be specified" << std::endl;
    clopts.print_description();
    return EXIT_FAILURE;
  }
  // must call finalize before querying the graph
  graph.finalize();
  dc.cout() << "#vertices:  " << graph.num_vertices() << std::endl
            << "#edges:     " << graph.num_edges() << std::endl;



  if(sources.empty()) {
    if (max_degree_source == false) {
      dc.cout()
        << "No source vertex provided. Adding vertex 0 as source"
        << std::endl;
      sources.push_back(0);
    }
  }

  if (max_degree_source) {
    max_deg_vertex_reducer v = graph.map_reduce_vertices<max_deg_vertex_reducer>(find_max_deg_vertex);
    dc.cout()
      << "No source vertex provided.  Using highest degree vertex " << v.vid << " as source."
      << std::endl;
    sources.push_back(v.vid);
  }



  // Running The Engine -------------------------------------------------------
  test::omni_engine<sssp> engine(dc, graph, exec_type, clopts);



  // Signal all the vertices in the source set
  for(size_t i = 0; i < sources.size(); ++i) {
    engine.signal(sources[i], min_distance_type(0));
  }

  engine.start();
  const float runtime = engine.elapsed_seconds();
  dc.cout() << "Finished Running engine in " << runtime
            << " seconds." << std::endl;


  // Save the final graph -----------------------------------------------------
  if (saveprefix != "") {
    graph.save(saveprefix, shortest_path_writer(),
               false,    // do not gzip
               true,     // save vertices
               false);   // do not save edges
  }

  // Tear-down communication layer and quit -----------------------------------
  test::mpi_tools::finalize();
  return EXIT_SUCCESS;
} // End of main
