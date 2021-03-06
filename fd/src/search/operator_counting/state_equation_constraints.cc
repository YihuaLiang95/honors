#include "state_equation_constraints.h"

#include "../globals.h"
#include "../operator.h"

#include "../option_parser.h"
#include "../plugin.h"

#include "../lp/lp_solver.h"

#include "../utils/markup.h"

using namespace std;

namespace operator_counting {
  void add_indices_to_constraint(lp::LPConstraint &constraint,
                                 const set<int> &indices,
                                 double coefficient) {
    for (int index : indices) {
      constraint.insert(index, coefficient);
    }
  }

  void StateEquationConstraints::build_propositions() {
    propositions.reserve(g_variable_domain.size());
    for (uint var = 0; var < g_variable_domain.size(); var++) {
      propositions.push_back(vector<Proposition>(g_variable_domain[var]));
    }
    for (uint op = 0; op < g_operators.size(); op++) {
      vector<int> precondition(g_variable_domain.size(), -1);
      for (const auto & prev : g_operators[op].get_prevail()) {
        precondition[prev.var] = prev.prev;
      }
      for (const auto & eff : g_operators[op].get_pre_post()) {
        int var = eff.var;
        int pre = eff.pre;
        int post = eff.post;
        if (pre != -1) {
          propositions[var][post].always_produced_by.insert(op);
          propositions[var][pre].always_consumed_by.insert(op);
        } else {
          propositions[var][post].sometimes_produced_by.insert(op);
        }
      }
    }
  }

  void StateEquationConstraints::add_constraints(
                                                 vector<lp::LPConstraint> &constraints, double infinity) {
    for (vector<Proposition> &var_propositions : propositions) {
      for (Proposition &prop : var_propositions) {
        lp::LPConstraint constraint(-infinity, infinity);
        add_indices_to_constraint(constraint, prop.always_produced_by, 1.0);
        add_indices_to_constraint(constraint, prop.sometimes_produced_by, 1.0);
        add_indices_to_constraint(constraint, prop.always_consumed_by, -1.0);
        if (!constraint.empty()) {
          prop.constraint_index = constraints.size();
          constraints.push_back(constraint);
        }
      }
    }
  }

  void StateEquationConstraints::initialize_constraints(
                                                        vector<lp::LPConstraint> &constraints,
                                                        double infinity) {
    cout << "Initializing constraints from state equation." << endl;
    ::verify_no_axioms();
    ::verify_no_conditional_effects();
    build_propositions();
    add_constraints(constraints, infinity);

    // Initialize goal state.
    goal_state = vector<int>(g_variable_domain.size(), numeric_limits<int>::max());
    for (const auto & goal : g_goal) {
      goal_state[goal.first] = goal.second;
    }
  }

  bool StateEquationConstraints::update_constraints(const State &state,
                                                    lp::LPSolver &lp_solver) {
    // Compute the bounds for the rows in the LP.
    for (size_t var = 0; var < propositions.size(); ++var) {
      int num_values = propositions[var].size();
      for (int value = 0; value < num_values; ++value) {
        const Proposition &prop = propositions[var][value];
        // Set row bounds.
        if (prop.constraint_index >= 0) {
          double lower_bound = 0;
          /* If we consider the current value of var, there must be an
             additional consumer. */
          if (state[var] == value) {
            --lower_bound;
          }
          /* If we consider the goal value of var, there must be an
             additional producer. */
          if (goal_state[var] == value) {
            ++lower_bound;
          }
          lp_solver.set_constraint_lower_bound(
                                               prop.constraint_index, lower_bound);
        }
      }
    }
    return false;
  }
}


static operator_counting::ConstraintGenerator* _parse(OptionParser &parser) {
  parser.document_synopsis(
                           "State equation constraints",
                           "For each fact, a permanent constraint is added that considers the net "
                           "change of the fact, i.e., the total number of times the fact is added "
                           "minus the total number of times is removed. The bounds of each "
                           "constraint depend on the current state and the goal state and are "
                           "updated in each state. For details, see" + utils::format_paper_reference(
                                                                                                     {"Menkes van den Briel", "J. Benton", "Subbarao Kambhampati",
                                                                                                         "Thomas Vossen"},
                                                                                                     "An LP-based heuristic for optimal planning",
                                                                                                     "http://link.springer.com/chapter/10.1007/978-3-540-74970-7_46",
                                                                                                     "Proceedings of the Thirteenth International Conference on"
                                                                                                     " Principles and Practice of Constraint Programming (CP 2007)",
                                                                                                     "651-665",
                                                                                                     "2007") + utils::format_paper_reference(
                                                                                                                                             {"Blai Bonet"},
                                                                                                                                             "An admissible heuristic for SAS+ planning obtained from the"
                                                                                                                                             " state equation",
                                                                                                                                             "http://ijcai.org/papers13/Papers/IJCAI13-335.pdf",
                                                                                                                                             "Proceedings of the Twenty-Third International Joint"
                                                                                                                                             " Conference on Artificial Intelligence (IJCAI 2013)",
                                                                                                                                             "2268-2274",
                                                                                                                                             "2013") + utils::format_paper_reference(
                                                                                                                                                                                     {"Florian Pommerening", "Gabriele Roeger", "Malte Helmert",
                                                                                                                                                                                         "Blai Bonet"},
                                                                                                                                                                                     "LP-based Heuristics for Cost-optimal Planning",
                                                                                                                                                                                     "http://www.aaai.org/ocs/index.php/ICAPS/ICAPS14/paper/view/7892/8031",
                                                                                                                                                                                     "Proceedings of the Twenty-Fourth International Conference"
                                                                                                                                                                                     " on Automated Planning and Scheduling (ICAPS 2014)",
                                                                                                                                                                                     "226-234",
                                                                                                                                                                                     "AAAI Press 2014"));

  if (parser.dry_run())
    return nullptr;
  return new operator_counting::StateEquationConstraints();
}

static Plugin<operator_counting::ConstraintGenerator> _plugin("state_equation_constraints", _parse);
