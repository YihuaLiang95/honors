
#include "uc_refinement_critical_paths.h"

#include "hc_heuristic.h"
#include "conjunction_operations.h"

#include "../globals.h"
#include "../state.h"
#include "../option_parser.h"
#include "../plugin.h"

#include "../operator.h"
#include "../utilities.h"

#include <string>

//int START = 0;

struct GreedyConflictSelector : public ConflictSelector {
    virtual unsigned operator()(UCRefinementCritPaths *r, const Fluent &subgoal,
                                int threshold) {
        UCHeuristic *pic = dynamic_cast<UCHeuristic*>(r->get_heuristic());
        int best_size = 0;
        int res = -1;
        std::vector<int> in_fluent;
        in_fluent.resize(pic->num_conjunctions(), 0);
        for (Fluent::iterator f = subgoal.begin(); f != subgoal.end(); f++) {
            const vector<unsigned> &f_conjs =
                pic->get_fact_conj_relation(pic->get_fact_id(*f));
            for (uint i = 0; i < f_conjs.size(); i++) {
                unsigned cid = f_conjs[i];
                const Conjunction &conj = pic->get_conjunction(cid);
                if (++in_fluent[cid] == conj.fluent_size &&
                    (!conj.is_achieved() ||
                     conj.cost >= threshold)) {
                    if (res == -1 || conj.fluent_size < best_size) {
                        res = cid;
                        best_size = conj.fluent_size;
                    }
                }
            }
        }
        in_fluent.clear();
        return res;
    }
};

UCRefinementCritPaths::UCRefinementCritPaths(const Options &opts) :
    UCRefinement(opts),
    c_use_caching(opts.get<bool>("caching")),
    m_selector(opts.get<ConflictSelector*>("conflicts"))
{}

void UCRefinementCritPaths::statistics()
{
    UCRefinement::statistics();
}

void UCRefinementCritPaths::release_memory() {
    m_conflicts.clear();
    m_achievers.clear();
    m_plan.clear();

    //m_duplicate_checking.clear();
    m_conflict_data.clear();
    m_pruned.clear();
    m_requires.clear();
    m_required_by.clear();
}

void UCRefinementCritPaths::prepare_refinement() {
    m_achievers.resize(uc->num_conjunctions());
    for (unsigned i = 0; i < uc->num_counters(); i++) {
        const ActionEffectCounter &counter = uc->get_counter(i);
        if (counter.unsatisfied_preconditions == 0
                && counter.cost + counter.base_cost == counter.effect->cost) {
            m_achievers[counter.effect->id].push_back(counter.action_id);
        }
    }
    if (c_use_caching) {
        m_conflict_data.resize(uc->num_facts());
    }
#ifndef NDEBUG
    for (unsigned i = 0; i < uc->num_conjunctions(); i++) {
        const Conjunction &conj = uc->get_conjunction(i);
        assert(!conj.is_achieved() || conj.cost == 0 || m_achievers[conj.id].size() > 0);
    }
#endif
}

