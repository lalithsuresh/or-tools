// Copyright 2010-2018 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef OR_TOOLS_SAT_TIMETABLE_H_
#define OR_TOOLS_SAT_TIMETABLE_H_

#include <vector>

#include "ortools/base/macros.h"
#include "ortools/sat/integer.h"
#include "ortools/sat/intervals.h"
#include "ortools/util/rev.h"

namespace operations_research {
namespace sat {

// Adds a reservoir constraint to the model. Note that to account for level not
// containing zero at time zero, we might needs to create an artificial fixed
// event.
//
// This instantiate one or more ReservoirTimeTabling class to perform the
// propagation.
void AddReservoirConstraint(std::vector<AffineExpression> times,
                            std::vector<IntegerValue> deltas,
                            std::vector<Literal> presences, int64 min_level,
                            int64 max_level, Model* model);

// The piecewise constant function must be below the given capacity. The initial
// function value is zero. Note that a negative capacity will thus be trivially
// infeasible.
//
// Note that we take for the definition of the function at time t to be the sum
// of all delta with time <= t. But because we check for the capacity over the
// full horizon, we could have taken < t with no behavior change.
class ReservoirTimeTabling : public PropagatorInterface {
 public:
  ReservoirTimeTabling(const std::vector<AffineExpression>& times,
                       const std::vector<IntegerValue>& deltas,
                       const std::vector<Literal>& presences,
                       IntegerValue capacity, Model* model);

  bool Propagate() final;

 private:
  // The rectangle will be ordered by start, and the end of each rectangle
  // will be equal to the start of the next one. The height correspond to the
  // one from start (inclusive) until the next one (exclusive).
  struct ProfileRectangle {
    ProfileRectangle() {}
    ProfileRectangle(IntegerValue start, IntegerValue height)
        : start(start), height(height) {}

    bool operator<(const ProfileRectangle& other) const {
      return start < other.start;
    }

    /* const */ IntegerValue start = IntegerValue(0);
    /* const */ IntegerValue height = IntegerValue(0);
  };

  // Builds the profile and increases the lower bound of the capacity
  // variable accordingly.
  bool BuildProfile();

  // Explanation of the profile minimum value at time t, eventually ignoring the
  // given event.
  void FillReasonForProfileAtGivenTime(IntegerValue t,
                                       int event_to_ignore = -1);

  // Tries to tighten the min/max time of the given event depending on the sign
  // of the delta associated with this event.
  bool TryToIncreaseMin(int event);
  bool TryToDecreaseMax(int event);

  // Input.
  std::vector<AffineExpression> times_;
  std::vector<IntegerValue> deltas_;
  std::vector<Literal> presences_;
  IntegerValue capacity_;

  // Model class.
  const VariablesAssignment& assignment_;
  IntegerTrail* integer_trail_;

  // Temporary data.
  std::vector<Literal> literal_reason_;
  std::vector<IntegerLiteral> integer_reason_;
  std::vector<ProfileRectangle> profile_;
};

// A strongly quadratic version of Time Tabling filtering. This propagator
// is similar to the CumulativeTimeTable propagator of the constraint solver.
class TimeTablingPerTask : public PropagatorInterface {
 public:
  TimeTablingPerTask(const std::vector<AffineExpression>& demands,
                     AffineExpression capacity, IntegerTrail* integer_trail,
                     SchedulingConstraintHelper* helper);

  bool Propagate() final;

  void RegisterWith(GenericLiteralWatcher* watcher);

 private:
  // The rectangle will be ordered by start, and the end of each rectangle
  // will be equal to the start of the next one. The height correspond to the
  // one from start (inclusive) until the next one (exclusive).
  struct ProfileRectangle {
    /* const */ IntegerValue start;
    /* const */ IntegerValue height;

    ProfileRectangle(IntegerValue start, IntegerValue height)
        : start(start), height(height) {}

    bool operator<(const ProfileRectangle& other) const {
      return start < other.start;
    }
  };

  // Builds the profile and increases the lower bound of the capacity
  // variable accordingly.
  bool BuildProfile();

  // Reverses the profile. This is needed to reuse a given profile to update
  // both the start and end times.
  void ReverseProfile();

  // Tries to increase the minimum start time of each task according to the
  // current profile. This function can be called after ReverseProfile() and
  // ReverseVariables to update the maximum end time of each task.
  bool SweepAllTasks(bool is_forward);

  // Tries to increase the minimum start time of task_id.
  bool SweepTask(int task_id);

  // Updates the starting time of task_id to right and explain it. The reason is
  // all the mandatory parts contained in [left, right).
  bool UpdateStartingTime(int task_id, IntegerValue left, IntegerValue right);

  // Increases the minimum capacity to new_min and explain it. The reason is all
  // the mandatory parts that overlap time.
  bool IncreaseCapacity(IntegerValue time, IntegerValue new_min);

  // Explains the state of the profile in the time interval [left, right). The
  // reason is all the mandatory parts that overlap the interval. The current
  // reason is not cleared when this method is called.
  void AddProfileReason(IntegerValue left, IntegerValue right);

  IntegerValue CapacityMin() const {
    return integer_trail_->LowerBound(capacity_);
  }

  IntegerValue CapacityMax() const {
    return integer_trail_->UpperBound(capacity_);
  }

  IntegerValue DemandMin(int task_id) const {
    return integer_trail_->LowerBound(demands_[task_id]);
  }

  IntegerValue DemandMax(int task_id) const {
    return integer_trail_->UpperBound(demands_[task_id]);
  }

  // Returns true if the tasks is present and has a mantatory part.
  bool IsInProfile(int t) const {
    return positions_in_profile_tasks_[t] < num_profile_tasks_;
  }

  // Number of tasks.
  const int num_tasks_;

  // The demand variables of the tasks.
  std::vector<AffineExpression> demands_;

  // Capacity of the resource.
  const AffineExpression capacity_;

  IntegerTrail* integer_trail_;
  SchedulingConstraintHelper* helper_;

  // Optimistic profile of the resource consumption over time.
  std::vector<ProfileRectangle> profile_;
  IntegerValue profile_max_height_;

  // Reversible starting height of the reduced profile. This corresponds to the
  // height of the leftmost profile rectangle that can be used for propagation.
  IntegerValue starting_profile_height_;

  // Reversible sets of tasks to consider for the forward (resp. backward)
  // propagation. A task with a fixed start do not need to be considered for the
  // forward pass, same for task with fixed end for the backward pass. It is why
  // we use two sets.
  std::vector<int> forward_tasks_to_sweep_;
  std::vector<int> backward_tasks_to_sweep_;
  int forward_num_tasks_to_sweep_;
  int backward_num_tasks_to_sweep_;

  // Reversible set (with random access) of tasks to consider for building the
  // profile. The set contains the tasks in the [0, num_profile_tasks_) prefix
  // of profile_tasks_. The positions of a task in profile_tasks_ is contained
  // in positions_in_profile_tasks_.
  std::vector<int> profile_tasks_;
  std::vector<int> positions_in_profile_tasks_;
  int num_profile_tasks_;

  DISALLOW_COPY_AND_ASSIGN(TimeTablingPerTask);
};

}  // namespace sat
}  // namespace operations_research

#endif  // OR_TOOLS_SAT_TIMETABLE_H_
