// Copyright 2010-2013 Google
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
//
// ACP 2014 challenge

#include <cstdio>

#include "base/commandlineflags.h"
#include "base/file.h"
#include "base/filelinereader.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "base/split.h"
#include "base/stringprintf.h"
#include "base/strtoint.h"
#include "constraint_solver/constraint_solver.h"
#include "constraint_solver/routing.h"
#include "util/tuple_set.h"

/* Data format
15
8
0 0 0 0 0 0 0 0 1 0 0 0 0 0 0
0 0 0 0 0 0 0 0 0 1 0 0 1 0 0
0 0 0 0 0 0 0 0 0 0 1 0 0 0 0
0 0 0 0 0 0 0 0 0 0 0 0 0 1 0
0 0 0 0 0 0 0 0 0 1 1 0 0 0 0
0 0 0 0 0 0 0 0 0 0 1 0 0 0 1
0 0 0 0 0 0 1 0 0 0 0 0 0 0 0
0 0 0 0 0 0 0 0 0 1 0 1 0 0 0
10
  0   78   86   93  120 12 155 20
165    0  193  213  178 12  90 20
214  170    0  190  185 12  40 20
178  177  185    0  196 12 155 66
201  199  215  190    0 12 155 20
201  100   88  190   14  0  75 70
 50  44   155  190   111 12 0  20
201  199  215  190   123 70 155 0
*/

DEFINE_string(input, "", "");
DEFINE_int32(lns_size, 10, "lns size");
DEFINE_int32(lns_limit, 30, "Limit the number of failures of the lns loop.");

DECLARE_bool(log_prefix);

namespace operations_research {

class AcpData {
 public:
  AcpData()
      : num_periods_(-1), num_products_(-1), inventory_cost_(0), state_(0) {}

  void Load(const std::string& filename) {
    FileLineReader reader(filename.c_str());
    reader.set_line_callback(
        NewPermanentCallback(this, &AcpData::ProcessNewLine));
    reader.Reload();
    if (!reader.loaded_successfully()) {
      LOG(ERROR) << "Could not open acp challenge file";
    }
  }

  void ProcessNewLine(char* const line) {
    const std::vector<std::string> words =
        strings::Split(line, " ", strings::SkipEmpty());
    if (words.empty()) return;
    switch (state_) {
      case 0: {
        num_periods_ = atoi32(words[0]);
        state_ = 1;
        break;
      }
      case 1: {
        num_products_ = atoi32(words[0]);
        state_ = 2;
        break;
      }
      case 2: {
        due_dates_per_product_.resize(due_dates_per_product_.size() + 1);
        CHECK_EQ(words.size(), num_periods_) << "Error with line " << line;
        for (int i = 0; i < num_periods_; ++i) {
          if (atoi32(words[i]) == 1) {
            due_dates_per_product_.back().push_back(i);
          }
        }
        if (due_dates_per_product_.size() == num_products_) {
          state_ = 3;
        }
        break;
      }
      case 3: {
        inventory_cost_ = atoi32(words[0]);
        state_ = 4;
        break;
      }
      case 4: {
        transitions_.resize(transitions_.size() + 1);
        CHECK_EQ(words.size(), num_products_);
        for (int i = 0; i < num_products_; ++i) {
          transitions_.back().push_back(atoi32(words[i]));
        }
        break;
      }
      default: {
        LOG(ERROR) << "Should not be here";
      }
    }
  }

  std::string DebugString() const {
    return StringPrintf("AcpData(%d periods, %d products, %d cost)",
                        num_periods_, num_products_, inventory_cost_);
  }

  const std::vector<std::vector<int>>& due_dates_per_product() const {
    return due_dates_per_product_;
  }

  const std::vector<std::vector<int>>& transitions() const {
    return transitions_;
  }

  int num_periods() const { return num_periods_; }
  int num_products() const { return num_products_; }
  int inventory_cost() const { return inventory_cost_; }