HeuristicRefiner::RefinementResult UCRefinementCritPaths::refine(
    const State &state)
{
    //std::cout << ">>>>>>>>>>>REFINEMENT_START" << std::endl;
#ifndef NDEBUG
    for (unsigned i = 0; i < uc->num_counters(); i++) {
        assert(uc->get_counter(i).base_cost >= 1);
    }
#endif
    bool updated_c = false;
    RefinementResult successful = SUCCESSFUL;
    int old_val = -2;
    Fluent goal;
    for (uint i = 0; i < g_goal.size(); i++) {
        goal.insert(g_goal[i]);
    }
    while (successful != FAILED) {
        bool x = uc->set_early_termination(false);
        uc->evaluate(state);
        uc->set_early_termination(x);
#ifndef NDEBUG
        std::cout << "{val = " << uc->get_value() << "}" << std::endl;
#endif
        //std::cout << "val = " << uc->get_value() << std::endl;
        if (uc->is_dead_end()) {
            //std::cout << ">>>>>>>>>>>REFINEMENT_END" << std::endl;
            return updated_c ? SUCCESSFUL : UNCHANGED;
        }
        if (uc->get_value() == old_val) {
            assert(false);
            break;
        }
        old_val = uc->get_value();
        updated_c = true;
        prepare_refinement();
        //std::cout << "computing conflicts..." << std::flush;
        //START = uc->get_value();
        std::pair<bool, unsigned> res = compute_conflict(goal, uc->get_value(), state);
        //std::cout << "done => " << m_conflicts.size() << std::endl;
        if (res.second == (unsigned) -1) {
            //std::cout << "Found plan in uC refinement!" << std::endl;
            successful = SOLVED;
            break;
        } else {
            if (c_use_caching) {
                m_required_by.clear();
                unsigned i = 0;
                std::vector<unsigned> open;
                open.push_back(res.second);
                m_pruned[res.second] = true;
                while (i < open.size()) {
                    for (std::set<unsigned>::iterator it = m_requires[open[i]].begin(); it != m_requires[open[i]].end(); it++) {
                        if (!m_pruned[*it]) {
                            m_pruned[*it] = true;
                            open.push_back(*it);
                        }
                    }
                    i++;
                }
                for (uint i = 0; i < m_conflicts.size(); i++) {
                    if (m_pruned[i]) {
                        if (uc->exceeded_size_limit()) {
                            successful = FAILED;
                            break;
                        } else {
                            uc->add_conflict(m_conflicts[i]);
                        }
                    }
                }
            }
            else if (!uc->update_c(m_conflicts)) {
                successful = FAILED;
            }
        }
#ifndef NDEBUG
        uc->dump_compilation_information(std::cout);
#endif
        //std::cout << "successful = " << successful << std::endl;
        //exit(1);
        release_memory();
    }
    //assert(false);
    //std::cout << ">>>>>>>>>>>REFINEMENT_END" << std::endl;
    return successful;
}

