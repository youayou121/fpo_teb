/*********************************************************************
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2016,
 *  TU Dortmund - Institute of Control Theory and Systems Engineering.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the institute nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Christoph Rösmann
 *********************************************************************/

#include <teb_local_planner/optimal_planner.h>

// g2o custom edges and vertices for the TEB planner
#include <teb_local_planner/g2o_types/edge_velocity.h>
#include <teb_local_planner/g2o_types/edge_velocity_obstacle_ratio.h>
#include <teb_local_planner/g2o_types/edge_acceleration.h>
#include <teb_local_planner/g2o_types/edge_kinematics.h>
#include <teb_local_planner/g2o_types/edge_time_optimal.h>
#include <teb_local_planner/g2o_types/edge_shortest_path.h>
#include <teb_local_planner/g2o_types/edge_obstacle.h>
#include <teb_local_planner/g2o_types/edge_dynamic_obstacle.h>
#include <teb_local_planner/g2o_types/edge_via_point.h>
#include <teb_local_planner/g2o_types/edge_prefer_rotdir.h>

#include <memory>
#include <limits>
#include "opencv2/video/tracking.hpp"
#include "opencv2/highgui.hpp"
#include "opencv2/core/cvdef.h"
#include <stdio.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <std_msgs/Float32MultiArray.h>
using namespace std;
using namespace cv;
namespace teb_local_planner
{

  // ============== Implementation ===================

  TebOptimalPlanner::TebOptimalPlanner() : cfg_(NULL), obstacles_(NULL), via_points_(NULL), cost_(HUGE_VAL), prefer_rotdir_(RotType::none),
                                           robot_model_(new PointRobotFootprint()), initialized_(false), optimized_(false)
  {
  }

  TebOptimalPlanner::TebOptimalPlanner(const TebConfig &cfg, ObstContainer *obstacles, RobotFootprintModelPtr robot_model, TebVisualizationPtr visual, const ViaPointContainer *via_points)
  {
    ros::NodeHandle nh;
    pos_sub = nh.subscribe("pos", 1, &TebOptimalPlanner::customObstaclePosCB, this);
    sub_costmap = nh.subscribe("move_base/global_costmap/costmap", 1, &TebOptimalPlanner::costmapCB, this);
    initialize(cfg, obstacles, robot_model, visual, via_points);
  }
  TebOptimalPlanner::~TebOptimalPlanner()
  {
    clearGraph();
    // free dynamically allocated memory
    // if (optimizer_)
    //  g2o::Factory::destroy();
    // g2o::OptimizationAlgorithmFactory::destroy();
    // g2o::HyperGraphActionLibrary::destroy();
  }

  void TebOptimalPlanner::customObstaclePosCB(const std_msgs::Float32MultiArray obpos)
  {
    this->obpos = obpos;
    // std::cout << this->obpos << std::endl;
  }

  void TebOptimalPlanner::costmapCB(const nav_msgs::OccupancyGridConstPtr &map)
  {
    if (map->data.size() > 0)
    {
      this->g_map = *map;
      cout << "height: " << this->g_map.info.height << " width: " << this->g_map.info.width
           << " origin_x:" << this->g_map.info.origin.position.x << " origin_y:" << this->g_map.info.origin.position.y
           << " resolution: " << this->g_map.info.resolution
           << endl;
    }
  }

  bool TebOptimalPlanner::is_static(int i)
  {
    for (int j = 0; j < 20; j++)
    {
      int i_left = i - j;
      int i_right = i + j;
      int i_up = i - j * this->g_map.info.width;
      int i_down = i + j * this->g_map.info.width;
      if (int(*(this->g_map.data.begin() + i_left)) > 0)
      {
        return true;
        if (int(*(this->g_map.data.begin() + i_right)) > 0)
        {
          return true;
          if (int(*(this->g_map.data.begin() + i_up)) > 0)
          {
            return true;
            if (int(*(this->g_map.data.begin() + i_down)) > 0)
            {
              return true;
            }
          }
        }
      }
    }
    return false;
  }

  void TebOptimalPlanner::updateRobotModel(RobotFootprintModelPtr robot_model)
  {
    robot_model_ = robot_model;
  }

  void TebOptimalPlanner::initialize(const TebConfig &cfg, ObstContainer *obstacles, RobotFootprintModelPtr robot_model, TebVisualizationPtr visual, const ViaPointContainer *via_points)
  {
    // init optimizer (set solver and block ordering settings)
    optimizer_ = initOptimizer();

    cfg_ = &cfg;
    obstacles_ = obstacles;
    robot_model_ = robot_model;
    via_points_ = via_points;
    cost_ = HUGE_VAL;
    prefer_rotdir_ = RotType::none;
    setVisualization(visual);

    vel_start_.first = true;
    vel_start_.second.linear.x = 0;
    vel_start_.second.linear.y = 0;
    vel_start_.second.angular.z = 0;

    vel_goal_.first = true;
    vel_goal_.second.linear.x = 0;
    vel_goal_.second.linear.y = 0;
    vel_goal_.second.angular.z = 0;
    initialized_ = true;
  }

  void TebOptimalPlanner::setVisualization(TebVisualizationPtr visualization)
  {
    visualization_ = visualization;
  }

  void TebOptimalPlanner::visualize()
  {
    if (!visualization_)
      return;

    visualization_->publishLocalPlanAndPoses(teb_);

    if (teb_.sizePoses() > 0)
      visualization_->publishRobotFootprintModel(teb_.Pose(0), *robot_model_);

    if (cfg_->trajectory.publish_feedback)
      visualization_->publishFeedbackMessage(*this, *obstacles_);
  }

  /*
   * registers custom vertices and edges in g2o framework
   */

  void TebOptimalPlanner::registerG2OTypes()
  {
    g2o::Factory *factory = g2o::Factory::instance();
    factory->registerType("VERTEX_POSE", new g2o::HyperGraphElementCreator<VertexPose>);
    factory->registerType("VERTEX_TIMEDIFF", new g2o::HyperGraphElementCreator<VertexTimeDiff>);

    factory->registerType("EDGE_TIME_OPTIMAL", new g2o::HyperGraphElementCreator<EdgeTimeOptimal>);
    factory->registerType("EDGE_SHORTEST_PATH", new g2o::HyperGraphElementCreator<EdgeShortestPath>);
    factory->registerType("EDGE_VELOCITY", new g2o::HyperGraphElementCreator<EdgeVelocity>);
    factory->registerType("EDGE_VELOCITY_HOLONOMIC", new g2o::HyperGraphElementCreator<EdgeVelocityHolonomic>);
    factory->registerType("EDGE_ACCELERATION", new g2o::HyperGraphElementCreator<EdgeAcceleration>);
    factory->registerType("EDGE_ACCELERATION_START", new g2o::HyperGraphElementCreator<EdgeAccelerationStart>);
    factory->registerType("EDGE_ACCELERATION_GOAL", new g2o::HyperGraphElementCreator<EdgeAccelerationGoal>);
    factory->registerType("EDGE_ACCELERATION_HOLONOMIC", new g2o::HyperGraphElementCreator<EdgeAccelerationHolonomic>);
    factory->registerType("EDGE_ACCELERATION_HOLONOMIC_START", new g2o::HyperGraphElementCreator<EdgeAccelerationHolonomicStart>);
    factory->registerType("EDGE_ACCELERATION_HOLONOMIC_GOAL", new g2o::HyperGraphElementCreator<EdgeAccelerationHolonomicGoal>);
    factory->registerType("EDGE_KINEMATICS_DIFF_DRIVE", new g2o::HyperGraphElementCreator<EdgeKinematicsDiffDrive>);
    factory->registerType("EDGE_KINEMATICS_CARLIKE", new g2o::HyperGraphElementCreator<EdgeKinematicsCarlike>);
    factory->registerType("EDGE_OBSTACLE", new g2o::HyperGraphElementCreator<EdgeObstacle>);
    factory->registerType("EDGE_INFLATED_OBSTACLE", new g2o::HyperGraphElementCreator<EdgeInflatedObstacle>);
    factory->registerType("EDGE_DYNAMIC_OBSTACLE", new g2o::HyperGraphElementCreator<EdgeDynamicObstacle>);
    factory->registerType("EDGE_VIA_POINT", new g2o::HyperGraphElementCreator<EdgeViaPoint>);
    factory->registerType("EDGE_PREFER_ROTDIR", new g2o::HyperGraphElementCreator<EdgePreferRotDir>);
    return;
  }

