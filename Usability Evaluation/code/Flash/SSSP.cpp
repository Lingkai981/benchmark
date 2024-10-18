#include "../core/api.h"
class SSSPtest : public testAppBase<FRAG_T, SSSP_TYPE> {
 public:
  INSTALL_test_WORKER(SSSPtest<FRAG_T>, SSSP_TYPE, FRAG_T)
  using context_t = testVertexDataContext<FRAG_T, SSSP_TYPE, double>;

  bool sync_all_ = false;

  double* Res(value_t* v) { return &(v->dis); }

  void Run(const fragment_t& graph, const std::shared_ptr<fw_t> fw,
           oid_t o_source) {
    vid_t source = Oid2testId(o_source);
    int n_vertex = graph.GetTotalVerticesNum();
    LOG(INFO) << "Run SSSP with test, total vertices: " << n_vertex
              << std::endl;

    DefineMapV(init_v) { v.dis = (id == source) ? 0 : -1; };
    vset_t a = All;
    a = VertexMap(a, CTrueV, init_v);

    DefineFV(f_filter) { return id == source; };
    a = VertexMap(a, f_filter);

    DefineFE(check) { return (d.dis < -0.5 || d.dis > s.dis + weight); };
    DefineMapE(update) {
      if (d.dis < -0.5 || d.dis > s.dis + weight)
        d.dis = s.dis + weight;
    };
    DefineMapE(reduce) {
      if (d.dis < -0.5 || d.dis > s.dis)
        d.dis = s.dis;
    };

    for (int len = VSize(a), i = 1; len > 0; len = VSize(a), ++i) {
      LOG(INFO) << "Round " << i << ": size=" << len << std::endl;
      a = EdgeMap(a, ED, check, update, CTrueV, reduce);
    }
  }
};