std::pair<bool, unsigned> UCRefinementCritPaths::compute_conflict(
    const Fluent &subgoal, int threshold, const State &state)
{
    //ConflictData *confl_data = NULL;
    if (c_use_caching) {
#if 0
        confl_data = &m_duplicate_checking[subgoal];
        if (confl_data->threshold  >= threshold) {
            return make_pair(false, confl_data->conflict);
        }
#else
        std::vector<unsigned> subset(m_conflicts.size(), 0);
        for (Fluent::const_iterator it = subgoal.begin(); it != subgoal.end(); it++) {
            unsigned fid = uc->get_fact_id(*it);
            const std::vector<ConflictData> &confls = m_conflict_data[fid];
            for (const ConflictData &data : confls) {
                if (data.threshold >= threshold &&
                        ++subset[data.conflict] == m_conflicts[data.conflict].get_fluent().size()) {
                    return std::make_pair(false, data.conflict);
                }
            }
        }
#endif
    }
    unsigned conflict_id = m_conflicts.size();
    m_requires.resize(m_requires.size() + 1);
    m_required_by.resize(m_required_by.size() + 1);
    m_conflicts.resize(m_conflicts.size() + 1);
    m_pruned.push_back(false);
    if (!uc->extract_mutex(subgoal, m_conflicts[conflict_id])) {
        unsigned cid = (*m_selector)(this, subgoal, threshold);
        if (cid == ConflictSelector::INVALID) {
            return make_pair(false, -1);
        }
        if (!uc->get_conjunction(cid).is_achieved() ||
            uc->get_conjunction(cid).cost > threshold) {
            return make_pair(true, cid);
        }

        m_conflicts[conflict_id].merge(uc->get_fluent(cid));

        const vector<unsigned> &candidates = m_achievers[cid];
        for (size_t i = 0; i < candidates.size(); i++) {
            const StripsAction &action = uc->get_action(candidates[i]);
            Fact f_del;
            if (fluent_op::intersection_not_empty(subgoal, action.del_effect, f_del)) {
                m_conflicts[conflict_id].merge(f_del);
                continue;
            }

            //std::cout <<
            //    std::string(START - threshold + 1, ' ') << g_operators[action.operator_no].get_name() << std::endl;
            Fluent regr;
            regr.insert(action.precondition.begin(), action.precondition.end());
            fluent_op::set_minus(subgoal, action.add_effect, regr);
            assert(action.base_cost > 0);
            pair<bool, unsigned> child_confl =
                compute_conflict(regr, threshold - action.base_cost, state);
            regr.clear();
            if (child_confl.second == (unsigned) -1) {
                //_plan.push_back(action.operator_no);
                m_plan.push_back(&g_operators[action.operator_no]);
                return child_confl;
            }
            if (child_confl.first) {
                fluent_op::set_minus(uc->get_fluent(child_confl.second),
                                     action.precondition, regr);
            } else {
                assert(child_confl.second < m_conflicts.size());
                m_requires[conflict_id].insert(child_confl.second);
                m_required_by[child_confl.second].insert(conflict_id);
                fluent_op::set_minus(m_conflicts[child_confl.second].get_fluent(),
                                     action.precondition, regr);
            }
            m_conflicts[conflict_id].merge(regr);
            regr.clear();
        }

        bool is_subset = true;
        for (Fluent::iterator it = m_conflicts[conflict_id].get_fluent().begin();
                is_subset && it != m_conflicts[conflict_id].get_fluent().end(); it++) {
            if (state[it->first] != it->second) {
                is_subset = false;
            }
        }
        if (is_subset) {
            for (Fluent::iterator it = subgoal.begin(); it != subgoal.end(); it++) {
                if (state[it->first] != it->second) {
                    is_subset = false;
                    m_conflicts[conflict_id].merge(*it);
                }
            }
            if (is_subset) {
                return std::make_pair(false, -1);
            }
        }
    }
    if (c_use_caching) {
#if 0
        confl_data->threshold = threshold;
        confl_data->conflict = m_conflicts.size();
#else
        assert(m_requires.size() == m_conflicts.size());
        assert(m_required_by.size() == m_conflicts.size());
        assert(m_pruned.size() == m_conflicts.size());
        std::vector<unsigned> subset(m_conflicts.size(), 0);
        for (Fluent::iterator it = m_conflicts[conflict_id].get_fluent().begin();
                it != m_conflicts[conflict_id].get_fluent().end(); it++) {
            unsigned fid = uc->get_fact_id(*it);
            const std::vector<ConflictData> &confls = m_conflict_data[fid];
            for (const ConflictData &data : confls) {
                if (data.threshold <= threshold &&
                        ++subset[data.conflict] == m_conflicts[conflict_id].get_fluent().size()) {
                    assert(data.conflict < m_conflicts.size());
                    //m_pruned[data.conflict] = true;
                    std::set<unsigned> &reqby = m_required_by[data.conflict];
                    for (std::set<unsigned>::iterator it2 = reqby.begin(); it2 != reqby.end(); it2++) {
                        assert(*it2 < m_conflicts.size());
                        assert(m_requires[*it2].find(data.conflict) != m_requires[*it2].end());
                        m_required_by[conflict_id].insert(*it2);
                        m_requires[*it2].erase(m_requires[*it2].find(data.conflict));
                        m_requires[*it2].insert(conflict_id);
                    }
                    reqby.clear();
                }
            }
            m_conflict_data[fid].push_back(ConflictData(conflict_id, threshold));
        }
#endif
    }
    return make_pair(false, conflict_id);
}

void UCRefinementCritPaths::add_options_to_parser(OptionParser &parser) {
    UCRefinement::add_options_to_parser(parser);
    parser.add_option<bool>("caching", "", "true");
}

static HeuristicRefiner *_parse(OptionParser &parser) {
    UCRefinementCritPaths::add_options_to_parser(parser);
    Options opts = parser.parse();
    if (!parser.dry_run()) {
        opts.set<ConflictSelector*>("conflicts", new GreedyConflictSelector());
        return new UCRefinementCritPaths(opts);
    }
    return NULL;
}

static Plugin<HeuristicRefiner> _plugin("uccp", _parse);
