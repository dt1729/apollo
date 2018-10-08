/******************************************************************************
 *
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file
 **/

#include "modules/planning/toolkits/optimizers/qp_piecewise_jerk_path/qp_piecewise_jerk_path_optimizer.h"

#include <algorithm>

#include "modules/common/time/time.h"
#include "modules/planning/math/finite_element_qp/active_set_augmented_lateral_qp_optimizer.h"
#include "modules/planning/math/finite_element_qp/active_set_lateral_qp_optimizer.h"

namespace apollo {
namespace planning {

using apollo::common::ErrorCode;
using apollo::common::Status;
using apollo::common::time::Clock;

namespace {
std::vector<std::pair<double, double>>::iterator min_pair_first(
    std::vector<std::pair<double, double>>::iterator begin,
    std::vector<std::pair<double, double>>::iterator end) {
  return std::min_element(begin, end, [](const std::pair<double, double>& lhs,
                                         const std::pair<double, double>& rhs) {
    return lhs.first < rhs.first;
  });
}

std::vector<std::pair<double, double>>::iterator max_pair_second(
    std::vector<std::pair<double, double>>::iterator begin,
    std::vector<std::pair<double, double>>::iterator end) {
  return std::max_element(begin, end, [](const std::pair<double, double>& lhs,
                                         const std::pair<double, double>& rhs) {
    return lhs.second < rhs.second;
  });
}

void assign_pair_first(std::vector<std::pair<double, double>>::iterator begin,
                       std::vector<std::pair<double, double>>::iterator end,
                       double first) {
  for (auto iter = begin; iter != end; ++iter) {
    iter->first = first;
  }
}

void assign_pair_second(std::vector<std::pair<double, double>>::iterator begin,
                        std::vector<std::pair<double, double>>::iterator end,
                        double second) {
  for (auto iter = begin; iter != end; ++iter) {
    iter->second = second;
  }
}

}  // namespace

QpPiecewiseJerkPathOptimizer::QpPiecewiseJerkPathOptimizer()
    : PathOptimizer("QpPiecewiseJerkPathOptimizer") {}

bool QpPiecewiseJerkPathOptimizer::Init(
    const ScenarioConfig::ScenarioTaskConfig& config) {
  if (config.has_qp_piecewise_jerk_path_config()) {
    config_ = config.qp_piecewise_jerk_path_config();
  }
  lateral_qp_optimizer_.reset(new ActiverSetLateralQPOptimizer());
  // TODO(all): use gflags or config to turn on/off new algorithms
  // lateral_qp_optimizer_.reset(new ActiverSetAugmentedLateralQPOptimizer());
  is_init_ = true;
  return true;
}

std::vector<std::pair<double, double>>
QpPiecewiseJerkPathOptimizer::GetLateralBounds(
    const SLBoundary& adc_sl, const common::FrenetFramePoint& frenet_point,
    const double qp_delta_s, double path_length,
    const ReferenceLine& reference_line,
    const std::vector<const PathObstacle*>& obstacles) {
  int size = std::max(2, static_cast<int>(path_length / qp_delta_s));
  const double buffered_adc_width =
      adc_sl.end_l() - adc_sl.start_l() + config_.lateral_buffer();
  std::vector<std::pair<double, double>> lateral_bounds(size);
  double start_s = frenet_point.s();

  // expand by road'sl or lane's l
  double accumulated_s = start_s;
  for (std::size_t i = 0; i < lateral_bounds.size();
       ++i, accumulated_s += qp_delta_s) {
    double left = 0.0;
    double right = 0.0;
    reference_line.GetLaneWidth(accumulated_s, &left, &right);
    bool adc_off_left = adc_sl.end_l() > left;  // adc is at left side of lane
    bool adc_off_right = adc_sl.start_l() < -right;
    // when ADC is not inside lane, use the min of road width and adc's l
    if (adc_off_left || adc_off_right) {
      // adc is off the lane.
      double road_left = 0.0;
      double road_right = 0.0;
      reference_line.GetRoadWidth(accumulated_s, &road_left, &road_right);
      if (adc_off_left) {  // adc is on left side of lane
        lateral_bounds[i].first = -right;
        lateral_bounds[i].second = std::min(adc_sl.end_l(), road_left);
      } else {  // adc is on right side of road
        lateral_bounds[i].first = std::max(adc_sl.start_l(), -road_right);
        lateral_bounds[i].second = left;
      }
    } else {  // use the lane's width
      lateral_bounds[i].first = -right;
      lateral_bounds[i].second = left;
    }
  }

  // shrink bounds by obstacles.
  auto find_s_iter = [&](double s) {
    std::vector<std::pair<double, double>>::iterator iter =
        lateral_bounds.begin() +
        std::min(lateral_bounds.size(),
                 static_cast<size_t>((s - start_s) / qp_delta_s));
    if (iter == lateral_bounds.end()) {
      --iter;
    }
    return iter;
  };
  for (const auto* path_obstacle :
       reference_line_info_->path_decision()->path_obstacles().Items()) {
    const auto& obstacle = *path_obstacle->obstacle();
    // only takes care of static obstacles
    if (!obstacle.IsStatic()) {
      continue;
    }
    // ignore obstacles that are not in longitudinal range.
    const auto& obstacle_sl = path_obstacle->PerceptionSLBoundary();
    if (obstacle_sl.end_s() < start_s ||
        obstacle_sl.start_s() > accumulated_s) {
      continue;
    }
    auto start_iter = find_s_iter(obstacle_sl.start_s());
    auto end_iter = find_s_iter(obstacle_sl.end_s()) + 1;
    double l_lower = max_pair_second(start_iter, end_iter)->first;
    double l_upper = min_pair_first(start_iter, end_iter)->second;
    // ignore obstacles that are not in lateral range
    if (obstacle_sl.start_l() > l_upper || obstacle_sl.end_l() < l_lower) {
      continue;
    }

    // handle parallel obstacles
    if (obstacle_sl.end_s() < adc_sl.end_s()) {
      if (obstacle_sl.start_l() > adc_sl.end_l()) {  // obstacle at left side
        assign_pair_second(start_iter, end_iter, obstacle_sl.start_l());
      } else {  // obstacle at right side
        assign_pair_first(start_iter, end_iter, obstacle_sl.end_l());
      }
      continue;
    }

    // handle general obstacles
    double left_remain = l_upper - obstacle_sl.end_l();
    double right_remain = -l_lower + obstacle_sl.start_l();
    if (left_remain > buffered_adc_width) {  // can pass at left side
      l_lower = std::max(l_lower, obstacle_sl.end_l());
    } else if (right_remain > buffered_adc_width) {  // can pass at right side
      l_upper = std::min(l_upper, obstacle_sl.start_l());
    } else {  // obstacle is blocking path
      if (obstacle_sl.start_l() * obstacle_sl.end_l() <
          0) {  // occupied ref line
        // path will not be affected by this obstacle. Very likely Path
        // Decider will stop for this obstacle.
      } else {  // reference line is not occupied by obstacle, try to bypass
        double road_left = 0.0;
        double road_right = 0.0;
        reference_line.GetRoadWidth(obstacle_sl.start_s(), &road_left,
                                    &road_right);
        if (obstacle_sl.start_l() >= 0) {  // pass from right side
          l_upper = obstacle_sl.start_l();
          l_lower =
              std::max(obstacle_sl.start_l() - buffered_adc_width, -road_right);
        } else {  // pass from left side
          l_upper =
              std::min(obstacle_sl.end_l() + buffered_adc_width, road_left);
          l_lower = obstacle_sl.end_l();
        }
      }
    }
    assign_pair_first(start_iter, end_iter, l_lower);
    assign_pair_second(start_iter, end_iter, l_upper);
  }
  return lateral_bounds;
}

Status QpPiecewiseJerkPathOptimizer::Process(
    const SpeedData& speed_data, const ReferenceLine& reference_line,
    const common::TrajectoryPoint& init_point, PathData* const path_data) {
  if (!is_init_) {
    AERROR << "Please call Init() before Process.";
    return Status(ErrorCode::PLANNING_ERROR, "Not init.");
  }
  const auto frenet_point = reference_line.GetFrenetPoint(init_point);
  const auto& adc_sl = reference_line_info_->AdcSlBoundary();
  const double qp_delta_s = config_.qp_delta_s();
  const double path_length =
      std::fmax(config_.min_look_ahead_time() * init_point.v(),
                config_.min_look_ahead_distance());
  auto lateral_bounds = GetLateralBounds(
      adc_sl, frenet_point, qp_delta_s, path_length, reference_line,
      reference_line_info_->path_decision()->path_obstacles().Items());

  std::array<double, 3> lateral_state{frenet_point.l(), frenet_point.dl(),
                                      frenet_point.ddl()};
  auto start_time = std::chrono::system_clock::now();
  bool success = lateral_qp_optimizer_->optimize(lateral_state, qp_delta_s,
                                                 lateral_bounds);
  auto end_time = std::chrono::system_clock::now();
  std::chrono::duration<double> diff = end_time - start_time;
  ADEBUG << "lateral_qp_optimizer used time: " << diff.count() * 1000 << " ms.";

  if (!success) {
    AERROR << "lateral qp optimizer failed";
    DCHECK(false);
    return Status(ErrorCode::PLANNING_ERROR, "lateral qp optimizer failed");
  }

  std::vector<common::FrenetFramePoint> frenet_path =
      lateral_qp_optimizer_->GetFrenetFramePath();

  for (auto& point : frenet_path) {
    point.set_s(frenet_point.s() + point.s());
  }
  path_data->SetReferenceLine(&reference_line);
  path_data->SetFrenetPath(FrenetFramePath(frenet_path));

  return Status::OK();
}

}  // namespace planning
}  // namespace apollo