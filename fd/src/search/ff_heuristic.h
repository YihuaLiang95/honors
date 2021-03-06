#ifndef FF_HEURISTIC_H
#define FF_HEURISTIC_H

#include "additive_heuristic.h"
#include "rng.h"

#include <vector>
#include <string.h>
/*
  TODO: In a better world, this should not derive from
        AdditiveHeuristic. Rather, the common parts should be
        implemented in a common base class. That refactoring could be
        made at the same time at which we also unify this with the
        other relaxation heuristics and the additional FF heuristic
        implementation in the landmark code.
*/


class FFHeuristic : public AdditiveHeuristic {
  enum TieBreaking {
    ARBITRARY = 0,
    RANDOM = 1,
  };
  const TieBreaking c_tiebreaking;
  RandomNumberGenerator m_rng;
    // Relaxed plans are represented as a set of operators implemented
    // as a bit vector.
    typedef std::vector<bool> RelaxedPlan;
    RelaxedPlan relaxed_plan;
    void mark_preferred_operators_and_relaxed_plan(
        const State &state, Proposition *goal);
    void mark_relaxed_plan(const State &state, Proposition *goal); // Peter: Needed for M&S for finding label sets
protected:
    virtual void initialize();
    virtual int compute_heuristic(const State &state);
    virtual int compute_heuristic(const State &state, int g_value);
    void compute_relaxed_plan(const State &state, RelaxedPlan &returned_relaxed_plan); // Peter: Needed for M&S for finding label set
public:
   std::string h_name = "FF_heuristic";
    FFHeuristic(const Options &options);
    ~FFHeuristic();
};

#endif