  /*
   * initialize g2o optimizer. Set solver settings here.
   * Return: pointer to new SparseOptimizer Object.
   */
  boost::shared_ptr<g2o::SparseOptimizer> TebOptimalPlanner::initOptimizer()
  {
    // Call register_g2o_types once, even for multiple TebOptimalPlanner instances (thread-safe)
    static boost::once_flag flag = BOOST_ONCE_INIT;
    boost::call_once(&registerG2OTypes, flag);

    // allocating the optimizer
    boost::shared_ptr<g2o::SparseOptimizer> optimizer = boost::make_shared<g2o::SparseOptimizer>();
    std::unique_ptr<TEBLinearSolver> linear_solver(new TEBLinearSolver()); // see typedef in optimization.h
    linear_solver->setBlockOrdering(true);
    std::unique_ptr<TEBBlockSolver> block_solver(new TEBBlockSolver(std::move(linear_solver)));
    g2o::OptimizationAlgorithmLevenberg *solver = new g2o::OptimizationAlgorithmLevenberg(std::move(block_solver));

    optimizer->setAlgorithm(solver);

    optimizer->initMultiThreading(); // required for >Eigen 3.1

    return optimizer;
  }

  bool TebOptimalPlanner::optimizeTEB(int iterations_innerloop, int iterations_outerloop, bool compute_cost_afterwards,
                                      double obst_cost_scale, double viapoint_cost_scale, bool alternative_time_cost)
  {
    if (cfg_->optim.optimization_activate == false)
      return false;

    bool success = false;
    optimized_ = false;

    double weight_multiplier = 1.0;

    // TODO(roesmann): we introduced the non-fast mode with the support of dynamic obstacles
    //                (which leads to better results in terms of x-y-t homotopy planning).
    //                 however, we have not tested this mode intensively yet, so we keep
    //                 the legacy fast mode as default until we finish our tests.
    bool fast_mode = !cfg_->obstacles.include_dynamic_obstacles;

    for (int i = 0; i < iterations_outerloop; ++i)
    {
      if (cfg_->trajectory.teb_autosize)
      {
        // teb_.autoResize(cfg_->trajectory.dt_ref, cfg_->trajectory.dt_hysteresis, cfg_->trajectory.min_samples, cfg_->trajectory.max_samples);
        teb_.autoResize(cfg_->trajectory.dt_ref, cfg_->trajectory.dt_hysteresis, cfg_->trajectory.min_samples, cfg_->trajectory.max_samples, fast_mode);
      }

      success = buildGraph(weight_multiplier);
      if (!success)
      {
        clearGraph();
        return false;
      }
      success = optimizeGraph(iterations_innerloop, false);
      if (!success)
      {
        clearGraph();
        return false;
      }
      optimized_ = true;

      if (compute_cost_afterwards && i == iterations_outerloop - 1) // compute cost vec only in the last iteration
        computeCurrentCost(obst_cost_scale, viapoint_cost_scale, alternative_time_cost);

      clearGraph();
      // std::cout<<"clearGraph"<<std::endl;

      weight_multiplier *= cfg_->optim.weight_adapt_factor;
    }

    return true;
  }

  void TebOptimalPlanner::setVelocityStart(const geometry_msgs::Twist &vel_start)
  {
    vel_start_.first = true;
    vel_start_.second.linear.x = vel_start.linear.x;
    vel_start_.second.linear.y = vel_start.linear.y;
    vel_start_.second.angular.z = vel_start.angular.z;
  }

  void TebOptimalPlanner::setVelocityGoal(const geometry_msgs::Twist &vel_goal)
  {
    vel_goal_.first = true;
    vel_goal_.second = vel_goal;
  }

  bool TebOptimalPlanner::plan(const std::vector<geometry_msgs::PoseStamped> &initial_plan, const geometry_msgs::Twist *start_vel, bool free_goal_vel)
  {
    ROS_ASSERT_MSG(initialized_, "Call initialize() first.");
    if (!teb_.isInit())
    {
      teb_.initTrajectoryToGoal(initial_plan, cfg_->robot.max_vel_x, cfg_->robot.max_vel_theta, cfg_->trajectory.global_plan_overwrite_orientation,
                                cfg_->trajectory.min_samples, cfg_->trajectory.allow_init_with_backwards_motion);
    }
    else // warm start
    {
      PoseSE2 start_(initial_plan.front().pose);
      PoseSE2 goal_(initial_plan.back().pose);
      double start_x = teb_.Pose(0).x();
      double start_y = teb_.Pose(0).y();
      double end_x = teb_.BackPose().x();
      double end_y = teb_.BackPose().y();
      double start_theta = teb_.Pose(0).theta();
      double vector1x = end_x - start_x;
      double vector1y = end_y - start_y;
      double angle = atan2(vector1y, vector1x);
      // std::cout << start_x << " " << start_y << " "
      // << " " << end_x << " " << end_y << " " << start_theta << " " << angle << std::endl;
      // if (teb_.sizePoses() > 0 && (goal_.position() - teb_.BackPose().position()).norm() < cfg_->trajectory.force_reinit_new_goal_dist && fabs(g2o::normalize_theta(goal_.theta() - teb_.BackPose().theta())) < cfg_->trajectory.force_reinit_new_goal_angular) // actual warm start!
      // if (teb_.sizePoses() > 0 && (fabs(angle - start_theta) > 0.685 && this->dynamic_obstacle_in_ == false || (teb_.Pose(0).position() - teb_.BackPose().position()).norm() < 0.7)) // actual warm start!
      // if (teb_.sizePoses() > 0 && (teb_.Pose(0).position() - teb_.BackPose().position()).norm() < 0.7)
      bool update_flag = false;
      if (this->update_rate % 1 == 0)
      {
        update_flag = true;
        if (this->update_rate > 10000)
        {
          this->update_rate = 0;
        }
      }
      this->update_rate++;
      int update_mode = 0;
      ros::param::get("update_mode",update_mode);
      if (update_mode)
      {
        if (this->dynamic_obstacle_in_ == true && update_flag && (teb_.Pose(0).position() - teb_.BackPose().position()).norm() > 1)
        {
          ROS_DEBUG("New goal: distance to existing goal is higher than the specified threshold. Reinitalizing trajectories.");
          // std::cout << "clear" << std::endl;
          teb_.clearTimedElasticBand();
          teb_.initTrajectoryToGoal(initial_plan, cfg_->robot.max_vel_x, cfg_->robot.max_vel_theta, cfg_->trajectory.global_plan_overwrite_orientation,
                                    cfg_->trajectory.min_samples, cfg_->trajectory.allow_init_with_backwards_motion);
        }
        else
        {
          teb_.updateAndPruneTEB(start_, goal_, cfg_->trajectory.min_samples);
          // std::cout << "update" << std::endl;
        }
      }
      else
      {
        if (teb_.sizePoses() > 0 && (goal_.position() - teb_.BackPose().position()).norm() < cfg_->trajectory.force_reinit_new_goal_dist && fabs(g2o::normalize_theta(goal_.theta() - teb_.BackPose().theta())) < cfg_->trajectory.force_reinit_new_goal_angular) // actual warm start!
          teb_.updateAndPruneTEB(start_, goal_, cfg_->trajectory.min_samples);                                                                                                                                                                                    // update TEB
        else                                                                                                                                                                                                                                                      // goal too far away -> reinit
        {
          ROS_DEBUG("New goal: distance to existing goal is higher than the specified threshold. Reinitalizing trajectories.");
          teb_.clearTimedElasticBand();
          teb_.initTrajectoryToGoal(initial_plan, cfg_->robot.max_vel_x, cfg_->robot.max_vel_theta, cfg_->trajectory.global_plan_overwrite_orientation,
                                    cfg_->trajectory.min_samples, cfg_->trajectory.allow_init_with_backwards_motion);
        }
      }
    }
    if (start_vel)
      setVelocityStart(*start_vel);
    if (free_goal_vel)
      setVelocityGoalFree();
    else
      vel_goal_.first = true; // we just reactivate and use the previously set velocity (should be zero if nothing was modified)

    // now optimize
    return optimizeTEB(cfg_->optim.no_inner_iterations, cfg_->optim.no_outer_iterations);
    // return true;
  }