 private:
  int num_periods_;
  int num_products_;
  int inventory_cost_;
  std::vector<std::vector<int>> due_dates_per_product_;
  std::vector<std::vector<int>> transitions_;
  int state_;
};

class Swap : public IntVarLocalSearchOperator {
 public:
  explicit Swap(const std::vector<IntVar*>& variables)
      : IntVarLocalSearchOperator(variables),
        index1_(0),
        index2_(0) {}

  virtual ~Swap() {}

 protected:
  // Make a neighbor assigning one variable to its target value.
  virtual bool MakeOneNeighbor() {
    const int size = Size();
    index2_++;
    if (index2_ == size) {
      index2_ = 0;
      index1_++;
    }
    if (index1_ == size - 1) {
      return false;
    }
    SetValue(index1_, OldValue(index2_));
    SetValue(index2_, OldValue(index1_));
    return true;
  }

 private:
  virtual void OnStart() {
    index1_ = 0;
    index2_ = 0;
  }

  int index1_;
  int index2_;
};

class Filter : public IntVarLocalSearchFilter {
 public:
  explicit Filter(const std::vector<IntVar*>& vars,
                  const std::vector<int>& item_to_product,
                  const std::vector<int>& due_dates,
                  const std::vector<std::vector<int>>& transitions,
                  int inventory_cost)
      : IntVarLocalSearchFilter(vars),
        item_to_product_(item_to_product),
        due_dates_(due_dates),
        transitions_(transitions),
        inventory_cost_(inventory_cost),
        tmp_solution_(vars.size()),
        current_position_(item_to_product.size()),
        current_cost_(0) {}

  virtual ~Filter() {}

  virtual void OnSynchronize(const Assignment* delta) {
    for (int i = 0; i < Size(); ++i) {
      const int value = Value(i);
      tmp_solution_[i] = value;
      if (value != -1) {
        current_position_[value] = i;
      }
    }
    current_cost_ = Evaluate();
  }

  virtual bool Accept(const Assignment* delta,
                      const Assignment* unused_deltadelta) {
    const Assignment::IntContainer& solution_delta = delta->IntVarContainer();
    const int solution_delta_size = solution_delta.Size();
    for (int i = 0; i < Size(); ++i) {
      tmp_solution_[i] = Value(i);
    }
    for (int i = 0; i < solution_delta_size; ++i) {
      if (!solution_delta.Element(i).Activated()) {
        return true;
      }
    }
    for (int index = 0; index < solution_delta_size; ++index) {
      int64 touched_var = -1;
      FindIndex(solution_delta.Element(index).Var(), &touched_var);
      const int64 new_value = solution_delta.Element(index).Value();
      tmp_solution_[touched_var] = new_value;
    }
    const int new_cost = Evaluate();
    return new_cost < current_cost_;
  }

  int Evaluate() const {
    return 0;
  }

