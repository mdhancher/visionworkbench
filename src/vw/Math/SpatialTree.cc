#include <vw/Math/SpatialTree.h>

#include <assert.h>
#include <iostream> // debugging
#include <fstream>
#include <stdexcept>
#include <deque>
using namespace std;

#define LOG_2 0.693147181

// A spatial tree is a hierarchical subdivision of n-dimensional space

// A SpatialTree node can be in 3 states:
//
// 1) leaf (bbox set, no quads, possibly polygons)
// 2) split (bbox set, quads (some may be leaves), possibly polygons)
// 3) unset (no bbox, no quads, no polygons)

namespace {

  using namespace vw::math;

  void split_node(SpatialTree::SpatialTreeNode* node, int num_quadrants, SpatialTree::BBoxT quadrant_bboxes[]) {
    node->m_is_split = true;
    for (int i = 0; i < num_quadrants; i++) {
      node->m_quadrant[i] = new SpatialTree::SpatialTreeNode(num_quadrants);
      node->m_quadrant[i]->m_bbox = quadrant_bboxes[i];
    }
  }
  
  void split_bbox(SpatialTree::BBoxT &bbox, int num_quadrants, SpatialTree::BBoxT quadrant_bboxes[]) {
    using namespace ::vw::math::vector_containment_comparison;
    SpatialTree::VectorT center = bbox.center();
    SpatialTree::VectorT diagonal_vec = bbox.size() * 0.5;
    int dim = center.size();
    int i, j;
  
    for (i = 0; i < num_quadrants; i++)
      quadrant_bboxes[i].min() = center;
    for (int d = 0, width = num_quadrants, half_width = num_quadrants / 2; d < dim; d++, width = half_width, half_width /= 2) {
      assert(d < dim - 1 || width > 1);
      for (i = 0; i < num_quadrants;) {
        for (j = 0; j < half_width; j++, i++)
          quadrant_bboxes[i].min()[d] -= diagonal_vec[d];
        for (; j < width; j++, i++)
          quadrant_bboxes[i].min()[d] += diagonal_vec[d];
      }
    }
    for (i = 0; i < num_quadrants; i++) {
      quadrant_bboxes[i].max() = ::vw::math::vector_containment_comparison::max(center, quadrant_bboxes[i].min());
      quadrant_bboxes[i].min() = ::vw::math::vector_containment_comparison::min(center, quadrant_bboxes[i].min());
    }
  
    // Debugging:
    if (num_quadrants == 4) {
      assert(quadrant_bboxes[0].max() >= quadrant_bboxes[0].min());
      assert(quadrant_bboxes[1].max() >= quadrant_bboxes[1].min());
      assert(quadrant_bboxes[2].max() >= quadrant_bboxes[2].min());
      assert(quadrant_bboxes[3].max() >= quadrant_bboxes[3].min());
      assert(quadrant_bboxes[3].min() >= quadrant_bboxes[0].min());
      assert(quadrant_bboxes[3].max() >= quadrant_bboxes[0].max());
      assert(quadrant_bboxes[0].max() == quadrant_bboxes[3].min());
    }
  }
  
  void grow_bbox(SpatialTree::BBoxT &bbox, int num_primitives, GeomPrimitive *prims[]) {
    cout << "SpatialTree: growing BBox..." << flush;
    for (int i = 0; i < num_primitives; i++)
      bbox.grow(prims[i]->bounding_box());
    cout << "done." << endl;
  }
  