  bool TebOptimalPlanner::plan(const tf::Pose &start, const tf::Pose &goal, const geometry_msgs::Twist *start_vel, bool free_goal_vel)
  {
    PoseSE2 start_(start);
    PoseSE2 goal_(goal);
    return plan(start_, goal_, start_vel);
  }

  bool TebOptimalPlanner::plan(const PoseSE2 &start, const PoseSE2 &goal, const geometry_msgs::Twist *start_vel, bool free_goal_vel)
  {
    ROS_ASSERT_MSG(initialized_, "Call initialize() first.");
    if (!teb_.isInit())
    {
      // init trajectory
      teb_.initTrajectoryToGoal(start, goal, 0, cfg_->robot.max_vel_x, cfg_->trajectory.min_samples, cfg_->trajectory.allow_init_with_backwards_motion); // 0 intermediate samples, but dt=1 -> autoResize will add more samples before calling first optimization
    }
    else // warm start
    {
      if (teb_.sizePoses() > 0 && (goal.position() - teb_.BackPose().position()).norm() < cfg_->trajectory.force_reinit_new_goal_dist && fabs(g2o::normalize_theta(goal.theta() - teb_.BackPose().theta())) < cfg_->trajectory.force_reinit_new_goal_angular) // actual warm start!
        teb_.updateAndPruneTEB(start, goal, cfg_->trajectory.min_samples);
      else // goal too far away -> reinit
      {
        ROS_DEBUG("New goal: distance to existing goal is higher than the specified threshold. Reinitalizing trajectories.");
        teb_.clearTimedElasticBand();
        teb_.initTrajectoryToGoal(start, goal, 0, cfg_->robot.max_vel_x, cfg_->trajectory.min_samples, cfg_->trajectory.allow_init_with_backwards_motion);
      }
    }
    if (start_vel)
      setVelocityStart(*start_vel);
    if (free_goal_vel)
      setVelocityGoalFree();
    else
      vel_goal_.first = true; // we just reactivate and use the previously set velocity (should be zero if nothing was modified)

    // now optimize
    return optimizeTEB(cfg_->optim.no_inner_iterations, cfg_->optim.no_outer_iterations);
  }

  bool TebOptimalPlanner::buildGraph(double weight_multiplier)
  {
    if (!optimizer_->edges().empty() || !optimizer_->vertices().empty())
    {
      ROS_WARN("Cannot build graph, because it is not empty. Call graphClear()!");
      return false;
    }

    optimizer_->setComputeBatchStatistics(cfg_->recovery.divergence_detection_enable);

    // add TEB vertices
    AddTEBVertices();

    // add Edges (local cost functions)
    this->dynamic_obstacle_in_ = false;
    if (cfg_->obstacles.legacy_obstacle_association)
      AddEdgesObstaclesLegacy(weight_multiplier);
    else
      AddEdgesObstacles(weight_multiplier);

    if (cfg_->obstacles.include_dynamic_obstacles)
      AddEdgesDynamicObstacles();

    AddEdgesViaPoints();

    AddEdgesVelocity();

    AddEdgesAcceleration();

    AddEdgesTimeOptimal();

    AddEdgesShortestPath();

    if (cfg_->robot.min_turning_radius == 0 || cfg_->optim.weight_kinematics_turning_radius == 0)
      AddEdgesKinematicsDiffDrive(); // we have a differential drive robot
    else
      AddEdgesKinematicsCarlike(); // we have a carlike robot since the turning radius is bounded from below.

    AddEdgesPreferRotDir();

    if (cfg_->optim.weight_velocity_obstacle_ratio > 0)
      AddEdgesVelocityObstacleRatio();

    return true;
  }

  bool TebOptimalPlanner::optimizeGraph(int no_iterations, bool clear_after)
  {
    if (cfg_->robot.max_vel_x < 0.01)
    {
      ROS_WARN("optimizeGraph(): Robot Max Velocity is smaller than 0.01m/s. Optimizing aborted...");
      if (clear_after)
        clearGraph();
      return false;
    }

    if (!teb_.isInit() || teb_.sizePoses() < cfg_->trajectory.min_samples)
    {
      ROS_WARN("optimizeGraph(): TEB is empty or has too less elements. Skipping optimization.");
      if (clear_after)
        clearGraph();
      return false;
    }

    optimizer_->setVerbose(cfg_->optim.optimization_verbose);
    optimizer_->initializeOptimization();

    int iter = optimizer_->optimize(no_iterations);

    // Save Hessian for visualization
    //  g2o::OptimizationAlgorithmLevenberg* lm = dynamic_cast<g2o::OptimizationAlgorithmLevenberg*> (optimizer_->solver());
    //  lm->solver()->saveHessian("~/MasterThesis/Matlab/Hessian.txt");

    if (!iter)
    {
      ROS_ERROR("optimizeGraph(): Optimization failed! iter=%i", iter);
      return false;
    }

    if (clear_after)
      clearGraph();

    return true;
  }

  void TebOptimalPlanner::clearGraph()
  {
    // clear optimizer states
    if (optimizer_)
    {
      // we will delete all edges but keep the vertices.
      // before doing so, we will delete the link from the vertices to the edges.
      auto &vertices = optimizer_->vertices();
      for (auto &v : vertices)
        v.second->edges().clear();

      optimizer_->vertices().clear(); // necessary, because optimizer->clear deletes pointer-targets (therefore it deletes TEB states!)
      optimizer_->clear();
    }
  }

  void TebOptimalPlanner::AddTEBVertices()
  {

    // add vertices to graph
    ROS_DEBUG_COND(cfg_->optim.optimization_verbose, "Adding TEB vertices ...");
    // ROS_INFO("Adding TEB vertices .. ");
    unsigned int id_counter = 0; // used for vertices ids
    obstacles_per_vertex_.resize(teb_.sizePoses());
    auto iter_obstacle = obstacles_per_vertex_.begin();
    for (int i = 0; i < teb_.sizePoses(); ++i)
    {
      if (i < teb_.sizePoses() - 1)
      {
        // std::cout <<i<<"time_diff:"<<teb_.TimeDiff(i) << std::endl;
        this->time_diff.data.push_back(teb_.TimeDiff(i));
      }
      teb_.PoseVertex(i)->setId(id_counter++);
      optimizer_->addVertex(teb_.PoseVertex(i));
      if (teb_.sizeTimeDiffs() != 0 && i < teb_.sizeTimeDiffs())
      {
        teb_.TimeDiffVertex(i)->setId(id_counter++);
        optimizer_->addVertex(teb_.TimeDiffVertex(i));
      }
      iter_obstacle->clear();
      (iter_obstacle++)->reserve(obstacles_->size());
    }
    // time_diff.data.clear();
    timediff_pub.publish(this->time_diff);
    // std::cout << std::endl;
  }

