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

#include <algorithm>
#include <cmath>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/strings/match.h"
#include "absl/strings/str_join.h"
#include "google/protobuf/text_format.h"
#include "google/protobuf/wrappers.pb.h"
#include "ortools/base/logging.h"
#include "ortools/base/timer.h"
#include "ortools/data/jobshop_scheduling.pb.h"
#include "ortools/data/jobshop_scheduling_parser.h"
#include "ortools/graph/connected_components.h"
#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model.pb.h"
#include "ortools/sat/model.h"

ABSL_FLAG(std::string, input, "", "Jobshop data file name.");
ABSL_FLAG(std::string, params, "", "Sat parameters in text proto format.");
ABSL_FLAG(bool, use_optional_variables, true,
          "Whether we use optional variables for bounds of an optional "
          "interval or not.");
ABSL_FLAG(bool, use_interval_makespan, true,
          "Whether we encode the makespan using an interval or not.");
ABSL_FLAG(bool, use_expanded_precedences, false,
          "Whether we add precedences between alternative tasks within the "
          "same job.");
ABSL_FLAG(
    bool, use_cumulative_relaxation, true,
    "Whether we regroup multiple machines to create a cumulative relaxation.");
ABSL_FLAG(
    int, job_suffix_relaxation_length, 5,
    "The maximum length of the suffix of a job used in the linear relaxation.");
ABSL_FLAG(bool, display_model, false, "Display jobshop proto before solving.");
ABSL_FLAG(bool, display_sat_model, false, "Display sat proto before solving.");
ABSL_FLAG(int, horizon, -1, "Override horizon computation.");

using operations_research::data::jssp::Job;
using operations_research::data::jssp::JobPrecedence;
using operations_research::data::jssp::JsspInputProblem;
using operations_research::data::jssp::Machine;
using operations_research::data::jssp::Task;
using operations_research::data::jssp::TransitionTimeMatrix;

