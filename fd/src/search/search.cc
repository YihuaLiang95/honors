
#include "search.h"

#include "globals.h"
#include "option_parser.h"
#include "state.h"
#include "operator.h"
#include "heuristic.h"
#include "state_registry.h"
#include "successor_generator.h"
#include "plugin.h"
#include "heuristic_refiner.h"
#include "timer.h"

#include "open_sets/open_set.h"
#include "open_sets/dfs_open_set.h"
#include "open_sets/v_open_set.h"

#include "pruning_method.h"

#include <iostream>
#include <vector>
#include <unordered_set>
#include <cstdio>
#include <set>

#include <cassert>

#include <fstream>

#include <numeric>

Search::Search(const Options &opts)
  : SearchEngine(opts),
    search_space(cost_type),
    c_reopen_nodes(opts.get<bool>("reopen")),
    c_unsath_new_full(opts.get<int>("u_new") == 2),
    c_unsath_open_full(opts.get<int>("u_open") == 2),
    c_unsath_closed_full(opts.get<int>("u_closed") == 2),
    c_unsath_bprop_full(opts.get<int>("u_bprop") == 2),
    c_unsath_new(opts.get<int>("u_new") > 0),
    c_unsath_open(opts.get<int>("u_open") > 0),
    c_unsath_closed(opts.get<int>("u_closed") > 0),
    c_unsath_bprop(opts.get<int>("u_bprop") > 0),
    c_unsath_search(opts.get<bool>("u_search")),
    c_unsath_use_plan(opts.get<bool>("u_use_plan")),
    c_unsath_refine_to_initial_state(opts.get<bool>("u_refine_initial_state")),
    c_ensure_u_consistency(opts.get<bool>("u_consistency")),
    m_unsath_refine(opts.get<bool>("u_refine")),
    m_pruning_method(opts.get<PruningMethod*>("pruning")),
    m_open_set(opts.get<OpenSet *>("open_set")),
    m_heuristics(opts.get_list<Heuristic *>("eval")),
    m_preferred(opts.contains("preferred") ? opts.get_list<Heuristic *>("preferred")
                : std::vector<Heuristic *>()),
    m_heuristic_refiner(opts.get_list<HeuristicRefiner *>("refiner")),
    m_cached_h(NULL),
    m_revision(1),
    m_open_states(0),
    m_next_states_print(1),
    m_next_state_print_multiplyer(2),
    m_smallest_h(std::numeric_limits<int>::max()),
    m_largest_g(0)
{
  c_unsath_compute_recognized_neighbors = false;
  if (m_heuristic_refiner.empty()) {
    m_unsath_refine = false;
  } else {
    for (uint i = 0; i < m_heuristic_refiner.size(); i++) {
      c_unsath_compute_recognized_neighbors |=
        m_heuristic_refiner[i]->dead_end_learning_requires_recognized_neighbors();
    }
  }
  cout<<"Bound is"<<bound<<endl;
  m_solved = false;
}

void Search::progress_information()
{
  if (g_state_registry->size() >= m_next_states_print) {
    m_next_states_print = m_next_states_print * m_next_state_print_multiplyer;
    printf("[g = %d, h = %d, registered=%zu, expanded=%d, open_states=%zu, dead_ends=%d, t=%.3f]\n",
           m_largest_g,
           m_smallest_h,
           g_state_registry->size(),
           search_progress.get_expanded(),
           m_open_states,
           search_progress.get_dead_ends(),
           g_timer());
  }
}

void Search::initialize()
{
  m_next_states_print = 1;
  m_smallest_h = std::numeric_limits<int>::max();
  m_largest_g = 0;

  for (uint i = 0; i < m_heuristic_refiner.size(); i++) {
    m_underlying_heuristics.insert(m_heuristic_refiner[i]->get_heuristic());
  }

  // TODO kind of hacky...
  // (have to initialize the heuristics)
  for (Heuristic *h : m_underlying_heuristics) {
    h->evaluate(g_initial_state());
  }
  for (HeuristicRefiner *r : m_heuristic_refiner) {
    r->refine_offline();
  }

  State init = g_initial_state();
  bool urec = false;
  if (evaluate(init, urec)) {
    std::cout << "Solved in initial state!" << std::endl;
    m_solved = true;
  } else if (!m_cached_h || !m_cached_h->is_dead_end()) {
    if (m_cached_h) {
      search_progress.add_heuristic(m_cached_h);
      search_progress.get_initial_h_values();
    }
    SearchNode node = search_space.get_node(init);
    node.open_initial(m_cached_h ? m_cached_h->get_value() : 0);
    //m_open_set->push(node, false);
    m_open_set->push(node, false);
    m_open_states++;
  } else {
    search_progress.inc_dead_ends();
    if (urec) {
      search_progress.inc_u_recognized_dead_ends();
    }
    std::cout << "Initial state is a dead end!" << std::endl;
  }
}