  void grow_bbox_on_grid(SpatialTree::BBoxT &bbox, const SpatialTree::BBoxT &new_bbox) {
    using namespace ::vw::math::vector_containment_comparison;
    double d, f;
    unsigned p = 1, p_;
    int i;
    int dim = bbox.min().size();
    SpatialTree::VectorT diag, super_diag;
    SpatialTree::VectorT extra, extra_needed(dim);
    SpatialTree::BBoxT super_bbox = bbox;
    super_bbox.grow(new_bbox);
    super_diag = super_bbox.size();
    diag = bbox.size();
  
    // find minimum p such that enlarging bbox by a factor of 2^p in each dimension
    //   will make it at least as large as super_bbox in each dimension
    for (i = 0; i < dim; i++) {
      f = super_diag[i] / diag[i];
      if (f > 1) {
        p_ = (unsigned)ceil(log(f) / LOG_2);
        p = max(p, p_);
      }
    }
    
    // shift super_bbox to fit bbox onto the grid, increasing p if necessary
    for (; 1; p++) {
      f = (double)((unsigned)1 << p);
      // extra needed in each dimension to make super_bbox 2^p times larger than bbox
      //   in each dimension
      extra = f * diag - super_diag;
      // determine how much we need to decrease super_bbox.min() in each dimension so that
      //   bbox fits on the grid
      for (i = 0; i < dim; i++) {
        d = bbox.min()[i] - super_bbox.min()[i];
        extra_needed[i] = ceil(d / diag[i]) * diag[i] - d;
      }
      // do we have this much extra?
      if (extra_needed <= extra)
        break;
    }
    super_bbox.min() -= extra_needed;
    super_bbox.max() += (extra - extra_needed);
  
    // return super_bbox in bbox
    bbox = super_bbox;
  }
  
  struct ApplyState {
    ApplyState(int num_quadrants_)
      : num_quadrants(num_quadrants_), tree_node(0), list_elem(0), queued_children(false), level(0) {}
    ApplyState(int num_quadrants_, SpatialTree::SpatialTreeNode *tree_node_)
      : num_quadrants(num_quadrants_), tree_node(tree_node_), list_elem(0), queued_children(false), level(0) {}
    ApplyState(const ApplyState &state)
      : num_quadrants(state.num_quadrants), tree_node(state.tree_node), list_elem(state.list_elem), queued_children(state.queued_children), level(state.level) {}
    int num_quadrants;
    SpatialTree::SpatialTreeNode *tree_node;
    SpatialTree::PrimitiveListElem *list_elem;
    bool queued_children;
    unsigned int level;
  };
  
  enum ApplySearchType {
    SEARCH_DFS,
    SEARCH_BFS
  };
  
  enum ApplyProcessOrder {
    PROCESS_CHILDREN_BEFORE_CURRENT_NODE,
    PROCESS_CURRENT_NODE_BEFORE_CHILDREN,
    PROCESS_CHILDREN_ONLY,
    PROCESS_CURRENT_NODE_ONLY,
    PROCESS_ONE_CHILD_ONLY
  };
  
  class ApplyFunctor {
  public:
    virtual ~ApplyFunctor() {}
    virtual ApplySearchType search_type() const {return SEARCH_DFS;}
    virtual ApplyProcessOrder process_order() const {return PROCESS_CURRENT_NODE_BEFORE_CHILDREN;}
    virtual bool should_process(const ApplyState &state) {return true;}
    virtual bool operator()(const ApplyState &state) {return true;}
    virtual void finished_processing(const ApplyState &state) {}
  };
  
  void
  apply_children(ApplyState &s, deque<ApplyState> &q) {
    ApplyState s2(s.num_quadrants);
    int i;
    
    if (s.tree_node->is_split()) {
      //s2.tree_node set below
      s2.list_elem = 0;
      s2.queued_children = false;
      s2.level = s.level + 1;
      for (i = 0; i < s.num_quadrants; i++) {
        s2.tree_node = s.tree_node->m_quadrant[i];
        q.push_back(s2);
      }
    }
  }
  
  void
  apply_one_child(ApplyFunctor &func, ApplyState &s, deque<ApplyState> &q) {
    ApplyState s2(s.num_quadrants);
    int i;
    
    if (s.tree_node->is_split()) {
      //s2.tree_node set below
      s2.list_elem = 0;
      s2.queued_children = false;
      s2.level = s.level + 1;
      for (i = 0; i < s.num_quadrants; i++) {
        s2.tree_node = s.tree_node->m_quadrant[i];
        if (func.should_process(s2))
          q.push_back(s2);
      }
    }
  }
  