  void TebOptimalPlanner::AddEdgesObstacles(double weight_multiplier)
  {
    if (cfg_->optim.weight_obstacle == 0 || weight_multiplier == 0 || obstacles_ == nullptr)
      return; // if weight equals zero skip adding edges!

    bool inflated = cfg_->obstacles.inflation_dist > cfg_->obstacles.min_obstacle_dist;

    Eigen::Matrix<double, 1, 1> information;
    information.fill(cfg_->optim.weight_obstacle * weight_multiplier);

    Eigen::Matrix<double, 2, 2> information_inflated;
    information_inflated(0, 0) = cfg_->optim.weight_obstacle * weight_multiplier;
    information_inflated(1, 1) = cfg_->optim.weight_inflation;
    information_inflated(0, 1) = information_inflated(1, 0) = 0;

    auto iter_obstacle = obstacles_per_vertex_.begin();

    auto create_edge = [inflated, &information, &information_inflated, this](int index, const Obstacle *obstacle)
    {
      if (inflated)
      {
        EdgeInflatedObstacle *dist_bandpt_obst = new EdgeInflatedObstacle;
        dist_bandpt_obst->setVertex(0, teb_.PoseVertex(index));
        dist_bandpt_obst->setInformation(information_inflated);
        dist_bandpt_obst->setParameters(*cfg_, robot_model_.get(), obstacle);
        optimizer_->addEdge(dist_bandpt_obst);
      }
      else
      {
        EdgeObstacle *dist_bandpt_obst = new EdgeObstacle;
        dist_bandpt_obst->setVertex(0, teb_.PoseVertex(index));
        dist_bandpt_obst->setInformation(information);
        dist_bandpt_obst->setParameters(*cfg_, robot_model_.get(), obstacle);
        optimizer_->addEdge(dist_bandpt_obst);
      };
    };
    // iterate all teb points, skipping the last and, if the EdgeVelocityObstacleRatio edges should not be created, the first one too
    const int first_vertex = cfg_->optim.weight_velocity_obstacle_ratio == 0 ? 1 : 0;
    marker_array.markers.clear();

    for (int i = 0; i < teb_.sizePoses() - 1; ++i)
    {
      visualization_msgs::Marker marker;
      const Eigen::Vector2d pose_orient = teb_.Pose(i).orientationUnitVec();

      for (const ObstaclePtr &obst : *obstacles_)
      {
        // calculate distance to robot model
        // double dist = robot_model_->calculateDistance(teb_.Pose(i), obst.get());
        double x = obst->getCentroid().coeffRef(0);
        double y = obst->getCentroid().coeffRef(1);
        bool dynamic_ = false;
        int mx, my;
        mx = (x - this->g_map.info.origin.position.x) / this->g_map.info.resolution;
        my = (y - this->g_map.info.origin.position.y) / this->g_map.info.resolution;
        int index = my * this->g_map.info.width + mx;
        if (!is_static(index))
        {
          dynamic_ = true;
        }
        if (!dynamic_)
        {
          Eigen::Vector2d obs(x, y);
          ObstaclePtr obptr = ObstaclePtr(new PointObstacle(obs));
          double teb_x = teb_.Pose(i).x();
          double teb_y = teb_.Pose(i).y();
          double dist = sqrt((teb_x - x) * (teb_x - x) + (teb_y - y) * (teb_y - y));
          if (dist < cfg_->obstacles.min_obstacle_dist * cfg_->obstacles.obstacle_association_force_inclusion_factor)
          {
            iter_obstacle->push_back(obptr);
          }
        }
      }

      if (this->obpos.data.size() > 0)
      {
        for (int k = 0; k < this->obpos.data.size(); k = k + 8)
        {
          double center_x;
          double center_y;

          double width = this->obpos.data[k + 6];
          double height = this->obpos.data[k + 7];

          KalmanFilter KF(6, 4, 0, CV_32F);
          setIdentity(KF.measurementMatrix);
          float sigmaP = 0.01;
          float sigmaQ = 0.1;
          setIdentity(KF.processNoiseCov, Scalar::all(sigmaP));
          setIdentity(KF.measurementNoiseCov, Scalar(sigmaQ));
          KF.statePost.at<float>(0) = this->obpos.data[k];
          KF.statePost.at<float>(1) = this->obpos.data[k + 1];
          KF.statePost.at<float>(2) = this->obpos.data[k + 2];
          KF.statePost.at<float>(3) = this->obpos.data[k + 3];
          KF.statePost.at<float>(4) = this->obpos.data[k + 4];
          KF.statePost.at<float>(5) = this->obpos.data[k + 5];
          // int flag_i = i;
          // if (i < teb_.sizePoses() / 3)
          // {
          //   flag_i = teb_.sizePoses() / 3;
          // }

          for (int j = 1; j < i; j++)
          {
            float dt = 0;
            dt = this->time_diff.data[j];
            KF.transitionMatrix = (Mat_<float>(6, 6) << 1, 0, dt, 0, 0.5 * pow(dt, 2), 0,
                                   0, 1, 0, dt, 0, 0.5 * pow(dt, 2),
                                   0, 0, 1, 0, dt, 0,
                                   0, 0, 0, 1, 0, dt,
                                   0, 0, 0, 0, 1, 0,
                                   0, 0, 0, 0, 0, 1);
            // KF.transitionMatrix = (Mat_<float>(4, 4) << 1, 0, dt, 0,
            //                        0, 1, 0, dt,
            //                        0, 0, 1, 0,
            //                        0, 0, 0, 1);
            Mat pred = KF.predict();
            center_x = pred.at<float>(0);
            center_y = pred.at<float>(1);
            // if (i < teb_.sizePoses() / 3)
            // {
            //   for (double x_i = center_x - 0.5 * width;;)
            //   {
            //     if (fabs(x_i - center_x) > 0.5 * width)
            //     {
            //       break;
            //     }
            //     else
            //     {
            //       for (double y_i = center_y - 0.5 * height;;)
            //       {
            //         if (fabs(y_i - center_y) > 0.5 * height)
            //         {
            //           break;
            //         }
            //         else
            //         {
            //           Eigen::Vector2d obs(x_i, y_i);
            //           ObstaclePtr obptr = ObstaclePtr(new PointObstacle(obs));
            //           double teb_x = teb_.Pose(i).x();
            //           double teb_y = teb_.Pose(i).y();
            //           double dist = sqrt((teb_x - x_i) * (teb_x - x_i) + (teb_y - y_i) * (teb_y - y_i));
            //           if (dist < cfg_->obstacles.min_obstacle_dist * cfg_->obstacles.obstacle_association_force_inclusion_factor)
            //           // if (dist < 1)
            //           {
            //             iter_obstacle->push_back(obptr);
            //             if (this->obpos.data[k + 2] > 0.2 || this->obpos.data[k + 3] > 0.2)
            //             {
            //               this->dynamic_obstacle_in_ = true;
            //             }
            //           }
            //           y_i += 0.08;
            //         }
            //       }
            //       x_i += 0.08;
            //     }
            //   }
            // }
          }

          for (double x_i = center_x - 0.5 * width;;)
          {
            if (fabs(x_i - center_x) > 0.5 * width)
            {
              break;
            }
            else
            {
              for (double y_i = center_y - 0.5 * height;;)
              {
                if (fabs(y_i - center_y) > 0.5 * height)
                {
                  break;
                }
                else
                {
                  Eigen::Vector2d obs(x_i, y_i);
                  ObstaclePtr obptr = ObstaclePtr(new PointObstacle(obs));
                  double teb_x = teb_.Pose(i).x();
                  double teb_y = teb_.Pose(i).y();
                  double dist = sqrt((teb_x - x_i) * (teb_x - x_i) + (teb_y - y_i) * (teb_y - y_i));
                  if (dist < cfg_->obstacles.min_obstacle_dist * cfg_->obstacles.obstacle_association_force_inclusion_factor)
                  // if (dist < 1)
                  {
                    iter_obstacle->push_back(obptr);
                    if (this->obpos.data[k + 2] > 0.1 || this->obpos.data[k + 3] > 0.1)
                    {
                      this->dynamic_obstacle_in_ = true;
                    }
                  }
                  y_i += 0.1;
                }
              }
              x_i += 0.1;
            }
          }

          // cout << i << " center_x:" << center_x << " center_y:" << center_y << " width:" << width << " height:" << height << endl;
        }
      }
      // create obstacle edges

      for (const ObstaclePtr obst : *iter_obstacle)
      {
        create_edge(i, obst.get());
        geometry_msgs::Point point;
        point.x = obst.get()->getCentroid().coeffRef(0);
        point.y = obst.get()->getCentroid().coeffRef(1);
        point.z = 0;
        marker.points.push_back(point);
      }
      // Eigen::Vector2d obs(i, 0);
      // ObstaclePtr obptr = ObstaclePtr(new PointObstacle(obs));

      // create_edge(i, obptr.get());
      ++iter_obstacle;

      marker.header.frame_id = "map";
      marker.header.stamp = ros::Time::now();
      marker.ns = "PointObstacles";
      marker.id = i;
      marker.type = visualization_msgs::Marker::POINTS;
      marker.action = visualization_msgs::Marker::ADD;
      marker.lifetime = ros::Duration(0.1);
      marker.scale.x = 0.05;
      marker.scale.y = 0.05;
      marker.color.a = 1.0;
      if (i % 3 == 1)
      {
        marker.color.r = 1;
        marker.color.g = 0;
        marker.color.b = 0;
      }
      else if (i % 3 == 2)
      {
        marker.color.r = 0;
        marker.color.g = 1;
        marker.color.b = 0;
      }
      else
      {
        marker.color.r = 0;
        marker.color.g = 0;
        marker.color.b = 1;
      }

      // std::cout << i << ": " << marker.color.r << " " << marker.color.g << " " << marker.color.b << std::endl;
      marker_array.markers.push_back(marker);
      marker.points.clear();
    }

    teb_markers_dynamic.publish(marker_array);
    this->time_diff.data.clear();
  }