bool Search::check_goal_and_set_plan(const State &state)
{
  if (test_goal(state)) {
    std::cout << "Solution found!" << std::endl;
    Plan plan;
    search_space.trace_path(state, plan);
    set_plan(plan);
    return true;
  }
  return false;
}

bool Search::trigger_refiner(const State &state, bool &success)
{
  unsigned tmp;
  success = false;
  for (uint i = 0; i < m_heuristic_refiner.size(); i++) {
    if (m_heuristic_refiner[i]->get_heuristic()->is_dead_end()) {
      success = true;
    } else {
      if (!m_heuristic_refiner[i]->dead_end_learning_requires_full_component()
          && !m_heuristic_refiner[i]->dead_end_learning_requires_recognized_neighbors()) {
        tmp = m_heuristic_refiner[i]->learn_unrecognized_dead_end(state);
        if (tmp == HeuristicRefiner::SOLVED && c_unsath_use_plan) {
          Plan plan;
          search_space.trace_path(state, plan);
          m_heuristic_refiner[i]->get_partial_plan(plan);
          set_plan(plan);
          return true;
        } else if (tmp == HeuristicRefiner::SUCCESSFUL) {
          success = true;
        }
      }
    }
  }
  return false;
}

Heuristic *Search::check_dead_end(const State &state, bool full)
{
  int maxh = 0;
  Heuristic *res = NULL;
  for (std::set<Heuristic *>::iterator it = m_underlying_heuristics.begin();
       it != m_underlying_heuristics.end(); it++) {
    Heuristic *h = *it;
    assert(h != NULL);
    if (full) {
      h->evaluate(state);
    } else {
      h->reevaluate(state);
    }
    if (h->is_dead_end()) {
      maxh = -1;
      res = h;
      if (!m_unsath_refine) {
        break;
      }
    } else if (maxh >= 0 && h->get_value() > maxh)  {
      maxh = h->get_value();
      res = h;
    }
  }
  return res;
}

bool Search::evaluate(SearchNode &node)
{
  bool u;
  bool res = evaluate(node.get_state(), u);
  if (u) {
    node.set_u_flag();
    search_progress.inc_u_recognized_dead_ends();
  }
  return res;
}


bool Search::evaluate(const State &state, bool &u)
{
  search_progress.inc_evaluated_states(1);
  u = false;
  m_cached_h = NULL;
  int maxh = 0;

  if (c_unsath_new) {
    Heuristic *x = check_dead_end(state, c_unsath_new_full);
    if (x && x->is_dead_end()) {
      maxh = -1;
      m_cached_h = x;
      u = true;
    } else if (c_unsath_search && x && x->get_value() > maxh) {
      maxh = x->get_value();
      m_cached_h = x;
    }
  }

  if (maxh != -1) {
    for (uint i = 0; i < m_heuristics.size(); i++) {
      search_progress.inc_evaluations(1);
      m_heuristics[i]->evaluate(state);
      if (m_heuristics[i]->is_dead_end()) {
        maxh = -1;
        m_cached_h = m_heuristics[i];
        break;
      }
      if (m_heuristics[i]->get_value() > maxh) {
        maxh = m_heuristics[i]->get_value();
        m_cached_h = m_heuristics[i];
      }
    }
  }

  if (maxh == -1) {
    if (m_unsath_refine) {
      check_dead_end(state, true);
      return trigger_refiner(state, u); 
    } else if (c_ensure_u_consistency && !c_unsath_new) {
      Heuristic *x = check_dead_end(state, c_unsath_new_full);
      if (x && x->is_dead_end()) {
        u = true;
      } 
    }
  }

  return false;
}