namespace operations_research {
namespace sat {

// Compute a valid horizon from a problem.
int64 ComputeHorizon(const JsspInputProblem& problem) {
  int64 sum_of_durations = 0;
  int64 max_latest_end = 0;
  int64 max_earliest_start = 0;
  for (const Job& job : problem.jobs()) {
    if (job.has_latest_end()) {
      max_latest_end = std::max(max_latest_end, job.latest_end().value());
    } else {
      max_latest_end = kint64max;
    }
    if (job.has_earliest_start()) {
      max_earliest_start =
          std::max(max_earliest_start, job.earliest_start().value());
    }
    for (const Task& task : job.tasks()) {
      int64 max_duration = 0;
      for (int64 d : task.duration()) {
        max_duration = std::max(max_duration, d);
      }
      sum_of_durations += max_duration;
    }
  }

  const int num_jobs = problem.jobs_size();
  int64 sum_of_transitions = 0;
  for (const Machine& machine : problem.machines()) {
    if (!machine.has_transition_time_matrix()) continue;
    const TransitionTimeMatrix& matrix = machine.transition_time_matrix();
    for (int i = 0; i < num_jobs; ++i) {
      int64 max_transition = 0;
      for (int j = 0; j < num_jobs; ++j) {
        max_transition =
            std::max(max_transition, matrix.transition_time(i * num_jobs + j));
      }
      sum_of_transitions += max_transition;
    }
  }
  return std::min(max_latest_end,
                  sum_of_durations + sum_of_transitions + max_earliest_start);
  // TODO(user): Uses transitions.
}

// A job is a sequence of tasks. For each task, we store the main interval, as
// well as its start, size, and end variables.
struct JobTaskData {
  IntervalVar interval;
  IntVar start;
  IntVar duration;
  IntVar end;
};

// Each task in a job can have multiple alternative ways of being performed.
// This structure stores the start, end, and presence variables attached to one
// alternative for a given task.
struct AlternativeTaskData {
  IntervalVar interval;
  IntVar start;
  IntVar end;
  BoolVar presence;
};

// Create the job structure as a chain of tasks. Fills in the job_to_tasks
// vector.
void CreateJobs(const JsspInputProblem& problem, int64 horizon,
                std::vector<std::vector<JobTaskData>>& job_to_tasks,
                bool& has_variable_duration_tasks, CpModelBuilder& cp_model) {
  const int num_jobs = problem.jobs_size();

  for (int j = 0; j < num_jobs; ++j) {
    const Job& job = problem.jobs(j);
    const int num_tasks_in_job = job.tasks_size();
    std::vector<JobTaskData>& task_data = job_to_tasks[j];

    const int64 hard_start =
        job.has_earliest_start() ? job.earliest_start().value() : 0L;
    const int64 hard_end =
        job.has_latest_end() ? job.latest_end().value() : horizon;

    for (int t = 0; t < num_tasks_in_job; ++t) {
      const Task& task = job.tasks(t);
      const int num_alternatives = task.machine_size();
      CHECK_EQ(num_alternatives, task.duration_size());

      // Add the "main" task interval.
      std::vector<int64> durations;
      int64 min_duration = task.duration(0);
      int64 max_duration = task.duration(0);
      durations.push_back(task.duration(0));
      for (int a = 1; a < num_alternatives; ++a) {
        min_duration = std::min(min_duration, task.duration(a));
        max_duration = std::max(max_duration, task.duration(a));
        durations.push_back(task.duration(a));
      }

      if (min_duration != max_duration) has_variable_duration_tasks = true;

      const IntVar start = cp_model.NewIntVar(Domain(hard_start, hard_end));
      const IntVar duration = cp_model.NewIntVar(Domain::FromValues(durations));
      const IntVar end = cp_model.NewIntVar(Domain(hard_start, hard_end));
      const IntervalVar interval =
          cp_model.NewIntervalVar(start, duration, end);

      // Fill in job_to_tasks.
      task_data.push_back({interval, start, duration, end});

      // Chain the task belonging to the same job.
      if (t > 0) {
        cp_model.AddLessOrEqual(task_data[t - 1].end, task_data[t].start);
      }
    }
  }
}

// For each task of each jobs, create the alternative tasks and link them to the
// main task of the job.
void CreateAlternativeTasks(
    const JsspInputProblem& problem,
    const std::vector<std::vector<JobTaskData>>& job_to_tasks, int64 horizon,
    std::vector<std::vector<std::vector<AlternativeTaskData>>>&
        job_task_to_alternatives,
    CpModelBuilder& cp_model) {
  const int num_jobs = problem.jobs_size();
  const BoolVar true_var = cp_model.TrueVar();

  for (int j = 0; j < num_jobs; ++j) {
    const Job& job = problem.jobs(j);
    const int num_tasks_in_job = job.tasks_size();
    job_task_to_alternatives[j].resize(num_tasks_in_job);
    const std::vector<JobTaskData>& tasks = job_to_tasks[j];

    const int64 hard_start =
        job.has_earliest_start() ? job.earliest_start().value() : 0L;
    const int64 hard_end =
        job.has_latest_end() ? job.latest_end().value() : horizon;

    for (int t = 0; t < num_tasks_in_job; ++t) {
      const Task& task = job.tasks(t);
      const int num_alternatives = task.machine_size();
      CHECK_EQ(num_alternatives, task.duration_size());
      std::vector<AlternativeTaskData>& alt_data =
          job_task_to_alternatives[j][t];

      absl::flat_hash_map<int64, std::vector<int>> duration_supports;
      duration_supports[task.duration(0)].push_back(0);
      for (int a = 1; a < num_alternatives; ++a) {
        duration_supports[task.duration(a)].push_back(a);
      }

      if (num_alternatives == 1) {
        alt_data.push_back(
            {tasks[t].interval, tasks[t].start, tasks[t].end, true_var});
      } else {
        for (int a = 0; a < num_alternatives; ++a) {
          const BoolVar local_presence = cp_model.NewBoolVar();
          const IntVar local_start =
              absl::GetFlag(FLAGS_use_optional_variables)
                  ? cp_model.NewIntVar(Domain(hard_start, hard_end))
                  : tasks[t].start;
          const IntVar local_duration = cp_model.NewConstant(task.duration(a));
          const IntVar local_end =
              absl::GetFlag(FLAGS_use_optional_variables)
                  ? cp_model.NewIntVar(Domain(hard_start, hard_end))
                  : tasks[t].end;
          const IntervalVar local_interval = cp_model.NewOptionalIntervalVar(
              local_start, local_duration, local_end, local_presence);

          // Link local and global variables.
          if (absl::GetFlag(FLAGS_use_optional_variables)) {
            cp_model.AddEquality(tasks[t].start, local_start)
                .OnlyEnforceIf(local_presence);
            cp_model.AddEquality(tasks[t].end, local_end)
                .OnlyEnforceIf(local_presence);

            // TODO(user): Experiment with the following implication.
            cp_model.AddEquality(tasks[t].duration, task.duration(a))
                .OnlyEnforceIf(local_presence);
          }

          alt_data.push_back(
              {local_interval, local_start, local_end, local_presence});
        }
        // Exactly one alternative interval is present.
        std::vector<BoolVar> interval_presences;
        for (const AlternativeTaskData& alternative : alt_data) {
          interval_presences.push_back(alternative.presence);
        }
        cp_model.AddEquality(LinearExpr::BooleanSum(interval_presences), 1);

        // Implement supporting literals for the duration of the main interval.
        if (duration_supports.size() > 1) {  // duration is not fixed.
          for (const auto& duration_alternative_indices : duration_supports) {
            const int64 value = duration_alternative_indices.first;
            const BoolVar duration_eq_value = cp_model.NewBoolVar();

            // duration_eq_value <=> duration == value.
            cp_model.AddEquality(tasks[t].duration, value)
                .OnlyEnforceIf(duration_eq_value);
            cp_model.AddNotEqual(tasks[t].duration, value)
                .OnlyEnforceIf(duration_eq_value.Not());
            // Implement the support part. If all literals pointing to the same
            // duration are false, then the duration cannot take this value.
            std::vector<BoolVar> support_clause;
            for (const int a : duration_alternative_indices.second) {
              support_clause.push_back(alt_data[a].presence);
            }
            support_clause.push_back(duration_eq_value.Not());
            cp_model.AddBoolOr(support_clause);
          }
        }
      }

      // Chain the alternative tasks belonging to the same job.
      if (t > 0 && absl::GetFlag(FLAGS_use_expanded_precedences)) {
        const std::vector<AlternativeTaskData>& prev_data =
            job_task_to_alternatives[j][t - 1];
        const std::vector<AlternativeTaskData>& curr_data =
            job_task_to_alternatives[j][t];
        for (int p = 0; p < prev_data.size(); ++p) {
          const IntVar previous_end = prev_data[p].end;
          const BoolVar previous_presence = prev_data[p].presence;
          for (int c = 0; c < curr_data.size(); ++c) {
            const IntVar current_start = curr_data[c].start;
            const BoolVar current_presence = curr_data[c].presence;
            cp_model.AddLessOrEqual(previous_end, current_start)
                .OnlyEnforceIf({previous_presence, current_presence});
          }
        }
      }
    }
  }
}

// Tasks or alternative tasks are added to machines one by one.
// This structure records the characteristics of each task added on a machine.
// This information is indexed on each vector by the order of addition.
struct MachineTaskData {
  IntervalVar interval;
  int job;
  IntVar start;
  int64 duration;
  IntVar end;
  BoolVar presence;
};
void CreateMachines(
    const JsspInputProblem& problem,
    const std::vector<std::vector<std::vector<AlternativeTaskData>>>&
        job_task_to_alternatives,
    IntervalVar makespan_interval, CpModelBuilder& cp_model) {
  const int num_jobs = problem.jobs_size();
  const int num_machines = problem.machines_size();
  std::vector<std::vector<MachineTaskData>> machine_to_tasks(num_machines);

  // Fills in the machine data vector.
  for (int j = 0; j < num_jobs; ++j) {
    const Job& job = problem.jobs(j);
    const int num_tasks_in_job = job.tasks_size();

    for (int t = 0; t < num_tasks_in_job; ++t) {
      const Task& task = job.tasks(t);
      const int num_alternatives = task.machine_size();
      CHECK_EQ(num_alternatives, task.duration_size());
      const std::vector<AlternativeTaskData>& alt_data =
          job_task_to_alternatives[j][t];

      for (int a = 0; a < num_alternatives; ++a) {
        // Record relevant variables for later use.
        machine_to_tasks[task.machine(a)].push_back(
            {alt_data[a].interval, j, alt_data[a].start, task.duration(a),
             alt_data[a].end, alt_data[a].presence});
      }
    }
  }

  // Add one no_overlap constraint per machine.
  for (int m = 0; m < num_machines; ++m) {
    std::vector<IntervalVar> intervals;
    for (const MachineTaskData& task : machine_to_tasks[m]) {
      intervals.push_back(task.interval);
    }
    if (absl::GetFlag(FLAGS_use_interval_makespan) &&
        problem.makespan_cost_per_time_unit() != 0L) {
      intervals.push_back(makespan_interval);
    }
    cp_model.AddNoOverlap(intervals);
  }

  // Add transition times if needed.
  for (int m = 0; m < num_machines; ++m) {
    if (problem.machines(m).has_transition_time_matrix()) {
      const int num_intervals = machine_to_tasks[m].size();
      const TransitionTimeMatrix& transitions =
          problem.machines(m).transition_time_matrix();

      // Create circuit constraint on a machine.
      // Node 0 and num_intervals + 1 are source and sink.
      CircuitConstraint circuit = cp_model.AddCircuitConstraint();
      for (int i = 0; i < num_intervals; ++i) {
        const int job_i = machine_to_tasks[m][i].job;
        // Source to nodes.
        circuit.AddArc(0, i + 1, cp_model.NewBoolVar());
        // Node to sink.
        circuit.AddArc(i + 1, 0, cp_model.NewBoolVar());
        // Node to node.
        for (int j = 0; j < num_intervals; ++j) {
          if (i == j) {
            circuit.AddArc(i + 1, i + 1, Not(machine_to_tasks[m][i].presence));
          } else {
            const int job_j = machine_to_tasks[m][i].job;
            const int64 transition =
                transitions.transition_time(job_i * num_jobs + job_j);
            const BoolVar lit = cp_model.NewBoolVar();
            const IntVar start = machine_to_tasks[m][j].start;
            const IntVar end = machine_to_tasks[m][i].end;
            circuit.AddArc(i + 1, j + 1, lit);
            // Push the new start with an extra transition.
            cp_model
                .AddLessOrEqual(LinearExpr(end).AddConstant(transition), start)
                .OnlyEnforceIf(lit);
          }
        }
      }
    }
  }
}

// Collect all objective terms and add them to the model.
void CreateObjective(
    const JsspInputProblem& problem,
    const std::vector<std::vector<JobTaskData>>& job_to_tasks,
    const std::vector<std::vector<std::vector<AlternativeTaskData>>>&
        job_task_to_alternatives,
    int64 horizon, IntVar makespan, CpModelBuilder& cp_model) {
  int64 objective_offset = 0;
  std::vector<IntVar> objective_vars;
  std::vector<int64> objective_coeffs;

  const int num_jobs = problem.jobs_size();
  for (int j = 0; j < num_jobs; ++j) {
    const Job& job = problem.jobs(j);
    const int num_tasks_in_job = job.tasks_size();

    // Add the cost associated to each task.
    for (int t = 0; t < num_tasks_in_job; ++t) {
      const Task& task = job.tasks(t);
      const int num_alternatives = task.machine_size();

      for (int a = 0; a < num_alternatives; ++a) {
        // Add cost if present.
        if (task.cost_size() > 0) {
          objective_vars.push_back(job_task_to_alternatives[j][t][a].presence);
          objective_coeffs.push_back(task.cost(a));
        }
      }
    }

    // Job lateness cost.
    const int64 lateness_penalty = job.lateness_cost_per_time_unit();
    if (lateness_penalty != 0L) {
      const int64 due_date = job.late_due_date();
      const IntVar job_end = job_to_tasks[j].back().end;
      if (due_date == 0) {
        objective_vars.push_back(job_end);
        objective_coeffs.push_back(lateness_penalty);
      } else {
        const IntVar lateness_var = cp_model.NewIntVar(Domain(0, horizon));
        cp_model.AddLinMaxEquality(
            lateness_var,
            {LinearExpr(0), LinearExpr(job_end).AddConstant(-due_date)});
        objective_vars.push_back(lateness_var);
        objective_coeffs.push_back(lateness_penalty);
      }
    }

    // Job earliness cost.
    const int64 earliness_penalty = job.earliness_cost_per_time_unit();
    if (earliness_penalty != 0L) {
      const int64 due_date = job.early_due_date();
      const IntVar job_end = job_to_tasks[j].back().end;

      if (due_date > 0) {
        const IntVar earliness_var = cp_model.NewIntVar(Domain(0, horizon));
        cp_model.AddLinMaxEquality(
            earliness_var,
            {LinearExpr(0),
             LinearExpr::Term(job_end, -1).AddConstant(due_date)});
        objective_vars.push_back(earliness_var);
        objective_coeffs.push_back(earliness_penalty);
      }
    }
  }

  // Makespan objective.
  if (problem.makespan_cost_per_time_unit() != 0L) {
    objective_coeffs.push_back(problem.makespan_cost_per_time_unit());
    objective_vars.push_back(makespan);
  }

  // Add the objective to the model.
  cp_model.Minimize(LinearExpr::ScalProd(objective_vars, objective_coeffs)
                        .AddConstant(objective_offset));
  if (problem.has_scaling_factor()) {
    cp_model.ScaleObjectiveBy(problem.scaling_factor().value());
  }
}

// This is a relaxation of the problem where we only consider the main tasks,
// and not the alternate copies.
void AddCumulativeRelaxation(
    const JsspInputProblem& problem,
    const std::vector<std::vector<JobTaskData>>& job_to_tasks,
    IntervalVar makespan_interval, CpModelBuilder& cp_model) {
  const int num_jobs = problem.jobs_size();
  const int num_machines = problem.machines_size();

  // Build a graph where two machines are connected if they appear in the same
  // set of alternate machines for a given task.
  std::vector<absl::flat_hash_set<int>> neighbors(num_machines);
  for (int j = 0; j < num_jobs; ++j) {
    const Job& job = problem.jobs(j);
    const int num_tasks_in_job = job.tasks_size();
    for (int t = 0; t < num_tasks_in_job; ++t) {
      const Task& task = job.tasks(t);
      for (int a = 1; a < task.machine_size(); ++a) {
        neighbors[task.machine(0)].insert(task.machine(a));
      }
    }
  }

  // Search for connected components in the above graph.
  std::vector<int> components =
      util::GetConnectedComponents(num_machines, neighbors);
  absl::flat_hash_map<int, std::vector<int>> machines_per_component;
  for (int c = 0; c < components.size(); ++c) {
    machines_per_component[components[c]].push_back(c);
  }

  const IntVar one = cp_model.NewConstant(1);
  for (const auto& it : machines_per_component) {
    // Ignore the trivial cases.
    if (it.second.size() < 2 || it.second.size() == num_machines) continue;

    LOG(INFO) << "Found machine connected component: ["
              << absl::StrJoin(it.second, ", ") << "]";
    absl::flat_hash_set<int> component(it.second.begin(), it.second.end());
    const IntVar capacity = cp_model.NewConstant(component.size());
    int num_intervals_in_cumulative = 0;
    CumulativeConstraint cumul = cp_model.AddCumulative(capacity);
    for (int j = 0; j < num_jobs; ++j) {
      const Job& job = problem.jobs(j);
      const int num_tasks_in_job = job.tasks_size();
      for (int t = 0; t < num_tasks_in_job; ++t) {
        const Task& task = job.tasks(t);
        for (const int m : task.machine()) {
          if (component.contains(m)) {
            cumul.AddDemand(job_to_tasks[j][t].interval, one);
            num_intervals_in_cumulative++;
            break;
          }
        }
      }
    }
    if (absl::GetFlag(FLAGS_use_interval_makespan)) {
      cumul.AddDemand(makespan_interval, capacity);
    }
    LOG(INFO) << "   - created cumulative with " << num_intervals_in_cumulative
              << " intervals";
  }
}

// There are two linear redundant constraints.
//
// The first one states that the sum of durations of all tasks is a lower bound
// of the makespan * number of machines.
//
// The second one takes a suffix of one job chain, and states that the start of
// the suffix + the sum of all task durations in the suffix is a lower bound of
// the makespan.
void AddMakespanRedundantConstraints(
    const JsspInputProblem& problem,
    const std::vector<std::vector<JobTaskData>>& job_to_tasks, IntVar makespan,
    bool has_variable_duration_tasks, CpModelBuilder& cp_model) {
  const int num_jobs = problem.jobs_size();
  const int num_machines = problem.machines_size();

  // Global energetic reasoning.
  std::vector<IntVar> all_task_durations;
  for (const std::vector<JobTaskData>& tasks : job_to_tasks) {
    for (const JobTaskData& task : tasks) {
      all_task_durations.push_back(task.duration);
    }
  }
  cp_model.AddLessOrEqual(LinearExpr::Sum(all_task_durations),
                          LinearExpr::Term(makespan, num_machines));

  // Suffix linear equations.
  if (has_variable_duration_tasks) {
    for (int j = 0; j < num_jobs; ++j) {
      const int job_length = job_to_tasks[j].size();
      const int start_suffix = std::max(
          0, job_length - absl::GetFlag(FLAGS_job_suffix_relaxation_length));
      for (int first_t = start_suffix; first_t + 1 < job_length; ++first_t) {
        std::vector<IntVar> terms = {job_to_tasks[j][first_t].start};
        for (int t = first_t; t < job_length; ++t) {
          terms.push_back(job_to_tasks[j][t].duration);
        }
        cp_model.AddLessOrEqual(LinearExpr::Sum(terms), makespan);
      }
    }
  }
}

// Solve a JobShop scheduling problem using CP-SAT.
void Solve(const JsspInputProblem& problem) {
  if (absl::GetFlag(FLAGS_display_model)) {
    LOG(INFO) << problem.DebugString();
  }

  CpModelBuilder cp_model;

  // Compute an over estimate of the horizon.
  const int64 horizon = absl::GetFlag(FLAGS_horizon) != -1
                            ? absl::GetFlag(FLAGS_horizon)
                            : ComputeHorizon(problem);

  // Create the main job structure.
  const int num_jobs = problem.jobs_size();
  std::vector<std::vector<JobTaskData>> job_to_tasks(num_jobs);
  bool has_variable_duration_tasks = false;
  CreateJobs(problem, horizon, job_to_tasks, has_variable_duration_tasks,
             cp_model);

  // For each task of each jobs, create the alternative copies if needed and
  // fill in the AlternativeTaskData vector.
  std::vector<std::vector<std::vector<AlternativeTaskData>>>
      job_task_to_alternatives(num_jobs);
  CreateAlternativeTasks(problem, job_to_tasks, horizon,
                         job_task_to_alternatives, cp_model);

  // Create the makespan variable and interval.
  // If this flag is true, we will add to each no overlap constraint a special
  // "makespan interval" that must necessarily be last by construction. This
  // gives us a better lower bound on the makespan because this way we known
  // that it must be after all other intervals in each no-overlap constraint.
  //
  // Otherwise, we will just add precence constraints between the last task of
  // each job and the makespan variable. Alternatively, we could have added a
  // precedence relation between all tasks and the makespan for a similar
  // propagation thanks to our "precedence" propagator in the dijsunctive but
  // that was slower than the interval trick when I tried.
  const IntVar makespan = cp_model.NewIntVar(Domain(0, horizon));
  IntervalVar makespan_interval;
  if (absl::GetFlag(FLAGS_use_interval_makespan)) {
    makespan_interval = cp_model.NewIntervalVar(
        /*start=*/makespan,
        /*size=*/cp_model.NewIntVar(Domain(1, horizon)),
        /*end=*/cp_model.NewIntVar(Domain(horizon + 1)));
  } else if (problem.makespan_cost_per_time_unit() != 0L) {
    for (int j = 0; j < num_jobs; ++j) {
      // The makespan will be greater than the end of each job.
      // This is not needed if we add the makespan "interval" to each
      // disjunctive.
      cp_model.AddLessOrEqual(job_to_tasks[j].back().end, makespan);
    }
  }

  // Machine constraints.
  CreateMachines(problem, job_task_to_alternatives, makespan_interval,
                 cp_model);

  // Try to detect connected components of alternative machines.
  // If this is happens, we can add a cumulative constraint as a relaxation of
  // all no_ovelap constraints on the set of alternative machines.
  if (absl::GetFlag(FLAGS_use_cumulative_relaxation)) {
    AddCumulativeRelaxation(problem, job_to_tasks, makespan_interval, cp_model);
  }

  // Various redundant constraints. They are mostly here to improve the LP
  // relaxation.
  if (problem.makespan_cost_per_time_unit() != 0L) {
    AddMakespanRedundantConstraints(problem, job_to_tasks, makespan,
                                    has_variable_duration_tasks, cp_model);
  }

  // Add job precedences.
  for (const JobPrecedence& precedence : problem.precedences()) {
    const IntVar start =
        job_to_tasks[precedence.second_job_index()].front().start;
    const IntVar end = job_to_tasks[precedence.first_job_index()].back().end;
    cp_model.AddLessOrEqual(LinearExpr(end).AddConstant(precedence.min_delay()),
                            start);
  }

  // Objective.
  CreateObjective(problem, job_to_tasks, job_task_to_alternatives, horizon,
                  makespan, cp_model);

  // Decision strategy.
  std::vector<IntVar> all_task_starts;
  for (const std::vector<JobTaskData>& job : job_to_tasks) {
    for (const JobTaskData& task : job) {
      all_task_starts.push_back(task.start);
    }
  }
  cp_model.AddDecisionStrategy(all_task_starts,
                               DecisionStrategyProto::CHOOSE_LOWEST_MIN,
                               DecisionStrategyProto::SELECT_MIN_VALUE);

  // Display problem statistics.
  int num_tasks = 0;
  int num_tasks_with_variable_duration = 0;
  int num_tasks_with_alternatives = 0;
  for (const std::vector<JobTaskData>& job : job_to_tasks) {
    num_tasks += job.size();
    for (const JobTaskData& task : job) {
      if (task.duration.Proto().domain_size() != 2 ||
          task.duration.Proto().domain(0) != task.duration.Proto().domain(1)) {
        num_tasks_with_variable_duration++;
      }
    }
  }
  for (const std::vector<std::vector<AlternativeTaskData>>&
           task_to_alternatives : job_task_to_alternatives) {
    for (const std::vector<AlternativeTaskData>& alternatives :
         task_to_alternatives) {
      if (alternatives.size() > 1) num_tasks_with_alternatives++;
    }
  }

  LOG(INFO) << "#machines:" << problem.machines_size();
  LOG(INFO) << "#jobs:" << num_jobs;
  LOG(INFO) << "horizon:" << horizon;
  LOG(INFO) << "#tasks: " << num_tasks;
  LOG(INFO) << "#tasks with alternative: " << num_tasks_with_alternatives;
  LOG(INFO) << "#tasks with variable duration: "
            << num_tasks_with_variable_duration;

  if (absl::GetFlag(FLAGS_display_sat_model)) {
    LOG(INFO) << cp_model.Proto().DebugString();
  }

  Model model;
  model.Add(NewSatParameters(absl::GetFlag(FLAGS_params)));
  const CpSolverResponse response = SolveCpModel(cp_model.Build(), &model);

  // Abort if we don't have any solution.
  if (response.status() != CpSolverStatus::OPTIMAL &&
      response.status() != CpSolverStatus::FEASIBLE)
    return;

  // Check cost, recompute it from scratch.
  int64 final_cost = 0;
  if (problem.makespan_cost_per_time_unit() != 0) {
    int64 makespan = 0;
    for (const std::vector<JobTaskData>& tasks : job_to_tasks) {
      const IntVar job_end = tasks.back().end;
      makespan = std::max(makespan, SolutionIntegerValue(response, job_end));
    }
    final_cost += makespan * problem.makespan_cost_per_time_unit();
  }

  for (int j = 0; j < num_jobs; ++j) {
    const int64 early_due_date = problem.jobs(j).early_due_date();
    const int64 late_due_date = problem.jobs(j).late_due_date();
    const int64 early_penalty = problem.jobs(j).earliness_cost_per_time_unit();
    const int64 late_penalty = problem.jobs(j).lateness_cost_per_time_unit();
    const int64 end =
        SolutionIntegerValue(response, job_to_tasks[j].back().end);
    if (end < early_due_date && early_penalty != 0) {
      final_cost += (early_due_date - end) * early_penalty;
    }
    if (end > late_due_date && late_penalty != 0) {
      final_cost += (end - late_due_date) * late_penalty;
    }
  }

  // TODO(user): Support alternative cost in check.
  const double tolerance = 1e-6;
  CHECK_GE(response.objective_value(), final_cost - tolerance);
  CHECK_LE(response.objective_value(), final_cost + tolerance);
}

}  // namespace sat
}  // namespace operations_research

int main(int argc, char** argv) {
  absl::SetFlag(&FLAGS_logtostderr, true);
  google::InitGoogleLogging(argv[0]);
  absl::ParseCommandLine(argc, argv);

  if (absl::GetFlag(FLAGS_input).empty()) {
    LOG(FATAL) << "Please supply a data file with --input=";
  }

  operations_research::data::jssp::JsspParser parser;
  CHECK(parser.ParseFile(absl::GetFlag(FLAGS_input)));
  operations_research::sat::Solve(parser.problem());
  return EXIT_SUCCESS;
}