  bool
  apply_current_node(ApplyFunctor &func, ApplyState &s) {
    SpatialTree::PrimitiveListElem *next;
    
    if (!s.list_elem)
      s.list_elem = s.tree_node->m_primitive_list;
    while (s.list_elem != 0) {
      next = s.list_elem->next;
      if (!func(s))
        return false; // do not continue processing
      s.list_elem = next;
    }
    return true; // continue processing
  }
  
  void
  apply(ApplyFunctor &func, const ApplyState &state) {
    deque<ApplyState> q;
    ApplyState s(state.num_quadrants);
    
    q.push_back(state);
    
    while (!q.empty()) {
      s = func.search_type() == SEARCH_DFS ? q.back() : q.front();
      if (func.search_type() == SEARCH_DFS)
        q.pop_back();
      else
        q.pop_front();
    
      if (func.should_process(s)) {
        switch (func.process_order()) {
        case PROCESS_CHILDREN_BEFORE_CURRENT_NODE:
          if (!s.queued_children) {
            s.queued_children = true;
            if (func.search_type() == SEARCH_DFS)
              q.push_back(s);
            apply_children(s, q);
            if (func.search_type() != SEARCH_DFS)
              q.push_back(s);
          }
          else {
            if (!apply_current_node(func, s))
              return; // do not continue processing
            func.finished_processing(s);
          }
          break;
        case PROCESS_CURRENT_NODE_BEFORE_CHILDREN:
          if (!apply_current_node(func, s))
            return; // do not continue processing
          s.queued_children = true;
          apply_children(s, q);
          func.finished_processing(s);
          break;
        case PROCESS_CHILDREN_ONLY:
          s.queued_children = true;
          apply_children(s, q);
          func.finished_processing(s);
          break;
        case PROCESS_CURRENT_NODE_ONLY:
          if (!apply_current_node(func, s))
            return; // do not continue processing
          func.finished_processing(s);
          break;
        case PROCESS_ONE_CHILD_ONLY:
          s.queued_children = true;
          apply_one_child(func, s, q);
          func.finished_processing(s);
          break;
        default:
          break;
        }
      }
    }
  }
  
  void
  apply(ApplyFunctor &func, int num_quadrants, SpatialTree::SpatialTreeNode *root_node) {
    ApplyState state(num_quadrants, root_node);
    apply(func, state);
  }
  
  class PrintFunctor : public ApplyFunctor {
  public:
    PrintFunctor(ostream &os) : m_os(os) {}
    virtual bool should_process(const ApplyState &state) {
      process_bbox(state.tree_node->bounding_box(), state.level, "+ ");
      return true; // process box
    }
    virtual bool operator()(const ApplyState &state) {
      process_bbox(state.list_elem->prim->bounding_box(), state.level + 1, "");
      return true; // continue processing
    }
  private:
    void process_bbox(const SpatialTree::BBoxT &bbox, int level, string prefix) const {
      for (int i = 0; i < level; i++)
        m_os << "  ";
      m_os << prefix;
      m_os << "Min[" << bbox.min() << "] "
          << "Max[" << bbox.max() << "]"
          << endl;    
    }
    ostream &m_os;
  };
  