  void TebOptimalPlanner::AddEdgesObstaclesLegacy(double weight_multiplier)
  {
    if (cfg_->optim.weight_obstacle == 0 || weight_multiplier == 0 || obstacles_ == nullptr)
      return; // if weight equals zero skip adding edges!

    Eigen::Matrix<double, 1, 1> information;
    information.fill(cfg_->optim.weight_obstacle * weight_multiplier);

    Eigen::Matrix<double, 2, 2> information_inflated;
    information_inflated(0, 0) = cfg_->optim.weight_obstacle * weight_multiplier;
    information_inflated(1, 1) = cfg_->optim.weight_inflation;
    information_inflated(0, 1) = information_inflated(1, 0) = 0;

    bool inflated = cfg_->obstacles.inflation_dist > cfg_->obstacles.min_obstacle_dist;

    for (ObstContainer::const_iterator obst = obstacles_->begin(); obst != obstacles_->end(); ++obst)
    {
      if (cfg_->obstacles.include_dynamic_obstacles && (*obst)->isDynamic()) // we handle dynamic obstacles differently below
        continue;

      int index;

      if (cfg_->obstacles.obstacle_poses_affected >= teb_.sizePoses())
        index = teb_.sizePoses() / 2;
      else
        index = teb_.findClosestTrajectoryPose(*(obst->get()));

      // check if obstacle is outside index-range between start and goal
      if ((index <= 1) || (index > teb_.sizePoses() - 2)) // start and goal are fixed and findNearestBandpoint finds first or last conf if intersection point is outside the range
        continue;

      if (inflated)
      {
        EdgeInflatedObstacle *dist_bandpt_obst = new EdgeInflatedObstacle;
        dist_bandpt_obst->setVertex(0, teb_.PoseVertex(index));
        dist_bandpt_obst->setInformation(information_inflated);
        dist_bandpt_obst->setParameters(*cfg_, robot_model_.get(), obst->get());
        optimizer_->addEdge(dist_bandpt_obst);
      }
      else
      {
        EdgeObstacle *dist_bandpt_obst = new EdgeObstacle;
        dist_bandpt_obst->setVertex(0, teb_.PoseVertex(index));
        dist_bandpt_obst->setInformation(information);
        dist_bandpt_obst->setParameters(*cfg_, robot_model_.get(), obst->get());
        optimizer_->addEdge(dist_bandpt_obst);
      }

      for (int neighbourIdx = 0; neighbourIdx < floor(cfg_->obstacles.obstacle_poses_affected / 2); neighbourIdx++)
      {
        if (index + neighbourIdx < teb_.sizePoses())
        {
          if (inflated)
          {
            EdgeInflatedObstacle *dist_bandpt_obst_n_r = new EdgeInflatedObstacle;
            dist_bandpt_obst_n_r->setVertex(0, teb_.PoseVertex(index + neighbourIdx));
            dist_bandpt_obst_n_r->setInformation(information_inflated);
            dist_bandpt_obst_n_r->setParameters(*cfg_, robot_model_.get(), obst->get());
            optimizer_->addEdge(dist_bandpt_obst_n_r);
          }
          else
          {
            EdgeObstacle *dist_bandpt_obst_n_r = new EdgeObstacle;
            dist_bandpt_obst_n_r->setVertex(0, teb_.PoseVertex(index + neighbourIdx));
            dist_bandpt_obst_n_r->setInformation(information);
            dist_bandpt_obst_n_r->setParameters(*cfg_, robot_model_.get(), obst->get());
            optimizer_->addEdge(dist_bandpt_obst_n_r);
          }
        }
        if (index - neighbourIdx >= 0) // needs to be casted to int to allow negative values
        {
          if (inflated)
          {
            EdgeInflatedObstacle *dist_bandpt_obst_n_l = new EdgeInflatedObstacle;
            dist_bandpt_obst_n_l->setVertex(0, teb_.PoseVertex(index - neighbourIdx));
            dist_bandpt_obst_n_l->setInformation(information_inflated);
            dist_bandpt_obst_n_l->setParameters(*cfg_, robot_model_.get(), obst->get());
            optimizer_->addEdge(dist_bandpt_obst_n_l);
          }
          else
          {
            EdgeObstacle *dist_bandpt_obst_n_l = new EdgeObstacle;
            dist_bandpt_obst_n_l->setVertex(0, teb_.PoseVertex(index - neighbourIdx));
            dist_bandpt_obst_n_l->setInformation(information);
            dist_bandpt_obst_n_l->setParameters(*cfg_, robot_model_.get(), obst->get());
            optimizer_->addEdge(dist_bandpt_obst_n_l);
          }
        }
      }
    }
  }

  void TebOptimalPlanner::AddEdgesDynamicObstacles(double weight_multiplier)
  {
    if (cfg_->optim.weight_obstacle == 0 || weight_multiplier == 0 || obstacles_ == NULL)
      return; // if weight equals zero skip adding edges!

    Eigen::Matrix<double, 2, 2> information;
    information(0, 0) = cfg_->optim.weight_dynamic_obstacle * weight_multiplier;
    information(1, 1) = cfg_->optim.weight_dynamic_obstacle_inflation;
    information(0, 1) = information(1, 0) = 0;

    for (ObstContainer::const_iterator obst = obstacles_->begin(); obst != obstacles_->end(); ++obst)
    {
      if (!(*obst)->isDynamic())
        continue;

      // Skip first and last pose, as they are fixed
      double time = teb_.TimeDiff(0);
      for (int i = 1; i < teb_.sizePoses() - 1; ++i)
      {
        // std::cout << teb_.TimeDiff(i) << std::endl;
        EdgeDynamicObstacle *dynobst_edge = new EdgeDynamicObstacle(time);
        dynobst_edge->setVertex(0, teb_.PoseVertex(i));
        dynobst_edge->setInformation(information);
        dynobst_edge->setParameters(*cfg_, robot_model_.get(), obst->get());
        optimizer_->addEdge(dynobst_edge);
        time += teb_.TimeDiff(i); // we do not need to check the time diff bounds, since we iterate to "< sizePoses()-1".
      }
    }
  }

  void TebOptimalPlanner::AddEdgesViaPoints()
  {
    if (cfg_->optim.weight_viapoint == 0 || via_points_ == NULL || via_points_->empty())
      return; // if weight equals zero skip adding edges!

    int start_pose_idx = 0;

    int n = teb_.sizePoses();
    if (n < 3) // we do not have any degrees of freedom for reaching via-points
      return;

    for (ViaPointContainer::const_iterator vp_it = via_points_->begin(); vp_it != via_points_->end(); ++vp_it)
    {

      int index = teb_.findClosestTrajectoryPose(*vp_it, NULL, start_pose_idx);
      if (cfg_->trajectory.via_points_ordered)
        start_pose_idx = index + 2; // skip a point to have a DOF inbetween for further via-points

      // check if point conicides with goal or is located behind it
      if (index > n - 2)
        index = n - 2; // set to a pose before the goal, since we can move it away!
      // check if point coincides with start or is located before it
      if (index < 1)
      {
        if (cfg_->trajectory.via_points_ordered)
        {
          index = 1; // try to connect the via point with the second (and non-fixed) pose. It is likely that autoresize adds new poses inbetween later.
        }
        else
        {
          ROS_DEBUG("TebOptimalPlanner::AddEdgesViaPoints(): skipping a via-point that is close or behind the current robot pose.");
          continue; // skip via points really close or behind the current robot pose
        }
      }
      Eigen::Matrix<double, 1, 1> information;
      information.fill(cfg_->optim.weight_viapoint);

      EdgeViaPoint *edge_viapoint = new EdgeViaPoint;
      edge_viapoint->setVertex(0, teb_.PoseVertex(index));
      edge_viapoint->setInformation(information);
      edge_viapoint->setParameters(*cfg_, &(*vp_it));
      optimizer_->addEdge(edge_viapoint);
    }
  }