void Search::get_preferred_operators(const State &state,
                                     std::set<const Operator *> &result)
{
  if (m_preferred.size() > 0) {
    search_progress.inc_evaluations(m_preferred.size());
    std::vector<const Operator *> ops;
    for (uint i = 0; i < m_preferred.size(); i++) {
      m_preferred[i]->evaluate(state);
      m_preferred[i]->get_preferred_operators(ops);
    }
    if (ops.size() > 0) {
      result.insert(ops.begin(), ops.end());
    }
    ops.clear();
  }
}

void Search::statistics() const
{
  for (uint i = 0; i < m_heuristic_refiner.size(); i++) {
    m_heuristic_refiner[i]->statistics();
  }
  m_pruning_method->print_statistics();
  printf("Open states in open list: %zu state(s).\n", m_open_states);
  search_progress.print_statistics();
  printf("Registered: %zu state(s).\n", g_state_registry->size());
  //dynamic_cast<UCHeuristic*>(m_heuristic_refiner[0]->get_heuristic())->dump_conjunctions_to_file("conjunctions.txt");
}

StateID Search::fetch_next_state()
{
  while (!m_open_set->empty()) {
    StateID m_next_state_id = m_open_set->pop();
    State state = g_state_registry->lookup_state(m_next_state_id);
    SearchNode node = search_space.get_node(state);

    if (node.is_closed() || node.is_dead_end()) {
      continue;
    }

    return state.get_id();
  }
  return StateID::no_state;
}

Search::SearchStatus Search::step()
{
  if (m_solved) {
    return SOLVED;
  }

  StateID m_next_state_id = fetch_next_state();

  if (m_next_state_id == StateID::no_state) {
    std::cout << "Completely explored state space!" << std::endl;
#ifndef NDEBUG
    if (m_unsath_refine && c_unsath_refine_to_initial_state) {
      assert(check_dead_end(g_initial_state(), true));
    }
#endif
    return FAILED;
  }

  bool whatever;
  State state = g_state_registry->lookup_state(m_next_state_id);
  SearchNode node = search_space.get_node(state);
  assert(!node.is_closed() && !node.is_dead_end());
  m_next_state_id = StateID::no_state;

  if (check_goal_and_set_plan(state)) {
    return SOLVED;
  }

  m_open_states--;
  m_revision++;

  if (c_unsath_open) {
    Heuristic *h = check_dead_end(state, c_unsath_open_full);
    if (h && h->is_dead_end()) {
      node.mark_as_dead_end();
      node.set_u_flag();
      search_progress.inc_u_recognized_dead_ends();
      if (m_unsath_refine && (c_unsath_refine_to_initial_state
                              || m_open_states > 0)) {
        trigger_refiner(state, whatever);
        backward_propagation(state);
      }
      search_progress.inc_dead_ends(1);
      return IN_PROGRESS;
    }
  }

  search_progress.inc_expanded(1);
  node.close();

  m_smallest_h = m_smallest_h < node.get_h() ? m_smallest_h : node.get_h();
  m_largest_g = m_largest_g > node.get_g() ? m_largest_g : node.get_g();
  progress_information();

  std::vector<const Operator *> ops;
  g_successor_generator->generate_applicable_ops(state, ops);
  m_pruning_method->prune_operators(state, ops);
  std::set<const Operator *> preferred_ops;
  get_preferred_operators(state, preferred_ops);

  bool open_child = false;
  std::vector<StateID> &successors = node.get_all_successors();
  successors.resize(ops.size(), StateID::no_state);
  search_progress.inc_generated(ops.size());
  for (uint i = 0; i < ops.size(); i++) {
    State succ = g_state_registry->get_successor_state(state, *ops[i]);
    SearchNode succ_node = search_space.get_node(succ);
    successors[i] = succ_node.get_state_id();
    succ_node.add_parent(state);
    if (succ_node.is_new()) {
      if (evaluate(succ_node)) {
        return SOLVED;
      }
      if (m_cached_h && m_cached_h->is_dead_end()) {
        search_progress.inc_dead_ends(1);
        succ_node.mark_as_dead_end();
        continue;
      } else {
        succ_node.open(m_cached_h ? m_cached_h->get_value() : 0, node, ops[i]);
        m_open_states++;
      }
    } else {
      int newg = node.get_g() + get_adjusted_cost(*ops[i]);
      if (c_reopen_nodes && m_open_set->reopen(succ_node, node, newg)) {
        succ_node.reopen(node, ops[i]);
        m_open_states++;
      } else if (newg < node.get_g()) {
        bool closed = succ_node.is_closed();
        succ_node.reopen(node, ops[i]);
        if (closed)
        succ_node.close();
      }
    }

    if (succ_node.is_open()) {
      assert(m_open_states > 0);
      open_child = true;
      //succ_node.set_depth(node.get_depth() + 1);
      m_open_set->push(succ_node, preferred_ops.count(ops[i]));
    }
  }

  if (!open_child && (c_unsath_refine_to_initial_state || m_open_states > 0)) {
    check_and_learn_dead_end(node);
  }

  return IN_PROGRESS;
}