  //NOTE: this can only write a 2D projection (because VRML is 3D)
  class VRMLFunctor : public ApplyFunctor {
  public:
    VRMLFunctor(ostream &os) : m_os(os) {
      m_selected_level = -1;
      m_z_spacing = 0.5;
      m_color[0] = 1; m_color[1] = m_color[2] = 0;
    }
    void select_level(int level) { m_selected_level = level; }
    void z_spacing(float spacing) { m_z_spacing = spacing; }
    void color(float red, float green, float blue) {
      m_color[0] = red; m_color[1] = green; m_color[2] = blue;
    }
    virtual bool should_process(const ApplyState &state) {
      process_bbox(state.tree_node->bounding_box(), state.level, 0.5);
      return true; // process box
    }
    virtual bool operator()(const ApplyState &state) {
      process_bbox(state.list_elem->prim->bounding_box(), state.level, 1.0);
      return true; // continue processing
    }
  private:
    void process_bbox(const SpatialTree::BBoxT &bbox, int level, float color_scale) const {
      if (!m_os.fail()) {
        if ((m_selected_level != -1) && (level != m_selected_level))
          return;
      
        float colors[7][3] = {{ 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 },
                              { 0.0, 0.0, 1.0 }, { 1.0, 0.0, 1.0 },
                              { 0.0, 1.0, 1.0 }, { 1.0, 1.0, 0.0 },
                              { 1.0, 1.0, 1.0 }};
        static const int num_colors = 7; // r,g,b, m,c,y, w
        int index = (level % num_colors);
        float z = -level * m_z_spacing;
        m_os << "    Shape {" << endl;
        m_os << "      appearance Appearance {" << endl;
        m_os << "        material Material {" << endl;
        m_os << "          emissiveColor " << (colors[index][0] * color_scale) << " " << (colors[index][1] * color_scale) << " " << (colors[index][2] * color_scale) << endl;
        m_os << "        }" << endl;
        m_os << "      }" << endl;
        m_os << "      geometry IndexedLineSet {" << endl;
        m_os << "        coord Coordinate {" << endl;
        m_os << "          point [" << endl;
        m_os << "            " << bbox.min()[0] << " " << bbox.min()[1] << " " << z << "," << endl;
        m_os << "            " << bbox.min()[0] << " " << bbox.max()[1] << " " << z << "," << endl;
        m_os << "            " << bbox.max()[0] << " " << bbox.max()[1] << " " << z << "," << endl;
        m_os << "            " << bbox.max()[0] << " " << bbox.min()[1] << " " << z << "," << endl;
        m_os << "          ]" << endl;
        m_os << "        }" << endl;
        m_os << "        coordIndex [ 0, 1, 2, 3, 0, -1, ]" << endl;
        m_os << "      }" << endl;
        m_os << "    }" << endl;
      }
      else {
        cout << "ERROR in VRMLFunctor: unable to write to output stream!" << endl;
      }
    }
  private:
    ostream &m_os;
    int m_selected_level;
    float m_z_spacing;
    float m_color[3];
  };
  
  class ContainsOneFunctor : public ApplyFunctor {
  public:
    ContainsOneFunctor(const SpatialTree::VectorT *point) : m_point(point), m_prim(0) {}
    virtual ApplyProcessOrder process_order() const {return PROCESS_CHILDREN_BEFORE_CURRENT_NODE;}
    virtual bool should_process(const ApplyState &state) {
      return state.tree_node->bounding_box().contains(*m_point);
    }
    virtual bool operator()(const ApplyState &state) {
      GeomPrimitive *prim = state.list_elem->prim;
      if (prim->bounding_box().contains(*m_point) && prim->contains(*m_point)) {
        m_prim = prim;
        return false; // stop processing
      }
      return true; // continue processing
    }
    GeomPrimitive *get_primitive() {return m_prim;}
  private:
    const SpatialTree::VectorT *m_point;
    GeomPrimitive *m_prim;
  };
  
  class ContainsAllFunctor : public ApplyFunctor {
  public:
    ContainsAllFunctor(const SpatialTree::VectorT *point) : m_point(point), m_alloc(true) {
      m_prims = new list<GeomPrimitive*>;
    }
    ContainsAllFunctor(const SpatialTree::VectorT *point, list<GeomPrimitive*> *prims)
      : m_point(point), m_alloc(false), m_prims(prims) {}
    virtual ~ContainsAllFunctor() {
      if (m_alloc)
        delete m_prims;
    }
    virtual ApplyProcessOrder process_order() const {return PROCESS_CHILDREN_BEFORE_CURRENT_NODE;}
    virtual bool should_process(const ApplyState &state) {
      return state.tree_node->bounding_box().contains(*m_point);
    }
    virtual bool operator()(const ApplyState &state) {
      GeomPrimitive *prim = state.list_elem->prim;
      if (prim->bounding_box().contains(*m_point) && prim->contains(*m_point))
        m_prims->push_back(prim);
      return true; // continue processing
    }
    list<GeomPrimitive*> *get_primitives() {return m_prims;}
  private:
    const SpatialTree::VectorT *m_point;
    bool m_alloc;
    list<GeomPrimitive*> *m_prims;
  };
  
