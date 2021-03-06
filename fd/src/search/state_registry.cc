#include "state_registry.h"

#include "axioms.h"
#include "operator.h"
#include "per_state_information.h"

using namespace std;

StateRegistry::StateRegistry()
  : state_data_pool(g_state_packer->get_num_bins()),
    registered_states(0,
                      StateIDSemanticHash(state_data_pool),
                      StateIDSemanticEqual(state_data_pool)),
    cached_initial_state(0) {
}


StateRegistry::~StateRegistry() {
  for (set<PerStateInformationBase *>::iterator it = subscribers.begin();
       it != subscribers.end(); ++it) {
    (*it)->remove_state_registry(this);
  }
  delete cached_initial_state;
}

StateID StateRegistry::insert_id_or_pop_state() {
  /*
    Attempt to insert a StateID for the last state of state_data_pool
    if none is present yet. If this fails (another entry for this state
    is present), we have to remove the duplicate entry from the
    state data pool.
  */
  StateID id(state_data_pool.size() - 1);
  pair<StateIDSet::iterator, bool> result = registered_states.insert(id);
  bool is_new_entry = result.second;
  if (!is_new_entry) {
    state_data_pool.pop_back();
  }
  assert(registered_states.size() == state_data_pool.size());
  return *result.first;
}

State StateRegistry::lookup_state(StateID id) const {
  assert(id.value < state_data_pool.size());
  return State(state_data_pool[id.value], *this, id);
}

const State &StateRegistry::get_initial_state() {
  if (cached_initial_state == 0) {
    PackedStateBin *buffer = new PackedStateBin[g_state_packer->get_num_bins()];
    for (size_t i = 0; i < g_initial_state_data.size(); ++i) {
      g_state_packer->set(buffer, i, g_initial_state_data[i]);
    }
    g_axiom_evaluator->evaluate(buffer);
    state_data_pool.push_back(buffer);
    // buffer is copied by push_back
    delete[] buffer;
    StateID id = insert_id_or_pop_state();
    cached_initial_state = new State(lookup_state(id));
  }
  return *cached_initial_state;
}

//TODO it would be nice to move the actual state creation (and operator application)
//     out of the StateRegistry. This could for example be done by global functions
//     operating on state buffers (PackedStateBin *).
State StateRegistry::get_successor_state(const State &predecessor, const Operator &op) {
  assert(!op.is_axiom());
  state_data_pool.push_back(predecessor.get_packed_buffer());
  PackedStateBin *buffer = state_data_pool[state_data_pool.size() - 1];
  for (size_t i = 0; i < op.get_pre_post().size(); ++i) {
    const PrePost &pre_post = op.get_pre_post()[i];
    if (pre_post.does_fire(predecessor))
      g_state_packer->set(buffer, pre_post.var, pre_post.post);
  }
  g_axiom_evaluator->evaluate(buffer);
  StateID id = insert_id_or_pop_state();
  return lookup_state(id);
}

State StateRegistry::get_temporary_successor_state(const State &predecessor, const Operator &op) {
  assert(!op.is_axiom());
  state_data_pool.push_back(predecessor.get_packed_buffer());
  PackedStateBin *buffer = state_data_pool[state_data_pool.size() - 1];
  for (size_t i = 0; i < op.get_pre_post().size(); ++i) {
    const PrePost &pre_post = op.get_pre_post()[i];
    if (pre_post.does_fire(predecessor))
      g_state_packer->set(buffer, pre_post.var, pre_post.post);
  }
  g_axiom_evaluator->evaluate(buffer);
  StateID id(state_data_pool.size() - 1);
  StateIDSet::iterator result = registered_states.find(id);
  if (result != registered_states.end()) {
    state_data_pool.pop_back();
    return State(state_data_pool[result->value], *this, *result);
  } else {
    return State(state_data_pool[state_data_pool.size() - 1], *this, StateID::no_state);
  }
}

State StateRegistry::store()
{
  StateID id = insert_id_or_pop_state();
  return lookup_state(id);
}

void StateRegistry::discard()
{
  state_data_pool.pop_back();
}

void StateRegistry::subscribe(PerStateInformationBase *psi) const {
  subscribers.insert(psi);
}

void StateRegistry::unsubscribe(PerStateInformationBase *const psi) const {
  subscribers.erase(psi);
}

void StateRegistry::shrink(size_t size) {
  if (size < state_data_pool.size()) {
    if (size == 0) {
      cached_initial_state = NULL;
    }
    for (size_t id = size; id < registered_states.size(); id++) {
      registered_states.erase(StateID(id));
    }
    state_data_pool.resize(size, NULL);
  }
}