bool Search::check_and_learn_dead_end(const SearchNode &node)
{
  if (!m_unsath_refine) {
    return false;
  }
  std::vector<State> component;
  bool res = check_and_learn_dead_end(node, component);
  backward_propagation(component);
  return res;
}

bool Search::check_and_learn_dead_end(const SearchNode &node,
                                      std::vector<State> &tbh)
{
  if (!m_unsath_refine) {
    return false;
  }

  State node_state = node.get_state();

  // setup search for open node in search space
  std::vector<State> visited;
  std::unordered_set<StateID> recognized_neighbors;
  bool open_node_reachable = false;
  //dedet_statistics.start_open_node_search();
  open_node_reachable = search_open_state(node, visited, recognized_neighbors,
                                          tbh);
  //dedet_statistics.end_open_node_search(visited.size(), 0);

  bool is_dead_end = false;
  // if node is a dead end
  if (!open_node_reachable) {
#ifndef NDEBUG
    //std::cout << "Found unrecognized dead end!" << std::endl;
    //std::cout << "Compontent size: " << visited.size() << std::endl;
    //std::cout << "Neighborhood size: " << recognized_neighbors.size() << std::endl;

    for (const State & state : visited) {
      SearchNode node = search_space.get_node(state);
      assert(node.is_flagged());
    }
    if (c_unsath_compute_recognized_neighbors) {
      for (const State & state : visited) {
        SearchNode node = search_space.get_node(state);
        const std::vector<StateID> &succs = node.get_all_successors();
        for (uint i = 0; i < succs.size(); i++) {
          SearchNode succ_node = search_space.get_node(g_state_registry->lookup_state(
                                                                                      succs[i]));
          assert(succ_node.is_flagged() || recognized_neighbors.count(succs[i]));
        }
      }
    }
#endif

    for (std::set<Heuristic *>::iterator it = m_underlying_heuristics.begin();
         it != m_underlying_heuristics.end(); it++) {
      (*it)->clear_cache();
    }
    bool new_revision = false;
    for (uint i = 0; i < m_heuristic_refiner.size(); i++) {
      if (m_heuristic_refiner[i]->get_heuristic()->is_dead_end()) {
        is_dead_end = true;
      } else {
        HeuristicRefiner::RefinementResult refine = \
          m_heuristic_refiner[i]->learn_unrecognized_dead_ends(visited,
                                                               recognized_neighbors);
        new_revision = new_revision || refine == HeuristicRefiner::SUCCESSFUL;
        is_dead_end = is_dead_end 
            || refine == HeuristicRefiner::SUCCESSFUL
            || refine == HeuristicRefiner::UNCHANGED; 
      }
    }
    if (!is_dead_end) {
      std::cout << "Stopping u-Refinement ..." << std::endl;
      m_unsath_refine = false;
    } else if (new_revision) {
      m_revision++;
    }
#ifndef NDEBUG
    //std::cout << "Refinement "
    //          << (is_dead_end ? "was successful" : "failed")
    //          << std::endl;
#endif
  }

  recognized_neighbors.clear();

  if (is_dead_end) {
    tbh.reserve(tbh.size() + visited.size());
  }

  for (uint i = 0; i < visited.size(); i++) {
    SearchNode v_node = search_space.get_node(visited[i]);
    if (is_dead_end) {
      if (!v_node.is_dead_end()) {
        tbh.push_back(v_node.get_state());
        search_progress.inc_u_recognized_dead_ends();
      }
      v_node.mark_as_dead_end();
      v_node.set_u_flag();
    }
    v_node.set_flag(false);
  }

  visited.clear();

  if (is_dead_end) {
    search_progress.inc_backtracks();
  }

  return is_dead_end;
}