  class AllOverlapsFunctor : public ApplyFunctor {
  public:
    AllOverlapsFunctor(GeomPrimitive *overlap_prim) : m_overlap_prim(overlap_prim), m_alloc(true) {
      m_overlaps = new list<pair<GeomPrimitive*, GeomPrimitive*> >;
    }
    AllOverlapsFunctor(GeomPrimitive *overlap_prim,
                      list<pair<GeomPrimitive*, GeomPrimitive*> > *overlaps)
                        : m_overlap_prim(overlap_prim), m_alloc(false), m_overlaps(overlaps) {}
    virtual ~AllOverlapsFunctor() {
      if (m_alloc)
        delete m_overlaps;
    }
    virtual ApplyProcessOrder process_order() const {return PROCESS_CURRENT_NODE_BEFORE_CHILDREN;}
    virtual bool should_process(const ApplyState &state) {
      return state.tree_node->bounding_box().intersects(m_overlap_prim->bounding_box());
    }
    virtual bool operator()(const ApplyState &state) {
      GeomPrimitive *prim = state.list_elem->prim;
      if (prim != m_overlap_prim && prim->bounding_box().intersects(m_overlap_prim->bounding_box()))
        m_overlaps->push_back(make_pair(m_overlap_prim, prim));
      return true; // continue processing
    }
    list<pair<GeomPrimitive*, GeomPrimitive*> > *overlaps() {return m_overlaps;}
  private:
    GeomPrimitive *m_overlap_prim;
    bool m_alloc;
    list<pair<GeomPrimitive*, GeomPrimitive*> > *m_overlaps;
  };
  
  class OverlapPairsFunctor : public ApplyFunctor {
  public:
    OverlapPairsFunctor() : m_alloc(true) {
      m_overlaps = new list<pair<GeomPrimitive*, GeomPrimitive*> >;
    }
    OverlapPairsFunctor(list<pair<GeomPrimitive*, GeomPrimitive*> > *overlaps)
                        : m_alloc(false), m_overlaps(overlaps) {}
    virtual ~OverlapPairsFunctor() {
      if (m_alloc)
        delete m_overlaps;
    }
    virtual ApplyProcessOrder process_order() const {return PROCESS_CURRENT_NODE_BEFORE_CHILDREN;}
    virtual bool operator()(const ApplyState &state) {
      AllOverlapsFunctor func(state.list_elem->prim, m_overlaps);
      //NOTE: we do not want to create a new state s with s.list_elem = state.list_elem->next
      //  because then, if s.list_elem == 0, apply() will go through the entire primitive list
      //  for the current node; instead, we rely on AllOverlapsFunctor to not add pairs where
      //  the two prims are the same (by pointer value)
      apply(func, state);
      return true; // continue processing
    }
    list<pair<GeomPrimitive*, GeomPrimitive*> > *overlaps() {return m_overlaps;}
  private:
    bool m_alloc;
    list<pair<GeomPrimitive*, GeomPrimitive*> > *m_overlaps;
  };
  