  void TebOptimalPlanner::AddEdgesVelocity()
  {
    if (cfg_->robot.max_vel_y == 0) // non-holonomic robot
    {
      if (cfg_->optim.weight_max_vel_x == 0 && cfg_->optim.weight_max_vel_theta == 0)
        return; // if weight equals zero skip adding edges!

      int n = teb_.sizePoses();
      Eigen::Matrix<double, 2, 2> information;
      information(0, 0) = cfg_->optim.weight_max_vel_x;
      information(1, 1) = cfg_->optim.weight_max_vel_theta;
      information(0, 1) = 0.0;
      information(1, 0) = 0.0;

      for (int i = 0; i < n - 1; ++i)
      {
        EdgeVelocity *velocity_edge = new EdgeVelocity;
        velocity_edge->setVertex(0, teb_.PoseVertex(i));
        velocity_edge->setVertex(1, teb_.PoseVertex(i + 1));
        velocity_edge->setVertex(2, teb_.TimeDiffVertex(i));
        velocity_edge->setInformation(information);
        velocity_edge->setTebConfig(*cfg_);
        optimizer_->addEdge(velocity_edge);
      }
    }
    else // holonomic-robot
    {
      if (cfg_->optim.weight_max_vel_x == 0 && cfg_->optim.weight_max_vel_y == 0 && cfg_->optim.weight_max_vel_theta == 0)
        return; // if weight equals zero skip adding edges!

      int n = teb_.sizePoses();
      Eigen::Matrix<double, 3, 3> information;
      information.fill(0);
      information(0, 0) = cfg_->optim.weight_max_vel_x;
      information(1, 1) = cfg_->optim.weight_max_vel_y;
      information(2, 2) = cfg_->optim.weight_max_vel_theta;

      for (int i = 0; i < n - 1; ++i)
      {
        EdgeVelocityHolonomic *velocity_edge = new EdgeVelocityHolonomic;
        velocity_edge->setVertex(0, teb_.PoseVertex(i));
        velocity_edge->setVertex(1, teb_.PoseVertex(i + 1));
        velocity_edge->setVertex(2, teb_.TimeDiffVertex(i));
        velocity_edge->setInformation(information);
        velocity_edge->setTebConfig(*cfg_);
        optimizer_->addEdge(velocity_edge);
      }
    }
  }

  void TebOptimalPlanner::AddEdgesAcceleration()
  {
    if (cfg_->optim.weight_acc_lim_x == 0 && cfg_->optim.weight_acc_lim_theta == 0)
      return; // if weight equals zero skip adding edges!

    int n = teb_.sizePoses();

    if (cfg_->robot.max_vel_y == 0 || cfg_->robot.acc_lim_y == 0) // non-holonomic robot
    {
      Eigen::Matrix<double, 2, 2> information;
      information.fill(0);
      information(0, 0) = cfg_->optim.weight_acc_lim_x;
      information(1, 1) = cfg_->optim.weight_acc_lim_theta;

      // check if an initial velocity should be taken into accound
      if (vel_start_.first)
      {
        EdgeAccelerationStart *acceleration_edge = new EdgeAccelerationStart;
        acceleration_edge->setVertex(0, teb_.PoseVertex(0));
        acceleration_edge->setVertex(1, teb_.PoseVertex(1));
        acceleration_edge->setVertex(2, teb_.TimeDiffVertex(0));
        acceleration_edge->setInitialVelocity(vel_start_.second);
        acceleration_edge->setInformation(information);
        acceleration_edge->setTebConfig(*cfg_);
        optimizer_->addEdge(acceleration_edge);
      }

      // now add the usual acceleration edge for each tuple of three teb poses
      for (int i = 0; i < n - 2; ++i)
      {
        EdgeAcceleration *acceleration_edge = new EdgeAcceleration;
        acceleration_edge->setVertex(0, teb_.PoseVertex(i));
        acceleration_edge->setVertex(1, teb_.PoseVertex(i + 1));
        acceleration_edge->setVertex(2, teb_.PoseVertex(i + 2));
        acceleration_edge->setVertex(3, teb_.TimeDiffVertex(i));
        acceleration_edge->setVertex(4, teb_.TimeDiffVertex(i + 1));
        acceleration_edge->setInformation(information);
        acceleration_edge->setTebConfig(*cfg_);
        optimizer_->addEdge(acceleration_edge);
      }

      // check if a goal velocity should be taken into accound
      if (vel_goal_.first)
      {
        EdgeAccelerationGoal *acceleration_edge = new EdgeAccelerationGoal;
        acceleration_edge->setVertex(0, teb_.PoseVertex(n - 2));
        acceleration_edge->setVertex(1, teb_.PoseVertex(n - 1));
        acceleration_edge->setVertex(2, teb_.TimeDiffVertex(teb_.sizeTimeDiffs() - 1));
        acceleration_edge->setGoalVelocity(vel_goal_.second);
        acceleration_edge->setInformation(information);
        acceleration_edge->setTebConfig(*cfg_);
        optimizer_->addEdge(acceleration_edge);
      }
    }
    else // holonomic robot
    {
      Eigen::Matrix<double, 3, 3> information;
      information.fill(0);
      information(0, 0) = cfg_->optim.weight_acc_lim_x;
      information(1, 1) = cfg_->optim.weight_acc_lim_y;
      information(2, 2) = cfg_->optim.weight_acc_lim_theta;

      // check if an initial velocity should be taken into accound
      if (vel_start_.first)
      {
        EdgeAccelerationHolonomicStart *acceleration_edge = new EdgeAccelerationHolonomicStart;
        acceleration_edge->setVertex(0, teb_.PoseVertex(0));
        acceleration_edge->setVertex(1, teb_.PoseVertex(1));
        acceleration_edge->setVertex(2, teb_.TimeDiffVertex(0));
        acceleration_edge->setInitialVelocity(vel_start_.second);
        acceleration_edge->setInformation(information);
        acceleration_edge->setTebConfig(*cfg_);
        optimizer_->addEdge(acceleration_edge);
      }

      // now add the usual acceleration edge for each tuple of three teb poses
      for (int i = 0; i < n - 2; ++i)
      {
        EdgeAccelerationHolonomic *acceleration_edge = new EdgeAccelerationHolonomic;
        acceleration_edge->setVertex(0, teb_.PoseVertex(i));
        acceleration_edge->setVertex(1, teb_.PoseVertex(i + 1));
        acceleration_edge->setVertex(2, teb_.PoseVertex(i + 2));
        acceleration_edge->setVertex(3, teb_.TimeDiffVertex(i));
        acceleration_edge->setVertex(4, teb_.TimeDiffVertex(i + 1));
        acceleration_edge->setInformation(information);
        acceleration_edge->setTebConfig(*cfg_);
        optimizer_->addEdge(acceleration_edge);
      }

      // check if a goal velocity should be taken into accound
      if (vel_goal_.first)
      {
        EdgeAccelerationHolonomicGoal *acceleration_edge = new EdgeAccelerationHolonomicGoal;
        acceleration_edge->setVertex(0, teb_.PoseVertex(n - 2));
        acceleration_edge->setVertex(1, teb_.PoseVertex(n - 1));
        acceleration_edge->setVertex(2, teb_.TimeDiffVertex(teb_.sizeTimeDiffs() - 1));
        acceleration_edge->setGoalVelocity(vel_goal_.second);
        acceleration_edge->setInformation(information);
        acceleration_edge->setTebConfig(*cfg_);
        optimizer_->addEdge(acceleration_edge);
      }
    }
  }

  void TebOptimalPlanner::AddEdgesTimeOptimal()
  {
    if (cfg_->optim.weight_optimaltime == 0)
      return; // if weight equals zero skip adding edges!

    Eigen::Matrix<double, 1, 1> information;
    information.fill(cfg_->optim.weight_optimaltime);

    for (int i = 0; i < teb_.sizeTimeDiffs(); ++i)
    {
      EdgeTimeOptimal *timeoptimal_edge = new EdgeTimeOptimal;
      timeoptimal_edge->setVertex(0, teb_.TimeDiffVertex(i));
      timeoptimal_edge->setInformation(information);
      timeoptimal_edge->setTebConfig(*cfg_);
      optimizer_->addEdge(timeoptimal_edge);
    }
  }

  void TebOptimalPlanner::AddEdgesShortestPath()
  {
    if (cfg_->optim.weight_shortest_path == 0)
      return; // if weight equals zero skip adding edges!

    Eigen::Matrix<double, 1, 1> information;
    information.fill(cfg_->optim.weight_shortest_path);

    for (int i = 0; i < teb_.sizePoses() - 1; ++i)
    {
      EdgeShortestPath *shortest_path_edge = new EdgeShortestPath;
      shortest_path_edge->setVertex(0, teb_.PoseVertex(i));
      shortest_path_edge->setVertex(1, teb_.PoseVertex(i + 1));
      shortest_path_edge->setInformation(information);
      shortest_path_edge->setTebConfig(*cfg_);
      optimizer_->addEdge(shortest_path_edge);
    }
  }

