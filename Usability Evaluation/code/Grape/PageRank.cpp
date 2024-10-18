#ifndef EXAMPLES_ANALYTICAL_APPS_PAGERANK_PAGERANK_H_
#define EXAMPLES_ANALYTICAL_APPS_PAGERANK_PAGERANK_H_

#include <test/test.h>
#include <iomanip>

namespace test {

/**
 * @brief Context for the batch version of PageRank.
 *
 * @tparam FRAG_T
 */
template <typename FRAG_T>
class PageRankContext : public VertexDataContext<FRAG_T, double> {
  using oid_t = typename FRAG_T::oid_t;
  using vid_t = typename FRAG_T::vid_t;

 public:
  explicit PageRankContext(const FRAG_T& fragment)
      : VertexDataContext<FRAG_T, double>(fragment, true),
        result(this->data()) {
    auto inner_vertices = fragment.InnerVertices();
    auto vertices = fragment.Vertices();
    degree.Init(inner_vertices);
    next_result.Init(vertices);
    avg_degree = static_cast<double>(fragment.GetEdgeNum()) /
                 static_cast<double>(fragment.GetInnerVerticesNum());

    send_buffers.resize(fragment.fnum());
    recv_buffers.resize(fragment.fnum());
    for (fid_t k = 0; k < fragment.fnum(); k++) {
      size_t send_size = fragment.MirrorVertices(k).size() * sizeof(double);
      size_t recv_size = fragment.OuterVertices(k).size() * sizeof(double);
      send_buffers[k].resize(send_size);
      memset(send_buffers[k].data(), 0, send_size);
      recv_buffers[k].resize(recv_size);
      memset(recv_buffers[k].data(), 0, recv_size);
    }

#ifdef PROFILING
    preprocess_time = 0;
    exec_time = 0;
    postprocess_time = 0;
#endif
  }
  ~PageRankContext() {}

  void Init(BatchShuffleMessageManager& messages, double delta, int max_round) {
    this->delta = delta;
    this->max_round = max_round;
    for (fid_t i = 0; i < this->fragment().fnum(); i++) {
      messages.SetupBuffer(i, std::move(send_buffers[i]),
                           std::move(recv_buffers[i]));
    }
    step = 0;
  }

  void Output(std::ostream& os) override {
    auto& frag = this->fragment();
    auto inner_vertices = frag.InnerVertices();
    for (auto v : inner_vertices) {
      os << frag.GetId(v) << " " << std::scientific << std::setprecision(15)
         << result[v] << std::endl;
    }
#ifdef PROFILING
    VLOG(2) << "preprocess_time: " << preprocess_time << "s.";
    VLOG(2) << "exec_time: " << exec_time << "s.";
    VLOG(2) << "postprocess_time: " << postprocess_time << "s.";
#endif
  }

  typename FRAG_T::template inner_vertex_array_t<int> degree;
  typename FRAG_T::template vertex_array_t<double>& result;
  typename FRAG_T::template vertex_array_t<double> next_result;
  std::vector<std::vector<char, Allocator<char>>> send_buffers;
  std::vector<std::vector<char, Allocator<char>>> recv_buffers;

#ifdef PROFILING
  double preprocess_time = 0;
  double exec_time = 0;
  double postprocess_time = 0;
#endif

  vid_t total_dangling_vnum = 0;
  vid_t graph_vnum;
  int step = 0;
  int max_round = 0;
  double delta = 0;