bool Search::search_open_state(SearchNode node,
                               std::vector<State> &closed_states,
                               std::unordered_set<StateID> &rn,
                               std::vector<State> &open_states)
{
  //if (node.get_revision() == m_revision) {
  //    return true;
  //}

  closed_states.push_back(node.get_state());
  node.set_revision(m_revision);
  node.set_flag();

  unsigned i = 0;
  while (i < closed_states.size()) {
    State state = closed_states[i++];
    SearchNode node = search_space.get_node(state);
    const std::vector<StateID> &successors = node.get_all_successors();
    for (unsigned j = 0; j < successors.size(); j++) {
      State succ = g_state_registry->lookup_state(successors[j]);
      SearchNode succ_node = search_space.get_node(succ);
      if (succ_node.is_flagged()) {
        continue;
      }
      if (succ_node.is_dead_end()) {
        if (c_ensure_u_consistency && !succ_node.is_u_flagged()) {
          return true;
        }
        if (c_unsath_compute_recognized_neighbors) {
          rn.insert(succ.get_id());
        }
        continue;
      }
      //if (succ_node.get_revision() == m_revision) {
      //    return true;
      //}
      if (c_unsath_closed) {
        Heuristic *h = check_dead_end(succ, c_unsath_closed_full);
        if (h && h->is_dead_end()) {
          mark_dead_ends(succ_node, open_states);
          rn.insert(succ.get_id());
          continue;
        }
      }
      if (succ_node.is_open()) {
        return true;
      }
      succ_node.set_revision(m_revision);
      succ_node.set_flag();
      closed_states.push_back(succ);
    }
  }

  return false;
}

// TODO try the following bw prop:
// run dfs in every state space
// while state is not marked as dead
// run open state search
// if open state is reachable -> stop
// if open state is not reachable
//  -> recur on parents
// if there is NO parent that has been identified as dead end, do refinement
// from the state
// otherwise, state must have been handled already

void Search::backward_propagation(const State &state)
{
  std::vector<State> component;
  component.push_back(state);
  backward_propagation(component);
}

void Search::backward_propagation(std::vector<State> &tbh)
{
  //for (unsigned i = 0; i < states.size(); i++) {
  //    backward_propagation(states[i]);
  //}
  unsigned i = 0;
  while (i < tbh.size()) {
    State state = tbh[i++];
    SearchNode node = search_space.get_node(state);
    const std::unordered_set<StateID> &parents = node.get_all_parents();
    for (std::unordered_set<StateID>::const_iterator it = parents.begin();
         (m_open_states > 0 || c_unsath_refine_to_initial_state)
           && it != parents.end(); it++) {
      State parent = g_state_registry->lookup_state(*it);
      SearchNode pnode = search_space.get_node(parent);
      if (!pnode.is_dead_end()) {
        // TODO rember some kind of revision of uC evaluation to speed up things here
        //if (node.get_revision() == m_revision) {
        //    continue;
        //}
        bool isdead = false;
        if (c_unsath_bprop) {
          Heuristic *h = check_dead_end(parent, c_unsath_bprop_full);
          if (h && h->is_dead_end()) {
            //std::vector<State> open;
#ifndef NDEBUG
            //size_t tmp_size = tbh.size();
#endif
            mark_dead_ends(node, tbh);
            //backward_propagation(open);
#ifndef NDEBUG
            //std::cout << "Recognized dead end during backward propagation." << std::endl;
            //std::cout << "Number of reachable open states: " << (tbh.size() - tmp_size) <<
            //  std::endl;
#endif
            //open.clear();
            isdead = true;
            //backward_propagation(parent);
            tbh.push_back(parent);
          }
        }
        if (!isdead) {
          check_and_learn_dead_end(pnode, tbh);
        }
      }
    }
  }

}

