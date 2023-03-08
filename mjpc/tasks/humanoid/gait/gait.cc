// Copyright 2022 DeepMind Technologies Limited
//
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

#include "mjpc/tasks/humanoid/gait/gait.h"

#include <mujoco/mujoco.h>

#include <iostream>
#include <string>

#include "mjpc/task.h"
#include "mjpc/utilities.h"

namespace mjpc {
std::string humanoid::Gait::XmlPath() const {
  return GetModelPath("humanoid/gait/task.xml");
}
std::string humanoid::Gait::Name() const { return "Humanoid Gait"; }

// height during flip
double humanoid::Gait::FlipHeight(double time) const {
  if (time >= jump_time_ + flight_time_ + land_time_) {
    return kHeightHumanoid + ground_;
  }
  double h = 0;
  if (time < jump_time_) {
    h = kHeightHumanoid + time * crouch_vel_ + 0.5 * time * time * jump_acc_;
  } else if (time >= jump_time_ && time < jump_time_ + flight_time_) {
    time -= jump_time_;
    h = kLeapHeight + jump_vel_ * time - 0.5 * 9.81 * time * time;
  } else if (time >= jump_time_ + flight_time_) {
    time -= jump_time_ + flight_time_;
    h = kLeapHeight - jump_vel_ * time + 0.5 * land_acc_ * time * time;
  }
  return h + ground_;
}

// orientation during flip
//  total rotation = leap + flight + land
//            2*pi = pi/2 + 5*pi/4 + pi/4
void humanoid::Gait::FlipQuat(double quat[4], double time) const {
  double angle = 0;
  if (time >= jump_time_ + flight_time_ + land_time_) {
    angle = 2 * mjPI;
  } else if (time >= crouch_time_ && time < jump_time_) {
    time -= crouch_time_;
    angle = 0.5 * jump_rot_acc_ * time * time + jump_rot_vel_ * time;
  } else if (time >= jump_time_ && time < jump_time_ + flight_time_) {
    time -= jump_time_;
    angle = mjPI / 2 + flight_rot_vel_ * time;
  } else if (time >= jump_time_ + flight_time_) {
    time -= jump_time_ + flight_time_;
    angle = 1.75 * mjPI + flight_rot_vel_ * time -
            0.5 * land_rot_acc_ * time * time;
  }
  int flip_dir = ReinterpretAsInt(parameters[flip_dir_param_id_]);
  double axis[3] = {0, flip_dir ? 1.0 : -1.0, 0};
  mju_axisAngle2Quat(quat, axis, angle);
  mju_mulQuat(quat, orientation_, quat);
}

// ------------------ Residuals for humanoid walk task ------------
//   Number of residuals: 
//   Number of parameters:
// ----------------------------------------------------------------
void humanoid::Gait::Residual(const mjModel* model, const mjData* data,
                              double* residual) const {
  int counter = 0;

  // position error
  double* torso_position = SensorByName(model, data, "torso_position");
  double* goal = SensorByName(model, data, "goal_position");
  double goal_position_error[2];

  // goal position error
  mju_sub(goal_position_error, goal, torso_position, 2);

  // set speed terms
  double vel_scaling = parameters[velscl_param_id_];
  double speed = parameters[speed_param_id_];

  // ----- height ----- //
  double torso_height = SensorByName(model, data, "torso_position")[2];
  double* foot_right = SensorByName(model, data, "foot_right");
  double* foot_left = SensorByName(model, data, "foot_left");

  if (current_mode_ == kModeHandStand) {
    foot_right = SensorByName(model, data, "hand_right");
    foot_left = SensorByName(model, data, "hand_left");
  }

  double foot_height_avg = 0.5 * (foot_right[2] + foot_left[2]);

  if (current_mode_ == kModeFlip) {
    double flip_time = data->time - mode_start_time_;
    residual[counter++] = torso_height - FlipHeight(flip_time);
  } else {
    residual[counter++] =
      (torso_height - foot_height_avg) - parameters[torso_height_param_id_];
  }

  // ----- actuation ----- //
  mju_copy(&residual[counter], data->actuator_force, model->nu);
  counter += model->nu;

  // ----- balance ----- //
  // capture point
  double* subcom = SensorByName(model, data, "torso_subcom");
  double* subcomvel = SensorByName(model, data, "torso_subcomvel");

  double capture_point[2];
  mju_addScl(capture_point, subcom, subcomvel, vel_scaling, 2);

  // convex hull
  // const int num_points = 4;
  // int hull[num_points];
  // double points[2 * num_points];
  // mju_copy(points + 0, SensorByName(model, data, "sp0"), 2);
  // mju_copy(points + 2, SensorByName(model, data, "sp1"), 2);
  // mju_copy(points + 4, SensorByName(model, data, "sp2"), 2);
  // mju_copy(points + 6, SensorByName(model, data, "sp3"), 2);
  // int num_hull = Hull2D(hull, num_points, points); 

  // // nearest point to hull
  // double nearest_point[2];
  // NearestInHull(nearest_point, capture_point, points, hull, )

  // project onto line segment

  double axis[3];
  double center[3];
  double vec[3];
  double pcp[3];
  mju_sub3(axis, foot_right, foot_left);
  axis[2] = 1.0e-3;
  double length = 0.5 * mju_normalize3(axis) - 0.05;
  mju_add3(center, foot_right, foot_left);
  mju_scl3(center, center, 0.5);
  mju_sub3(vec, capture_point, center);

  // project onto axis
  double t = mju_dot3(vec, axis);

  // clamp
  t = mju_max(-length, mju_min(length, t));
  mju_scl3(vec, axis, t);
  mju_add3(pcp, vec, center);
  pcp[2] = 1.0e-3;

  // is standing
  double standing =
      torso_height / mju_sqrt(torso_height * torso_height + 0.45 * 0.45) - 0.4;

  mju_sub(&residual[counter], capture_point, pcp, 2);
  mju_scl(&residual[counter], &residual[counter], standing, 2);

  counter += 1;

  // ----- upright ----- //
  double* torso_up = SensorByName(model, data, "torso_up");
  double* pelvis_up = SensorByName(model, data, "pelvis_up");
  double* foot_right_up = SensorByName(model, data, "foot_right_up");
  double* foot_left_up = SensorByName(model, data, "foot_left_up");

  if (current_mode_ == kModeFlip) {
    // special handling of flip orientation
    double flip_time = data->time - mode_start_time_;
    double quat[4];
    FlipQuat(quat, flip_time);
    double* torso_xquat = data->xquat + 4 * torso_body_id_;
    mju_subQuat(residual + counter, torso_xquat, quat);
    counter += 3;

    // zero remaining upright terms
    mju_zero(&residual[counter], 5);
    counter += 5;
  } else {
    // torso
    if (current_mode_ == kModeHandStand) {
      residual[counter++] = torso_up[2] + 1.0;

      // pelvis
      residual[counter++] = 0.3 * (pelvis_up[2] + 1.0);

      double z_ref[3] = {0.0, 0.0, -1.0};

      // right foot
      mju_sub3(&residual[counter], foot_right_up, z_ref);
      mju_scl3(&residual[counter], &residual[counter], 0.1 * standing);
      counter += 3;

      mju_sub3(&residual[counter], foot_left_up, z_ref);
      mju_scl3(&residual[counter], &residual[counter], 0.1 * standing);
      counter += 3;
    } else {
      residual[counter++] = torso_up[2] - 1.0;

      // pelvis
      residual[counter++] = 0.3 * (pelvis_up[2] - 1.0);

      double z_ref[3] = {0.0, 0.0, 1.0};

      // right foot
      mju_sub3(&residual[counter], foot_right_up, z_ref);
      mju_scl3(&residual[counter], &residual[counter], 0.1 * standing);
      counter += 3;

      mju_sub3(&residual[counter], foot_left_up, z_ref);
      mju_scl3(&residual[counter], &residual[counter], 0.1 * standing);
      counter += 3;
    }
  }

  // ----- posture ----- //
  if (current_mode_ == kModeFlip) {
    mju_sub(&residual[counter], data->qpos + 7,
            model->key_qpos + qpos_flip_crouch_id_ * model->nq + 7, model->nq - 7);
  } else {
    if (current_mode_ == kModeHandStand) {
      mju_sub(&residual[counter], data->qpos + 7,
            model->key_qpos + qpos_handstand_id_ * model->nq + 7, model->nq - 7);
    } else {
      mju_sub(&residual[counter], data->qpos + 7,
                model->key_qpos + qpos_reference_id_ * model->nq + 7, model->nq - 7);
    }
  }
  counter += model->nq - 7;

  // ----- goal ----- //

  // ----- position error ----- //
  mju_copy(&residual[counter], goal_position_error, 2);
  counter += 2;

  // ----- orientation error ----- //
  // direction to goal
  double goal_direction[2];
  mju_copy(goal_direction, goal_position_error, 2);
  mju_normalize(goal_direction, 2);

  // torso direction
  double* torso_xaxis = SensorByName(model, data, "torso_xaxis");

  mju_sub(&residual[counter], goal_direction, torso_xaxis, 2);
  counter += 2;

  // ----- walk ----- //
  double* torso_forward = SensorByName(model, data, "torso_forward");
  double* pelvis_forward = SensorByName(model, data, "pelvis_forward");
  double* foot_right_forward = SensorByName(model, data, "foot_right_forward");
  double* foot_left_forward = SensorByName(model, data, "foot_left_forward");

  double forward[2];
  mju_copy(forward, torso_forward, 2);
  mju_addTo(forward, pelvis_forward, 2);
  mju_addTo(forward, foot_right_forward, 2);
  mju_addTo(forward, foot_left_forward, 2);
  mju_normalize(forward, 2);

  // com vel
  double* waist_lower_subcomvel =
      SensorByName(model, data, "waist_lower_subcomvel");
  double* torso_velocity = SensorByName(model, data, "torso_velocity");
  double com_vel[2];
  mju_add(com_vel, waist_lower_subcomvel, torso_velocity, 2);
  mju_scl(com_vel, com_vel, 0.5, 2);

  // walk forward
  residual[counter++] = standing * (mju_dot(com_vel, forward, 2) - speed);

  // ----- move feet ----- //
  double* foot_right_vel = SensorByName(model, data, "foot_right_velocity");
  double* foot_left_vel = SensorByName(model, data, "foot_left_velocity");
  double move_feet[2];
  mju_copy(move_feet, com_vel, 2);
  mju_addToScl(move_feet, foot_right_vel, -0.5, 2);
  mju_addToScl(move_feet, foot_left_vel, -0.5, 2);

  mju_copy(&residual[counter], move_feet, 2);
  mju_scl(&residual[counter], &residual[counter], standing, 2);

  counter += 2;

  // ----- gait ----- //
  double step[2];
  FootStep(step, GetPhase(data->time));

  double foot_pos[2][3];
  mju_copy(foot_pos[0], foot_left, 3);
  mju_copy(foot_pos[1], foot_right, 3);

  for (int i = 0; i < 2; i++) {
    double query[3] = {foot_pos[i][0], foot_pos[i][1], foot_pos[i][2]};
    double ground_height = Ground(model, data, query);
    double height_target = ground_height + 0.025 + step[i];
    double height_difference = foot_pos[i][2] - height_target;
    residual[counter++] = step[i] ? height_difference : 0;
  }

  // sensor dim sanity check
  CheckSensorDim(model, counter);
}

// transition
void humanoid::Gait::Transition(const mjModel* model, mjData* data) {
  // set weights and residual parameters
  if (stage != current_mode_) {
    mju_copy(weight.data(), kModeWeight[stage], weight.size());
    mju_copy(parameters.data(), kModeParameter[stage], 6);
  }

  // ---------- handle mjData reset ----------
  if (data->time < last_transition_time_ || last_transition_time_ == -1) {
    if (stage != kModeStand && stage != kModeHandStand) {
      stage = kModeStand;  // stage is stateful, switch to Quadruped
    }
    last_transition_time_ = phase_start_time_ = phase_start_ = data->time;
  }

  // ---------- prevent forbidden stage transitions ----------
  // switching stage, not from humanoid
  if (stage != current_mode_ && current_mode_ != kModeStand) {
    // switch into stateful stage only allowed from Quadruped
    if (stage == kModeWalk || stage == kModeFlip) {
      stage = kModeStand;
    }
  }

  // ---------- handle phase velocity change ----------
  double phase_velocity = 2 * mjPI * parameters[cadence_param_id_];
  if (phase_velocity != phase_velocity_) {
    phase_start_ = GetPhase(data->time);
    phase_start_time_ = data->time;
    phase_velocity_ = phase_velocity;
  }

  // ---------- Walk ----------
  double* goal_pos = data->mocap_pos + 3 * goal_mocap_id_;

  // ---------- Flip ----------
  double* compos = SensorByName(model, data, "torso_subcom");
  if (stage == kModeFlip) {
    // switching into Flip, reset task state
    if (stage != current_mode_) {
      // save time
      mode_start_time_ = data->time;

      // save body orientation, ground height
      mju_copy4(orientation_, data->xquat + 4 * torso_body_id_);
      ground_ = Ground(model, data, compos);

      // save parameters
      save_weight_ = weight;
      save_gait_switch_ = parameters[gait_switch_param_id_];
    }

    // time from start of Flip
    double flip_time = data->time - mode_start_time_;

    if (flip_time >= jump_time_ + flight_time_ + land_time_) {
      // Flip ended, back to Humanoid, restore values
      stage = kModeStand;
      weight = save_weight_;
      parameters[gait_switch_param_id_] = save_gait_switch_;
      goal_pos[0] = data->site_xpos[3 * torso_body_id_ + 0];
      goal_pos[1] = data->site_xpos[3 * torso_body_id_ + 1];

      // reset time
      last_transition_time_ = data->time;

      // return before setting current_mode_
      return;
    }
  }

  // ----- handstand ----- // 
  if (current_mode_ != kModeHandStand && hand_stand_phase_ != 0) {
    hand_stand_phase_ = 0;
  }
  if (stage == kModeHandStand) {
    // initialization
    if (stage != current_mode_) {
      // printf("reset handstand mode\n");
      hand_stand_phase_ = 0;
      weight[CostTermByName(model, "Height")] = 0.0;
      weight[CostTermByName(model, "Actuation")] = 0.1;
      weight[CostTermByName(model, "Balance")] = 0.0;
      weight[CostTermByName(model, "Upright")] = 0.0;
      weight[CostTermByName(model, "Posture")] = 1.0;
      parameters[torso_height_param_id_] = 1.3;
      parameters[amplitude_param_id_] = 0.0;
      qpos_handstand_id_ = 2;
      hand_stand_time_ = 0.0;
    }

    // crouch
    if (hand_stand_phase_ == 0) {
      double position_error[26];
      mju_sub(position_error, data->qpos + 2, model->key_qpos + qpos_handstand_id_ * model->nq + 2, 26);
      position_error[0] *= 0.5;
      // check near crouch position
      if (mju_norm(position_error, 26) / 26 < 0.05) {
        // reset crouch time
        if (hand_stand_time_ == 0.0) {
          hand_stand_time_ = data->time;
        }

        // check duration of crouch
        if (data->time - hand_stand_time_ > 0.5) {
          // set costs and parameters
          weight[CostTermByName(model, "Height")] = 1.0;
          weight[CostTermByName(model, "Actuation")] = 0.005;
          weight[CostTermByName(model, "Balance")] = 1.0;
          weight[CostTermByName(model, "Upright")] = 1.0;
          weight[CostTermByName(model, "Posture")] = 0.075;
          parameters[torso_height_param_id_] = 0.645;
          parameters[amplitude_param_id_] = 0.0;
          qpos_handstand_id_ = 3;

          // reset time
          hand_stand_time_ = 0.0;

          // handstand phase
          hand_stand_phase_ = 1;
        }
      }
    } 
    // in handstand
    if (hand_stand_phase_ == 1) {
      // set amplitude
      if (data->time - hand_stand_time_ > 2.5) {
        parameters[amplitude_param_id_] = 0.1;
        hand_stand_time_ = 0.0;
      }
    }
  } else {
    // reset
    hand_stand_phase_ = 0;
  }

  // save stage
  current_mode_ = static_cast<HumanoidMode>(stage);
  last_transition_time_ = data->time;
}

// reset humanoid task
void humanoid::Gait::Reset(const mjModel* model) {
  // call method from base class
  Task::Reset(model);

  // ----------  task identifiers  ----------
  flip_dir_param_id_ = ParameterIndex(model, "select_Flip dir");
  torso_height_param_id_ = ParameterIndex(model, "Torso");
  speed_param_id_ = ParameterIndex(model, "Speed");
  velscl_param_id_ = ParameterIndex(model, "VelScl");
  cadence_param_id_ = ParameterIndex(model, "Cadence");
  amplitude_param_id_ = ParameterIndex(model, "Amplitude");
  duty_param_id_ = ParameterIndex(model, "DutyRatio");
  balance_cost_id_ = CostTermByName(model, "Balance");
  upright_cost_id_ = CostTermByName(model, "Upright");
  height_cost_id_ = CostTermByName(model, "Height");
  qpos_reference_id_ = 0;
  qpos_flip_crouch_id_ = 1;
  qpos_handstand_id_ = 2;

  // ----------  model identifiers  ----------
  torso_body_id_ = mj_name2id(model, mjOBJ_XBODY, "torso");
  if (torso_body_id_ < 0) mju_error("body 'torso' not found");

  head_site_id_ = mj_name2id(model, mjOBJ_XBODY, "head");
  if (head_site_id_ < 0) mju_error("body 'head' not found");

  int goal_id = mj_name2id(model, mjOBJ_XBODY, "goal");
  if (goal_id < 0) mju_error("body 'goal' not found");

  goal_mocap_id_ = model->body_mocapid[goal_id];
  if (goal_mocap_id_ < 0) mju_error("body 'goal' is not mocap");

  // foot geom ids
  // int foot_index = 0;
  // for (const char* footname : {"FL", "HL", "FR", "HR"}) {
  //   int foot_id = mj_name2id(model, mjOBJ_GEOM, footname);
  //   if (foot_id < 0) mju_error_s("geom '%s' not found", footname);
  //   foot_geom_id_[foot_index] = foot_id;
  //   foot_index++;
  // }

  // shoulder body ids
  // int shoulder_index = 0;
  // for (const char* shouldername : {"FL_hip", "HL_hip", "FR_hip", "HR_hip"}) {
  //   int foot_id = mj_name2id(model, mjOBJ_BODY, shouldername);
  //   if (foot_id < 0) mju_error_s("body '%s' not found", shouldername);
  //   shoulder_body_id_[shoulder_index] = foot_id;
  //   shoulder_index++;
  // }

  // ----------  derived kinematic quantities for Flip  ----------
  gravity_ = mju_norm3(model->opt.gravity);
  // velocity at takeoff
  jump_vel_ = mju_sqrt(2 * gravity_ * (kMaxHeight - kLeapHeight));
  // time in flight phase
  flight_time_ = 2 * jump_vel_ / gravity_;
  // acceleration during jump phase
  jump_acc_ = jump_vel_ * jump_vel_ / (2 * (kLeapHeight - kCrouchHeight));
  // time in crouch sub-phase of jump
  crouch_time_ = mju_sqrt(2 * (kHeightHumanoid - kCrouchHeight) / jump_acc_);
  // time in leap sub-phase of jump
  leap_time_ = jump_vel_ / jump_acc_;
  // jump total time
  jump_time_ = crouch_time_ + leap_time_;
  // velocity at beginning of crouch
  crouch_vel_ = -jump_acc_ * crouch_time_;
  // time of landing phase
  land_time_ = 2 * (kLeapHeight - kHeightHumanoid) / jump_vel_;
  // acceleration during landing
  land_acc_ = jump_vel_ / land_time_;
  // rotational velocity during flight phase (rotates 1.25 pi)
  flight_rot_vel_ = 1.25 * mjPI / flight_time_;
  // rotational velocity at start of leap (rotates 0.5 pi)
  jump_rot_vel_ = mjPI / leap_time_ - flight_rot_vel_;
  // rotational acceleration during leap (rotates 0.5 pi)
  jump_rot_acc_ = (flight_rot_vel_ - jump_rot_vel_) / leap_time_;
  // rotational deceleration during land (rotates 0.25 pi)
  land_rot_acc_ =
      2 * (flight_rot_vel_ * land_time_ - mjPI / 4) / (land_time_ * land_time_);
}

// colors of visualisation elements drawn in ModifyScene()
constexpr float kFlipRgba[4] = {0, 1, 0, 0.5};       // flip body
constexpr float kCapRgba[4] = {1.0, 0.0, 1.0, 1.0};  // capture point
constexpr float kHullRgba[4] = {1.0, 0.0, 0.0, 1};   // convex hull
constexpr float kPcpRgba[4] = {1.0, 0.0, 0.0, 1.0};  // projected capture point

// draw task-related geometry in the scene
void humanoid::Gait::ModifyScene(const mjModel* model, const mjData* data,
                                 mjvScene* scene) const {
  // flip target pose
  if (current_mode_ == kModeFlip) {
    double flip_time = data->time - mode_start_time_;
    double* torso_pos = data->xpos + 3 * torso_body_id_;
    double pos[3] = {torso_pos[0], torso_pos[1], FlipHeight(flip_time)};
    double quat[4];
    FlipQuat(quat, flip_time);
    double mat[9];
    mju_quat2Mat(mat, quat);
    double size[3] = {0.05, 0.15, 0.25};
    AddGeom(scene, mjGEOM_BOX, size, pos, mat, kFlipRgba);

    // don't draw anything else during flip
    return;
  }
  
  // feet site positions (xy plane)
  double foot_pos[4][3];
  mju_copy(foot_pos[0], SensorByName(model, data, "sp0"), 3);
  mju_copy(foot_pos[1], SensorByName(model, data, "sp1"), 3);
  mju_copy(foot_pos[2], SensorByName(model, data, "sp2"), 3);
  mju_copy(foot_pos[3], SensorByName(model, data, "sp3"), 3);
  foot_pos[0][2] = 0.0;
  foot_pos[1][2] = 0.0;
  foot_pos[2][2] = 0.0;
  foot_pos[3][2] = 0.0;

  // support polygon
  double polygon[8];
  for (int i = 0; i < 4; i++) {
    polygon[2 * i] = foot_pos[i][0];
    polygon[2 * i + 1] = foot_pos[i][1];
  }
  int hull[4];
  int num_hull = Hull2D(hull, 4, polygon);

  // draw connectors
  for (int i = 0; i < num_hull; i++) {
    int j = (i + 1) % num_hull;
    AddConnector(scene, mjGEOM_CAPSULE, 0.015, foot_pos[hull[i]],
                 foot_pos[hull[j]], kHullRgba);
  }

  // capture point
  double fall_time = mju_sqrt(2.0 * parameters[torso_height_param_id_] /
                              mju_norm(model->opt.gravity, 3));
  double capture[3];
  double* compos = SensorByName(model, data, "torso_subcom");
  double* comvel = SensorByName(model, data, "torso_subcomvel");
  mju_addScl3(capture, compos, comvel, fall_time);

  // ground under CoM
  double com_ground = Ground(model, data, compos);

  // capture point
  double foot_size[3] = {kFootRadius, 0, 0};

  capture[2] = com_ground;

  AddGeom(scene, mjGEOM_SPHERE, foot_size, capture, /*mat=*/nullptr, kCapRgba);

  // capture point, projected onto hull
  double pcp2[2];
  NearestInHull(pcp2, capture, polygon, hull, num_hull);
  double pcp[3] = {pcp2[0], pcp2[1], com_ground};
  AddGeom(scene, mjGEOM_SPHERE, foot_size, pcp, /*mat=*/nullptr, kPcpRgba);
}

// return phase as a function of time
double humanoid::Gait::GetPhase(double time) const {
  return phase_start_ + (time - phase_start_time_) * phase_velocity_;
}

// return normalized target step height
double humanoid::Gait::StepHeight(double time, double footphase,
                                  double duty_ratio) const {
  double angle = std::fmod(time + mjPI - footphase, 2 * mjPI) - mjPI;
  double value = 0;
  if (duty_ratio < 1) {
    angle *= 0.5 / (1 - duty_ratio);
    value = mju_cos(mju_clip(angle, -mjPI / 2, mjPI / 2));
  }
  return mju_abs(value) < 1e-6 ? 0.0 : value;
}

// compute target step height for all feet
void humanoid::Gait::FootStep(double* step, double time) const {
  double amplitude = parameters[amplitude_param_id_];
  double duty_ratio = parameters[duty_param_id_];
  double gait_phase[2] = {0.0, 0.5};
  for (int i = 0; i < 2; i++) {
    double footphase = 2 * mjPI * gait_phase[i];
    step[i] = amplitude * StepHeight(time, footphase, duty_ratio);
  }
}

}  // namespace mjpc