  double dangling_sum = 0.0;
  double avg_degree = 0;
};

/**
 * @brief An implementation of PageRank, which can work
 * on undirected graphs.
 *
 * This version of PageRank inherits BatchShuffleAppBase.
 * Messages are generated in batches and received in-place.
 *
 * @tparam FRAG_T
 */
template <typename FRAG_T>
class PageRank : public BatchShuffleAppBase<FRAG_T, PageRankContext<FRAG_T>>,
                 public ParallelEngine,
                 public Communicator {
 public:
  INSTALL_BATCH_SHUFFLE_WORKER(PageRank<FRAG_T>, PageRankContext<FRAG_T>,
                               FRAG_T)

  using vertex_t = typename FRAG_T::vertex_t;
  using vid_t = typename FRAG_T::vid_t;

  static constexpr bool need_split_edges = true;
  static constexpr bool need_split_edges_by_fragment = true;
  static constexpr MessageStrategy message_strategy =
      MessageStrategy::kAlongOutgoingEdgeToOuterVertex;
  static constexpr LoadStrategy load_strategy = LoadStrategy::kOnlyOut;

  PageRank() = default;

  void PEval(const fragment_t& frag, context_t& ctx,
             message_manager_t& messages) {
    if (ctx.max_round <= 0) {
      return;
    }

    auto inner_vertices = frag.InnerVertices();

#ifdef PROFILING
    ctx.exec_time -= GetCurrentTime();
#endif

    ctx.step = 0;
    ctx.graph_vnum = frag.GetTotalVerticesNum();
    vid_t dangling_vnum = 0;
    double p = 1.0 / ctx.graph_vnum;

    std::vector<vid_t> dangling_vnum_tid(thread_num(), 0);
    ForEach(inner_vertices,
            [&ctx, &frag, p, &dangling_vnum_tid](int tid, vertex_t u) {
              int EdgeNum = frag.GetLocalOutDegree(u);
              ctx.degree[u] = EdgeNum;
              if (EdgeNum > 0) {
                ctx.result[u] = p / EdgeNum;
              } else {
                ++dangling_vnum_tid[tid];
                ctx.result[u] = p;
              }
              ctx.result[u] = EdgeNum > 0 ? p / EdgeNum : p;
            });

    for (auto vn : dangling_vnum_tid) {
      dangling_vnum += vn;
    }

    Sum(dangling_vnum, ctx.total_dangling_vnum);
    ctx.dangling_sum = p * ctx.total_dangling_vnum;

#ifdef PROFILING
    ctx.exec_time += GetCurrentTime();
    ctx.postprocess_time -= GetCurrentTime();
#endif

    messages.SyncInnerVertices<fragment_t, double>(frag, ctx.result,
                                                   thread_num());
#ifdef PROFILING
    ctx.postprocess_time += GetCurrentTime();
#endif
  }

  void IncEval(const fragment_t& frag, context_t& ctx,
               message_manager_t& messages) {
    auto inner_vertices = frag.InnerVertices();
    ++ctx.step;

    double base = (1.0 - ctx.delta) / ctx.graph_vnum +
                  ctx.delta * ctx.dangling_sum / ctx.graph_vnum;
    ctx.dangling_sum = base * ctx.total_dangling_vnum;

#ifdef PROFILING
    ctx.preprocess_time -= GetCurrentTime();
#endif
    messages.UpdateOuterVertices();
#ifdef PROFILING
    ctx.preprocess_time += GetCurrentTime();
    ctx.exec_time -= GetCurrentTime();
#endif
    ForEach(inner_vertices, [&ctx, &frag, base](int tid, vertex_t u) {
      double cur = 0;
      auto es = frag.GetOutgoingAdjList(u);
      for (auto& e : es) {
        cur += ctx.result[e.get_neighbor()];
      }
      int en = frag.GetLocalOutDegree(u);
      ctx.next_result[u] = en > 0 ? (ctx.delta * cur + base) / en : base;
    });
#ifdef PROFILING
    ctx.exec_time += GetCurrentTime();
#endif

    ctx.result.Swap(ctx.next_result);

    if (ctx.step != ctx.max_round) {
#ifdef PROFILING
      ctx.postprocess_time -= GetCurrentTime();
#endif
      messages.SyncInnerVertices<fragment_t, double>(frag, ctx.result,
                                                     thread_num());
#ifdef PROFILING
      ctx.postprocess_time += GetCurrentTime();
#endif
    } else {
      auto& degree = ctx.degree;
      auto& result = ctx.result;

      for (auto v : inner_vertices) {
        if (degree[v] != 0) {
          result[v] *= degree[v];
        }
      }
      return;
    }
  }
};

}  // namespace test

#endif  // EXAMPLES_ANALYTICAL_APPS_PAGERANK_PAGERANK_H_