void Search::mark_dead_ends(SearchNode node, std::vector<State> &open)
{
  // TODO: when marking state dead end, clear parents and successor pointers
  // (before doing that have to do bw prop)
  if (!node.is_dead_end()) {
    search_progress.inc_u_recognized_dead_ends();
  }
  node.mark_as_dead_end();
  node.set_u_flag();
  std::vector<State> dead_ends;
  dead_ends.push_back(node.get_state());
  mark_dead_ends(dead_ends, open);
  for (uint i = 0; i < m_heuristic_refiner.size(); i++) {
    m_heuristic_refiner[i]->learn_recognized_dead_ends(dead_ends);
  }
  dead_ends.clear();
  m_revision++;
}

void Search::mark_dead_ends(std::vector<State> &dead_ends,
                            std::vector<State> &open)
{
  unsigned i = 0;
  while (i < dead_ends.size()) {
    State state = dead_ends[i++];
    SearchNode node = search_space.get_node(state);
    const std::vector<StateID> &successors = node.get_all_successors();
    for (uint j = 0; j < successors.size(); j++) {
      State succ = g_state_registry->lookup_state(successors[j]);
      SearchNode succ_node = search_space.get_node(succ);
      if (!node.is_dead_end()) {
        search_progress.inc_u_recognized_dead_ends();
      }
      if (succ_node.is_open()) {
        succ_node.mark_as_dead_end();
        succ_node.set_u_flag();
        open.push_back(succ);
        m_open_states--;
      }
      if (!succ_node.is_dead_end() || !succ_node.is_u_flagged()) {
        succ_node.mark_as_dead_end();
        succ_node.set_u_flag();
        dead_ends.push_back(succ);
      }
    }
  }
}

void Search::add_options_to_parser(OptionParser &parser)
{
  parser.add_list_option<Heuristic *>("eval", "", "[]");
  parser.add_list_option<HeuristicRefiner *>("refiner", "", "[]");
  parser.add_list_option<Heuristic *>("preferred", "", "", OptionFlags(false));
  parser.add_option<bool>("u_refine", "", "true");
  parser.add_option<bool>("u_refine_initial_state", "", "false");
  parser.add_option<int>("u_new", "", "0");
  parser.add_option<int>("u_open", "", "2");
  parser.add_option<int>("u_closed", "", "0");
  parser.add_option<int>("u_bprop", "", "2");
  parser.add_option<bool>("u_search", "", "false");
  parser.add_option<bool>("u_use_plan", "", "false");
  parser.add_option<bool>("reopen", "", "false");
  parser.add_option<bool>("useoss", "", "false");
  parser.add_option<bool>("u_consistency", "", "false");
  parser.add_option<PruningMethod*>("pruning", "", "null");
  SearchEngine::add_options_to_parser(parser);
}

//static SearchEngine *_parse(OptionParser &parser) {
//    Search::add_options_to_parser(parser);
//    Options opts = parser.parse();
//    if (!parser.dry_run()) {
//        return new Search(opts);
//    }
//    return NULL;
//}

static SearchEngine *_parse_dfs(OptionParser &parser)
{
  Search::add_options_to_parser(parser);
  dfs_open_set::add_options_to_parser(parser);
  Options opts = parser.parse();
  if (!parser.dry_run()) {
    opts.set<OpenSet *>("open_set", dfs_open_set::parse(opts));
    return new Search(opts);
  }
  return NULL;
}

static SearchEngine *_parse_astar(OptionParser &parser)
{
  Search::add_options_to_parser(parser);
  Options opts = parser.parse();
  if (!parser.dry_run()) {
    opts.set<OpenSet *>("open_set", new AstarOpenSet());
    return new Search(opts);
  }
  return NULL;
}

static SearchEngine *_parse_greedy(OptionParser &parser)
{
  Search::add_options_to_parser(parser);
  Options opts = parser.parse();
  if (!parser.dry_run()) {
    opts.set<OpenSet *>("open_set", new GreedyOpenSet());
    return new Search(opts);
  }
  return NULL;
}

static Plugin<SearchEngine> _plugin_dfs("dfs", _parse_dfs);
static Plugin<SearchEngine> _plugin_astar("astar", _parse_astar);
static Plugin<SearchEngine> _plugin_greedy("greedy", _parse_greedy);