  void TebOptimalPlanner::AddEdgesKinematicsDiffDrive()
  {
    if (cfg_->optim.weight_kinematics_nh == 0 && cfg_->optim.weight_kinematics_forward_drive == 0)
      return; // if weight equals zero skip adding edges!

    // create edge for satisfiying kinematic constraints
    Eigen::Matrix<double, 2, 2> information_kinematics;
    information_kinematics.fill(0.0);
    information_kinematics(0, 0) = cfg_->optim.weight_kinematics_nh;
    information_kinematics(1, 1) = cfg_->optim.weight_kinematics_forward_drive;

    for (int i = 0; i < teb_.sizePoses() - 1; i++) // ignore twiced start only
    {
      EdgeKinematicsDiffDrive *kinematics_edge = new EdgeKinematicsDiffDrive;
      kinematics_edge->setVertex(0, teb_.PoseVertex(i));
      kinematics_edge->setVertex(1, teb_.PoseVertex(i + 1));
      kinematics_edge->setInformation(information_kinematics);
      kinematics_edge->setTebConfig(*cfg_);
      optimizer_->addEdge(kinematics_edge);
    }
  }

  void TebOptimalPlanner::AddEdgesKinematicsCarlike()
  {
    if (cfg_->optim.weight_kinematics_nh == 0 && cfg_->optim.weight_kinematics_turning_radius == 0)
      return; // if weight equals zero skip adding edges!

    // create edge for satisfiying kinematic constraints
    Eigen::Matrix<double, 2, 2> information_kinematics;
    information_kinematics.fill(0.0);
    information_kinematics(0, 0) = cfg_->optim.weight_kinematics_nh;
    information_kinematics(1, 1) = cfg_->optim.weight_kinematics_turning_radius;

    for (int i = 0; i < teb_.sizePoses() - 1; i++) // ignore twiced start only
    {
      EdgeKinematicsCarlike *kinematics_edge = new EdgeKinematicsCarlike;
      kinematics_edge->setVertex(0, teb_.PoseVertex(i));
      kinematics_edge->setVertex(1, teb_.PoseVertex(i + 1));
      kinematics_edge->setInformation(information_kinematics);
      kinematics_edge->setTebConfig(*cfg_);
      optimizer_->addEdge(kinematics_edge);
    }
  }

  void TebOptimalPlanner::AddEdgesPreferRotDir()
  {
    // TODO(roesmann): Note, these edges can result in odd predictions, in particular
    //                 we can observe a substantional mismatch between open- and closed-loop planning
    //                 leading to a poor control performance.
    //                 At the moment, we keep these functionality for oscillation recovery:
    //                 Activating the edge for a short time period might not be crucial and
    //                 could move the robot to a new oscillation-free state.
    //                 This needs to be analyzed in more detail!
    if (prefer_rotdir_ == RotType::none || cfg_->optim.weight_prefer_rotdir == 0)
      return; // if weight equals zero skip adding edges!

    if (prefer_rotdir_ != RotType::right && prefer_rotdir_ != RotType::left)
    {
      ROS_WARN("TebOptimalPlanner::AddEdgesPreferRotDir(): unsupported RotType selected. Skipping edge creation.");
      return;
    }

    // create edge for satisfiying kinematic constraints
    Eigen::Matrix<double, 1, 1> information_rotdir;
    information_rotdir.fill(cfg_->optim.weight_prefer_rotdir);

    for (int i = 0; i < teb_.sizePoses() - 1 && i < 3; ++i) // currently: apply to first 3 rotations
    {
      EdgePreferRotDir *rotdir_edge = new EdgePreferRotDir;
      rotdir_edge->setVertex(0, teb_.PoseVertex(i));
      rotdir_edge->setVertex(1, teb_.PoseVertex(i + 1));
      rotdir_edge->setInformation(information_rotdir);

      if (prefer_rotdir_ == RotType::left)
        rotdir_edge->preferLeft();
      else if (prefer_rotdir_ == RotType::right)
        rotdir_edge->preferRight();

      optimizer_->addEdge(rotdir_edge);
    }
  }

  void TebOptimalPlanner::AddEdgesVelocityObstacleRatio()
  {
    Eigen::Matrix<double, 2, 2> information;
    information(0, 0) = cfg_->optim.weight_velocity_obstacle_ratio;
    information(1, 1) = cfg_->optim.weight_velocity_obstacle_ratio;
    information(0, 1) = information(1, 0) = 0;

    auto iter_obstacle = obstacles_per_vertex_.begin();

    for (int index = 0; index < teb_.sizePoses() - 1; ++index)
    {
      for (const ObstaclePtr obstacle : (*iter_obstacle++))
      {
        EdgeVelocityObstacleRatio *edge = new EdgeVelocityObstacleRatio;
        edge->setVertex(0, teb_.PoseVertex(index));
        edge->setVertex(1, teb_.PoseVertex(index + 1));
        edge->setVertex(2, teb_.TimeDiffVertex(index));
        edge->setInformation(information);
        edge->setParameters(*cfg_, robot_model_.get(), obstacle.get());
        optimizer_->addEdge(edge);
      }
    }
  }

  bool TebOptimalPlanner::hasDiverged() const
  {
    // Early returns if divergence detection is not active
    if (!cfg_->recovery.divergence_detection_enable)
      return false;

    auto stats_vector = optimizer_->batchStatistics();

    // No statistics yet
    if (stats_vector.empty())
      return false;

    // Grab the statistics of the final iteration
    const auto last_iter_stats = stats_vector.back();

    return last_iter_stats.chi2 > cfg_->recovery.divergence_detection_max_chi_squared;
  }

  void TebOptimalPlanner::computeCurrentCost(double obst_cost_scale, double viapoint_cost_scale, bool alternative_time_cost)
  {
    // check if graph is empty/exist  -> important if function is called between buildGraph and optimizeGraph/clearGraph
    bool graph_exist_flag(false);
    if (optimizer_->edges().empty() && optimizer_->vertices().empty())
    {
      // here the graph is build again, for time efficiency make sure to call this function
      // between buildGraph and Optimize (deleted), but it depends on the application
      buildGraph();
      optimizer_->initializeOptimization();
    }
    else
    {
      graph_exist_flag = true;
    }

    optimizer_->computeInitialGuess();

    cost_ = 0;

    if (alternative_time_cost)
    {
      cost_ += teb_.getSumOfAllTimeDiffs();
      // TEST we use SumOfAllTimeDiffs() here, because edge cost depends on number of samples, which is not always the same for similar TEBs,
      // since we are using an AutoResize Function with hysteresis.
    }

    // now we need pointers to all edges -> calculate error for each edge-type
    // since we aren't storing edge pointers, we need to check every edge
    for (std::vector<g2o::OptimizableGraph::Edge *>::const_iterator it = optimizer_->activeEdges().begin(); it != optimizer_->activeEdges().end(); it++)
    {
      double cur_cost = (*it)->chi2();

      if (dynamic_cast<EdgeObstacle *>(*it) != nullptr || dynamic_cast<EdgeInflatedObstacle *>(*it) != nullptr || dynamic_cast<EdgeDynamicObstacle *>(*it) != nullptr)
      {
        cur_cost *= obst_cost_scale;
      }
      else if (dynamic_cast<EdgeViaPoint *>(*it) != nullptr)
      {
        cur_cost *= viapoint_cost_scale;
      }
      else if (dynamic_cast<EdgeTimeOptimal *>(*it) != nullptr && alternative_time_cost)
      {
        continue; // skip these edges if alternative_time_cost is active
      }
      cost_ += cur_cost;
    }

    // delete temporary created graph
    if (!graph_exist_flag)
      clearGraph();
  }

  void TebOptimalPlanner::extractVelocity(const PoseSE2 &pose1, const PoseSE2 &pose2, double dt, double &vx, double &vy, double &omega) const
  {
    if (dt == 0)
    {
      vx = 0;
      vy = 0;
      omega = 0;
      return;
    }

    Eigen::Vector2d deltaS = pose2.position() - pose1.position();

    if (cfg_->robot.max_vel_y == 0) // nonholonomic robot
    {
      Eigen::Vector2d conf1dir(cos(pose1.theta()), sin(pose1.theta()));
      // translational velocity
      double dir = deltaS.dot(conf1dir);
      vx = (double)g2o::sign(dir) * deltaS.norm() / dt;
      vy = 0;
    }
    else // holonomic robot
    {
      // transform pose 2 into the current robot frame (pose1)
      // for velocities only the rotation of the direction vector is necessary.
      // (map->pose1-frame: inverse 2d rotation matrix)
      double cos_theta1 = std::cos(pose1.theta());
      double sin_theta1 = std::sin(pose1.theta());
      double p1_dx = cos_theta1 * deltaS.x() + sin_theta1 * deltaS.y();
      double p1_dy = -sin_theta1 * deltaS.x() + cos_theta1 * deltaS.y();
      vx = p1_dx / dt;
      vy = p1_dy / dt;
    }

    // rotational velocity
    double orientdiff = g2o::normalize_theta(pose2.theta() - pose1.theta());
    omega = orientdiff / dt;
  }