  class NodeContainsBBoxFunctor : public ApplyFunctor {
  public:
    NodeContainsBBoxFunctor(const SpatialTree::BBoxT &bbox, int num_quadrants, bool create_children = false)
      : m_bbox(bbox), m_create_children(create_children), m_tree_node(0) {}
    virtual ApplyProcessOrder process_order() const {return PROCESS_ONE_CHILD_ONLY;}
    virtual bool should_process(const ApplyState &state) {
      if (!state.tree_node->bounding_box().contains(m_bbox))
        return false; // do not process
      
      if (m_create_children && !state.tree_node->is_split()) {
        SpatialTree::BBoxT quadrant_bboxes[state.num_quadrants];
  
        split_bbox(state.tree_node->bounding_box(), state.num_quadrants, quadrant_bboxes);
        // Check to see if new bbox would fit in a child quad... if so create
        // the quadrants
        for (int i = 0; i < state.num_quadrants; i++) {
          if (quadrant_bboxes[i].contains(m_bbox)) {
            // we need to make quads...
            split_node(state.tree_node, state.num_quadrants, quadrant_bboxes);
            break;
          }
        }
      }
  
      m_tree_node = state.tree_node;
  
      return true; // process node
    }
    SpatialTree::SpatialTreeNode *tree_node() {return m_tree_node;}
  private:
    SpatialTree::BBoxT m_bbox;
    bool m_create_children;
    SpatialTree::SpatialTreeNode *m_tree_node;
  };
  
  class CleanupFunctor : public ApplyFunctor {
  public:
    CleanupFunctor() {}
    virtual ApplyProcessOrder process_order() const {return PROCESS_CHILDREN_BEFORE_CURRENT_NODE;}
    virtual bool operator()(const ApplyState &state) {
      delete state.list_elem;
      return true;
    }
    virtual void finished_processing(const ApplyState &state) {
      for (int i = 0; i < state.num_quadrants; i++)
        state.tree_node->m_quadrant[i] = 0;
      state.tree_node->m_primitive_list = 0;
      state.tree_node->m_num_primitives = 0; // mostly for debugging
      state.tree_node->m_is_split = false;
      delete state.tree_node;
    }
  };

} // namespace

namespace vw {
namespace math {

  GeomPrimitive *
  SpatialTree::contains(const VectorT &point) {
    ContainsOneFunctor func(&point);
    apply(func, m_num_quadrants, m_root_node);
    return func.get_primitive();
  }
  
  void
  SpatialTree::contains(const VectorT &point, list<GeomPrimitive*> &prims) {
    ContainsAllFunctor func(&point, &prims);
    apply(func, m_num_quadrants, m_root_node);
  }
  
  void
  SpatialTree::overlap_pairs(list<pair<GeomPrimitive*, GeomPrimitive*> > &overlaps) {
    OverlapPairsFunctor func(&overlaps);
    apply(func, m_num_quadrants, m_root_node);
  }
  
  void
  SpatialTree::print(ostream &os /*= cout*/) {
    PrintFunctor func(os);
    apply(func, m_num_quadrants, m_root_node);
  }
  
  void
  SpatialTree::write_vrml(char *fn, int level) {
    ofstream os(fn);
  
    if (os.fail()) {
      fprintf(stderr, "write_vrml(): cannot open output file: %s\n", fn);
      exit(-1);
    }
  
    write_vrml(os, level);
  
    os.close();
  }
  
  void
  SpatialTree::write_vrml(ostream &os, int level) {
    assert(m_num_quadrants >= 4);
    VectorT center = bounding_box().center();
  
    os << "#VRML V2.0 utf8" << endl << "#" << endl;
    os << "Transform {" << endl;
    os << "  translation " << -center[0] << " " << -center[1] << " 0" << endl;
    os << "  children [" << endl;
  
    VRMLFunctor func(os);
    func.select_level(level);
    apply(func, m_num_quadrants, m_root_node);
  
    os << "  ]" << endl;
    os << "}" << endl;
  }
  