 private:
  const std::vector<int> item_to_product_;
  const std::vector<int> due_dates_;
  const std::vector<std::vector<int>> transitions_;
  const int inventory_cost_;
  std::vector<int> tmp_solution_;
  std::vector<int> current_position_;
  int current_cost_;
};

void Solve(const string& filename) {
  LOG(INFO) << "Load " << filename;
  AcpData data;
  data.Load(filename);
  LOG(INFO) << "  - " << data.num_periods() << " periods";
  LOG(INFO) << "  - " << data.num_products() << " products";
  LOG(INFO) << "  - earliness cost is " << data.inventory_cost();

  int num_items = 0;
  for (const std::vector<int>& d : data.due_dates_per_product()) {
    num_items += d.size();
  }

  LOG(INFO) << "  - " << num_items << " items";
  const int num_residuals = data.num_periods() - num_items;
  LOG(INFO) << "  - " << num_residuals << " non act`ive periods";

  std::vector<int> item_to_product;
  for (int i = 0; i < data.num_products(); ++i) {
    for (int j = 0; j < data.due_dates_per_product()[i].size(); ++j) {
      item_to_product.push_back(i);
    }
  }

  LOG(INFO) << "Build model";
  int max_cost = 0;
  IntTupleSet transition_cost_tuples(5);
  vector<int> tuple(5);
  for (int i = 0; i < data.num_products(); ++i) {
    for (int j = 0; j < data.num_products(); ++j) {
      const int cost = data.transitions()[i][j];
      max_cost = std::max(max_cost, cost);
      tuple[0] = i;  // value on timetable
      tuple[1] = i;  // state (last value defined if value is -1)
      tuple[2] = j;  // value on next date
      tuple[3] = j;  // state on next date
      tuple[4] = cost;
      transition_cost_tuples.Insert(tuple);
      tuple[0] = -1;  // Continuation from i, -1 ,j
      transition_cost_tuples.Insert(tuple);
    }
    // Build tuples value, -1 that keeps the state.
    tuple[0] = i;
    tuple[1] = i;
    tuple[2] = -1;
    tuple[3] = i;
    tuple[4] = 0;
    transition_cost_tuples.Insert(tuple);
    tuple[0] = -1;
    tuple[1] = i;
    tuple[2] = -1;
    tuple[3] = i;
    tuple[4] = 0;
    transition_cost_tuples.Insert(tuple);
    // Add transition from initial state.
    tuple[0] = -1;
    tuple[1] = -1;
    tuple[2] = i;
    tuple[3] = i;
    tuple[4] = 0;
    transition_cost_tuples.Insert(tuple);

  }
  // Add initial state in case no production periods are packed at the start.
  tuple[0] = -1;
  tuple[1] = -1;
  tuple[2] = -1;
  tuple[3] = -1;
  tuple[4] = 0;
  transition_cost_tuples.Insert(tuple);
  LOG(INFO) << "  - transition cost tuple set has "
            << transition_cost_tuples.NumTuples() << " tuples";

  IntTupleSet product_tuples(2);
  for (int i = 0; i < item_to_product.size(); ++i) {
    product_tuples.Insert2(i, item_to_product[i]);
  }
  for (int i = 0; i <= num_residuals; ++i) {
    product_tuples.Insert2(num_items + i, -1);
  }
  LOG(INFO) << "  - item to product tuple set has "
            << product_tuples.NumTuples() << " tuples";

  Solver solver("acp_challenge");
  std::vector<IntVar*> products;
  solver.MakeIntVarArray(data.num_periods(), -1, data.num_products() - 1,
                         "product_", &products);
  std::vector<IntVar*> items;
  solver.MakeIntVarArray(data.num_periods(), 0, data.num_periods() - 1,
                         "item_", &items);
  std::vector<IntVar*> deliveries;
  std::vector<int> due_dates;
  LOG(INFO) << "  - build inventory costs";
  std::vector<IntVar*> inventory_costs;
  for (int i = 0; i < data.num_products(); ++i) {
    for (int j = 0; j < data.due_dates_per_product()[i].size(); ++j) {
      const int due_date = data.due_dates_per_product()[i][j];
      IntVar* const delivery =
          solver.MakeIntVar(0, due_date, StringPrintf("delivery_%d_%d", i, j));
      if (j > 0) {  // Order deliveries of the same product.
        solver.AddConstraint(solver.MakeLess(deliveries.back(), delivery));
      }
      inventory_costs.push_back(
          solver.MakeDifference(due_date, delivery)->Var());
    }
  }
  for (int i = 0; i < num_residuals; ++i) {
    IntVar* const inactive =
        solver.MakeIntVar(0, data.num_periods() - 1, "inactive");
    deliveries.push_back(inactive);
  }
  solver.AddConstraint(
      solver.MakeInversePermutationConstraint(items, deliveries));

  // Link items and tuples.
  for (int p = 0; p < data.num_periods(); ++p) {
    std::vector<IntVar*> tmp(2);
    tmp[0] = items[p];
    tmp[1] = products[p];
    solver.AddConstraint(solver.MakeAllowedAssignments(tmp, product_tuples));
  }

  LOG(INFO) << "  - build transition cost";
  // Create transition costs.
  std::vector<IntVar*> transition_costs;
  solver.MakeIntVarArray(data.num_periods() - 1, 0, max_cost, "transition_cost",
                         &transition_costs);
  std::vector<IntVar*> states;
  solver.MakeIntVarArray(data.num_periods(), -1, data.num_products() - 1,
                         "state_", &states);
  for (int p = 0; p < data.num_periods() - 1; ++p) {
    std::vector<IntVar*> tmp(5);
    tmp[0] = products[p];
    tmp[1] = states[p];
    tmp[2] = products[p + 1];
    tmp[3] = states[p + 1];
    tmp[4] = transition_costs[p];
    solver.AddConstraint(
        solver.MakeAllowedAssignments(tmp, transition_cost_tuples));
  }
  // Special rule for the first element.
  solver.AddConstraint(
      solver.MakeGreaterOrEqual(solver.MakeIsEqualCstVar(states[0], -1),
                                solver.MakeIsEqualCstVar(products[0], -1)));

  // Create objective.
  IntVar* const objective_var =
      solver.MakeSum(solver.MakeProd(solver.MakeSum(inventory_costs),
                                     data.inventory_cost()),
                     solver.MakeSum(transition_costs))->Var();
  OptimizeVar* const objective = solver.MakeMinimize(objective_var, 1);
  // Create search monitors.
  SearchMonitor* const log = solver.MakeSearchLog(1000000, objective);

  DecisionBuilder* const db = solver.MakePhase(items,
                                               Solver::CHOOSE_MIN_SIZE,
                                               Solver::ASSIGN_MIN_VALUE);

  DecisionBuilder* const random_db =
      solver.MakePhase(items,
                       Solver::CHOOSE_RANDOM,
                       Solver::ASSIGN_RANDOM_VALUE);
  SearchLimit* const lns_limit = solver.MakeFailuresLimit(FLAGS_lns_limit);
  DecisionBuilder* const inner_db = solver.MakeSolveOnce(random_db, lns_limit);

  LocalSearchOperator* swap = solver.RevAlloc(new Swap(items));
  LocalSearchOperator* random_lns = solver.MakeRandomLNSOperator(items, 10);
  std::vector<LocalSearchOperator*> operators;
  //  operators.push_back(swap);
  operators.push_back(random_lns);

  LocalSearchOperator* const moves =
      solver.ConcatenateOperators(operators);

  std::vector<LocalSearchFilter*> filters;
  // filters.push_back(solver.RevAlloc(new Filter(items, item_to_product,
  //                                              due_dates, data.transitions(),
  //                                              data.inventory_cost())));

  LocalSearchPhaseParameters* const ls_params =
      solver.MakeLocalSearchPhaseParameters(moves, inner_db, nullptr, filters);

  DecisionBuilder* const ls_db =
      solver.MakeLocalSearchPhase(items, db, ls_params);

  solver.NewSearch(ls_db, objective, log);
  while (solver.NextSolution()) {
    // for (int p = 0; p < data.num_periods(); ++p) {
    //   LOG(INFO) << StringPrintf("%d: %d %d - %s", p, items[p]->Value(),
    //                             products[p]->Value(),
    //                             states[p]->DebugString().c_str());
    // }

    // for (int i = 0; i < num_items; ++i) {
    //   LOG(INFO) << "item = " << i << ", product = " << item_to_product[i]
    //             << ", due date = " << due_dates[i] << ", delivery = "
    //             << deliveries[i]->Value() << ", cost = "
    //             << inventory_costs[i]->Value();
    // }

    std::string result;
    for (int p = 0; p < data.num_periods(); ++p) {
      result += StringPrintf("%" GG_LL_FORMAT "d ", products[p]->Value());
    }
    LOG(INFO) << result;
  }

  solver.EndSearch();
}
}  // namespace operations_research

static const char kUsage[] =
    "Usage: see flags.\nThis program runs the ACP 2014 summer school "
    "competition";

int main(int argc, char** argv) {
  FLAGS_log_prefix = false;
  google::SetUsageMessage(kUsage);
  google::ParseCommandLineFlags(&argc, &argv, true);
  if (FLAGS_input.empty()) {
    LOG(FATAL) << "Please supply a data file with --input=";
  }
  operations_research::Solve(FLAGS_input);
}