  bool TebOptimalPlanner::getVelocityCommand(double &vx, double &vy, double &omega, int look_ahead_poses) const
  {
    if (teb_.sizePoses() < 2)
    {
      ROS_ERROR("TebOptimalPlanner::getVelocityCommand(): The trajectory contains less than 2 poses. Make sure to init and optimize/plan the trajectory fist.");
      vx = 0;
      vy = 0;
      omega = 0;
      return false;
    }
    look_ahead_poses = std::max(1, std::min(look_ahead_poses, teb_.sizePoses() - 1 - cfg_->trajectory.prevent_look_ahead_poses_near_goal));
    double dt = 0.0;
    for (int counter = 0; counter < look_ahead_poses; ++counter)
    {
      dt += teb_.TimeDiff(counter);
      if (dt >= cfg_->trajectory.dt_ref * look_ahead_poses) // TODO: change to look-ahead time? Refine trajectory?
      {
        look_ahead_poses = counter + 1;
        break;
      }
    }
    if (dt <= 0)
    {
      ROS_ERROR("TebOptimalPlanner::getVelocityCommand() - timediff<=0 is invalid!");
      vx = 0;
      vy = 0;
      omega = 0;
      return false;
    }

    // Get velocity from the first two configurations
    extractVelocity(teb_.Pose(0), teb_.Pose(look_ahead_poses), dt, vx, vy, omega);
    return true;
  }

  void TebOptimalPlanner::getVelocityProfile(std::vector<geometry_msgs::Twist> &velocity_profile) const
  {
    int n = teb_.sizePoses();
    velocity_profile.resize(n + 1);

    // start velocity
    velocity_profile.front().linear.z = 0;
    velocity_profile.front().angular.x = velocity_profile.front().angular.y = 0;
    velocity_profile.front().linear.x = vel_start_.second.linear.x;
    velocity_profile.front().linear.y = vel_start_.second.linear.y;
    velocity_profile.front().angular.z = vel_start_.second.angular.z;

    for (int i = 1; i < n; ++i)
    {
      velocity_profile[i].linear.z = 0;
      velocity_profile[i].angular.x = velocity_profile[i].angular.y = 0;
      extractVelocity(teb_.Pose(i - 1), teb_.Pose(i), teb_.TimeDiff(i - 1), velocity_profile[i].linear.x, velocity_profile[i].linear.y, velocity_profile[i].angular.z);
    }

    // goal velocity
    velocity_profile.back().linear.z = 0;
    velocity_profile.back().angular.x = velocity_profile.back().angular.y = 0;
    velocity_profile.back().linear.x = vel_goal_.second.linear.x;
    velocity_profile.back().linear.y = vel_goal_.second.linear.y;
    velocity_profile.back().angular.z = vel_goal_.second.angular.z;
  }

  void TebOptimalPlanner::getFullTrajectory(std::vector<TrajectoryPointMsg> &trajectory) const
  {
    int n = teb_.sizePoses();

    trajectory.resize(n);

    if (n == 0)
      return;

    double curr_time = 0;

    // start
    TrajectoryPointMsg &start = trajectory.front();
    teb_.Pose(0).toPoseMsg(start.pose);
    start.velocity.linear.z = 0;
    start.velocity.angular.x = start.velocity.angular.y = 0;
    start.velocity.linear.x = vel_start_.second.linear.x;
    start.velocity.linear.y = vel_start_.second.linear.y;
    start.velocity.angular.z = vel_start_.second.angular.z;
    start.time_from_start.fromSec(curr_time);

    curr_time += teb_.TimeDiff(0);

    // intermediate points
    for (int i = 1; i < n - 1; ++i)
    {
      TrajectoryPointMsg &point = trajectory[i];
      teb_.Pose(i).toPoseMsg(point.pose);
      point.velocity.linear.z = 0;
      point.velocity.angular.x = point.velocity.angular.y = 0;
      double vel1_x, vel1_y, vel2_x, vel2_y, omega1, omega2;
      extractVelocity(teb_.Pose(i - 1), teb_.Pose(i), teb_.TimeDiff(i - 1), vel1_x, vel1_y, omega1);
      extractVelocity(teb_.Pose(i), teb_.Pose(i + 1), teb_.TimeDiff(i), vel2_x, vel2_y, omega2);
      point.velocity.linear.x = 0.5 * (vel1_x + vel2_x);
      point.velocity.linear.y = 0.5 * (vel1_y + vel2_y);
      point.velocity.angular.z = 0.5 * (omega1 + omega2);
      point.time_from_start.fromSec(curr_time);

      curr_time += teb_.TimeDiff(i);
    }

    // goal
    TrajectoryPointMsg &goal = trajectory.back();
    teb_.BackPose().toPoseMsg(goal.pose);
    goal.velocity.linear.z = 0;
    goal.velocity.angular.x = goal.velocity.angular.y = 0;
    goal.velocity.linear.x = vel_goal_.second.linear.x;
    goal.velocity.linear.y = vel_goal_.second.linear.y;
    goal.velocity.angular.z = vel_goal_.second.angular.z;
    goal.time_from_start.fromSec(curr_time);
  }

  bool TebOptimalPlanner::isTrajectoryFeasible(base_local_planner::CostmapModel *costmap_model, const std::vector<geometry_msgs::Point> &footprint_spec,
                                               double inscribed_radius, double circumscribed_radius, int look_ahead_idx)
  {
    if (look_ahead_idx < 0 || look_ahead_idx >= teb().sizePoses())
      look_ahead_idx = teb().sizePoses() - 1;

    for (int i = 0; i <= look_ahead_idx; ++i)
    {
      if (costmap_model->footprintCost(teb().Pose(i).x(), teb().Pose(i).y(), teb().Pose(i).theta(), footprint_spec, inscribed_radius, circumscribed_radius) == -1)
      {
        if (visualization_)
        {
          visualization_->publishInfeasibleRobotPose(teb().Pose(i), *robot_model_);
        }
        return false;
      }
      // Checks if the distance between two poses is higher than the robot radius or the orientation diff is bigger than the specified threshold
      // and interpolates in that case.
      // (if obstacles are pushing two consecutive poses away, the center between two consecutive poses might coincide with the obstacle ;-)!
      if (i < look_ahead_idx)
      {
        double delta_rot = g2o::normalize_theta(g2o::normalize_theta(teb().Pose(i + 1).theta()) -
                                                g2o::normalize_theta(teb().Pose(i).theta()));
        Eigen::Vector2d delta_dist = teb().Pose(i + 1).position() - teb().Pose(i).position();
        if (fabs(delta_rot) > cfg_->trajectory.min_resolution_collision_check_angular || delta_dist.norm() > inscribed_radius)
        {
          int n_additional_samples = std::max(std::ceil(fabs(delta_rot) / cfg_->trajectory.min_resolution_collision_check_angular),
                                              std::ceil(delta_dist.norm() / inscribed_radius)) -
                                     1;
          PoseSE2 intermediate_pose = teb().Pose(i);
          for (int step = 0; step < n_additional_samples; ++step)
          {
            intermediate_pose.position() = intermediate_pose.position() + delta_dist / (n_additional_samples + 1.0);
            intermediate_pose.theta() = g2o::normalize_theta(intermediate_pose.theta() +
                                                             delta_rot / (n_additional_samples + 1.0));
            if (costmap_model->footprintCost(intermediate_pose.x(), intermediate_pose.y(), intermediate_pose.theta(),
                                             footprint_spec, inscribed_radius, circumscribed_radius) == -1)
            {
              if (visualization_)
              {
                visualization_->publishInfeasibleRobotPose(intermediate_pose, *robot_model_);
              }
              return false;
            }
          }
        }
      }
    }
    return true;
  }

} // namespace teb_local_planner