  void
  SpatialTree::add(GeomPrimitive *prim) {
    SpatialTreeNode *tree_node;
    PrimitiveListElem *new_list_elem;
  
    // expand size of entire spatial tree, if necessary
    if (!m_root_node->bounding_box().contains(prim->bounding_box())) {
      int i;
  
      // create tree structure above the current root
      BBoxT bbox = m_root_node->bounding_box(), super_bbox = m_root_node->bounding_box();
      grow_bbox_on_grid(super_bbox, prim->bounding_box());
      NodeContainsBBoxFunctor func2(bbox, m_num_quadrants, true);
      SpatialTreeNode *root_node = new SpatialTreeNode(m_num_quadrants);
      root_node->m_bbox = super_bbox;
      ApplyState state(m_num_quadrants, root_node);
      apply(func2, state);
      tree_node = func2.tree_node();
  
      // at this point we either have the node that is equivalent to the
      //   old root node, or its parent (due to numerical issues)
      double f = prod(tree_node->bounding_box().size()) / prod(m_root_node->bounding_box().size());
      if (f > (m_num_quadrants - 1) / 2) {
        // we have the parent (volume is m_num_quadrants times greater)
        BBoxT quadrant_bboxes[m_num_quadrants];
        split_bbox(tree_node->bounding_box(), m_num_quadrants, quadrant_bboxes);
        split_node(tree_node, m_num_quadrants, quadrant_bboxes);
        for (i = 0; i < m_num_quadrants; i++) {
          if (tree_node->m_quadrant[i]->bounding_box().contains(bbox.center())) {
            tree_node = tree_node->m_quadrant[i];
            break;
          }
        }
      }
  
      // tree_node holds the node that is equivalent to the old root node;
      //   transfer everything from the old root node to tree_node
      assert(!tree_node->is_split() && tree_node->m_num_primitives == 0);
      for (i = 0; i < m_num_quadrants; i++) {
        tree_node->m_quadrant[i] = m_root_node->m_quadrant[i];
        m_root_node->m_quadrant[i] = 0;
      }
      tree_node->m_is_split = m_root_node->m_is_split;
      m_root_node->m_is_split = false;
      tree_node->m_primitive_list = m_root_node->m_primitive_list;
      m_root_node->m_primitive_list = 0;
      tree_node->m_num_primitives = m_root_node->m_num_primitives;
      m_root_node->m_num_primitives = 0;
  
      // replace the root node
      delete m_root_node;
      m_root_node = root_node;
    }
  
    // find the smallest spatial tree node containing the primitive's bbox
    NodeContainsBBoxFunctor func(prim->bounding_box(), m_num_quadrants, true);
    apply(func, m_num_quadrants, m_root_node);
    tree_node = func.tree_node();
    assert(tree_node);
  
    // add primitive to this spatial tree node
    new_list_elem = new PrimitiveListElem;
    new_list_elem->prim = prim;
    new_list_elem->next = tree_node->m_primitive_list;
    tree_node->m_primitive_list = new_list_elem;
    tree_node->m_num_primitives++;
  }
  
  SpatialTree::SpatialTree(BBoxT bbox) {
    m_dim = bbox.min().size();
    m_num_quadrants = (int)((unsigned)1 << (unsigned)m_dim);
    m_root_node = new SpatialTreeNode(m_num_quadrants);
    m_root_node->m_bbox = bbox;
  }
  
  SpatialTree::SpatialTree(int num_primitives, GeomPrimitive **prims) {
    VW_ASSERT( prims != 0 && prims[0] != 0, ArgumentErr() << "No GeomPrimitives provided." );
    m_dim = prims[0]->bounding_box().min().size();
    m_num_quadrants = (int)((unsigned)1 << (unsigned)m_dim);
    m_root_node = new SpatialTreeNode(m_num_quadrants);
    grow_bbox(m_root_node->m_bbox, num_primitives, prims);
  
    cout << "Bounding Box of Total mesh ="
          << " Min[" << m_root_node->m_bbox.min() << "]"
          << " Max[" << m_root_node->m_bbox.max() << "]"
          << endl;
  
    for (int i = 0; i < num_primitives; i++)
      add(prims[i]);
  }
  
  SpatialTree::~SpatialTree() {
    CleanupFunctor func;
    apply(func, m_num_quadrants, m_root_node);
    m_root_node = 0;
  }

}} // namespace vw::math
