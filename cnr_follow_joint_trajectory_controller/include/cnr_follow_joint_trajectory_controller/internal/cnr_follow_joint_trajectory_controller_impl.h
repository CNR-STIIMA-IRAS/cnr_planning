#ifndef CNR_FOLLOW_JOINT_TRAJECTORY_CONTROLLER_CNR_FOLLOW_JOINT_TRAJECTORY_CONTROLLER_IMPL_H
#define CNR_FOLLOW_JOINT_TRAJECTORY_CONTROLLER_CNR_FOLLOW_JOINT_TRAJECTORY_CONTROLLER_IMPL_H

#include <controller_interface/controller_base.h>
#include <name_sorting/sort_trajectories.h>
#include <std_msgs/Int64.h>
#include <std_msgs/Float64.h>
#include <sensor_msgs/JointState.h>
#include <cnr_follow_joint_trajectory_controller/cnr_follow_joint_trajectory_controller.h>
#include <cnr_interpolator_interface/cnr_interpolator_inputs.h>
#include <cnr_interpolator_interface/cnr_interpolator_outputs.h>
#include <cnr_interpolator_interface/cnr_interpolator_point.h>
#include <cnr_interpolator_interface/cnr_interpolator_trajectory.h>

#include <cnr_regulator_interface/cnr_regulator_inputs.h>
#include <cnr_regulator_interface/cnr_regulator_outputs.h>

namespace cnr
{
namespace control
{

template< class T>
FollowJointTrajectoryController<T>::~FollowJointTrajectoryController()
{
  CNR_DEBUG(*this->logger(), "Destroying Thor Prefilter Controller");
  joinActionServerThread();
  m_as.reset();
  CNR_DEBUG(*this->logger(), "Destroyed Thor Prefilter Controller");
}

template< class T>
FollowJointTrajectoryController<T>::FollowJointTrajectoryController()
  : m_interpolator_loader("cnr_interpolator_interface", "cnr_interpolator_interface::InterpolatorInterface")
  , m_regulator_loader("cnr_regulator_interface", "cnr_regulator_interface::RegulatorBase")
{
  m_is_in_tolerance = false;
  m_preempted = false;
  m_interpolator.reset();

  //----------- NP -----------
  m_scaled_time_pub_idx = this->template add_publisher<std_msgs::Float64>("scaled_time_pub", 1);
  m_target_pub_idx      = this->template add_publisher<sensor_msgs::JointState>("/target/joint_states", 1);
  //--------------------------

}

template< class T>
bool FollowJointTrajectoryController<T>::doInit()
{
  CNR_TRACE_START(*this->logger());

  std::string interpolator;
  if(!this->getControllerNh().getParam("interpolator", interpolator))
  {
    CNR_RETURN_FALSE(*this->logger(),"The param '"+this->getControllerNamespace()+"/interpolator' is missing. Abort.")
  }
  try
  {
    auto interp = m_interpolator_loader.createInstance(interpolator);
    m_interpolator = cnr_controller_interface::to_std_shared_ptr( interp );
    if(!m_interpolator->initialize(this->logger(), this->getControllerNh()) )
    {
      CNR_RETURN_FALSE(*this->logger(), "The interpolator init failed. Abort.")
    }
  }
  catch(pluginlib::PluginlibException& ex)
  {
    CNR_RETURN_FALSE(*this->logger(), "The plugin failed to load for some reason. Error: " + std::string(ex.what()));
  }


  std::string regulator;
  if(!this->getControllerNh().getParam("regulator", regulator))
  {
    CNR_RETURN_FALSE(*this->logger(), "The param '"+this->getControllerNamespace()+"/regulator' is missing. Abort.")
  }
  try
  {
    auto reg = m_regulator_loader.createInstance(regulator);
    m_regulator = cnr_controller_interface::to_std_shared_ptr( reg );
    if(!m_regulator->initialize(this->getRootNh(), this->getControllerNh(),
                                this->logger(), this->m_kin, this->m_state, ros::Duration(this->m_sampling_period)))
    {
      CNR_RETURN_FALSE(*this->logger(), "The regulator init failed. Abort.")
    }
    m_regulator_input .reset(new cnr_regulator_interface::JointRegulatorInput(this->m_kin->nAx()));
    m_regulator_output.reset(new cnr_regulator_interface::JointRegulatorOutput(this->m_kin->nAx()));

  }
  catch(pluginlib::PluginlibException& ex)
  {
    CNR_RETURN_FALSE(*this->logger(), "The plugin failed to load for some reason. Error: " + std::string(ex.what()));
  }

  double goal_tolerance;
  if(!this->getControllerNh().getParam("goal_tolerance",  goal_tolerance))
  {
    goal_tolerance = 0.001;
  }
  m_regulator_input->set_goal_tolerance(goal_tolerance);

  double path_tolerance;
  if(!this->getControllerNh().getParam("path_tolerance", path_tolerance))
  {
     path_tolerance = 0.001;
  }
  m_regulator_input->set_path_tolerance(path_tolerance);

  m_as.reset(new actionlib::ActionServer<control_msgs::FollowJointTrajectoryAction>(
                    this->getControllerNh(),
                    "follow_joint_trajectory",
                    boost::bind(&FollowJointTrajectoryController<T>::actionGoalCallback, this, _1),
                    boost::bind(&FollowJointTrajectoryController<T>::actionCancelCallback, this, _1),
                    false));

  CNR_RETURN_TRUE(*this->logger());
}

template< class T>
bool FollowJointTrajectoryController<T>::doStarting(const ros::Time& time)
{
  CNR_TRACE_START(*this->logger());

  trajectory_msgs::JointTrajectoryPoint pnt;
  pnt.time_from_start = ros::Duration(0.0);
  pnt.positions   = std::vector<double>(this->q().data(), this->q().data() + this->nAx() );
  pnt.velocities  = std::vector<double>(this->qd().data(), this->qd().data() + this->nAx() );
  pnt.accelerations.resize(this->nAx(), 0);
  pnt.effort.resize(this->nAx(), 0);

  cnr_interpolator_interface::JointTrajectoryPtr trj(new cnr_interpolator_interface::JointTrajectory({pnt}) );
  m_interpolator->setTrajectory(trj);
  m_regulator->starting(ros::Time::now());
  m_regulator_input->set_target_override(1.0);

  m_as->start();
  CNR_RETURN_TRUE(*this->logger());
}

template<class T>
bool FollowJointTrajectoryController<T>::doUpdate(const ros::Time& time, const ros::Duration& period)
{
  CNR_TRACE_START_THROTTLE_DEFAULT(this->logger());
  try
  {
    m_regulator_input->set_target_override(this->getTargetOverride());
    m_regulator_input->set_period(period);
    m_regulator->update(m_interpolator, m_regulator_input, m_regulator_output);

    this->setCommandPosition     (m_regulator_output->get_q());
    this->setCommandVelocity     (m_regulator_output->get_qd());
    this->setCommandAcceleration (m_regulator_output->get_qdd());
    this->setCommandEffort       (m_regulator_output->get_effort());

    std_msgs::Float64Ptr msg(new std_msgs::Float64());
    msg->data = m_regulator_output->get_scaling();
    this->publish(m_scaled_time_pub_idx, msg);

    m_is_in_tolerance = m_regulator_output->get_in_goal_tolerance();

    sensor_msgs::JointStatePtr js_msg(new sensor_msgs::JointState());
    js_msg->name = this->jointNames();
    js_msg->position.resize(this->nAx());
    js_msg->velocity.resize(this->nAx());
    js_msg->effort.resize(this->nAx(), 0);

    Eigen::VectorXd::Map(&js_msg->position[0], this->nAx()) = m_regulator_output->get_q();
    Eigen::VectorXd::Map(&js_msg->velocity[0], this->nAx()) = m_regulator_output->get_qd();
    Eigen::VectorXd::Map(&js_msg->effort  [0], this->nAx()) = m_regulator_output->get_effort();
    js_msg->header.stamp  = ros::Time::now();
    this->publish(m_target_pub_idx, js_msg);

  }
  catch (std::exception& e)
  {
    CNR_ERROR(*this->logger(), "Got and exception: '" << e.what()
               << "'(function input: Time: " << time.toSec() << " Duration: " << period.toSec() << ")" );
    CNR_RETURN_FALSE(*this->logger());
  }
  CNR_RETURN_TRUE_THROTTLE_DEFAULT(*this->logger());
}

template< class T>
bool FollowJointTrajectoryController<T>::doStopping(const ros::Time& time)
{
  CNR_TRACE_START(*this->logger());
  if (m_gh)
  {
    m_gh->setCanceled();
  }
  joinActionServerThread();
  m_gh.reset();
  CNR_RETURN_TRUE(*this->logger());
}

template< class T>
bool FollowJointTrajectoryController<T>::joinActionServerThread()
{
  m_preempted = true;
  if (m_as_thread.joinable())
  {
    m_as_thread.join();
  }
  m_preempted = false;
  return true;
}

template< class T>
void FollowJointTrajectoryController<T>::actionServerThread()
{
  CNR_DEBUG(*this->logger(), "START ACTION GOAL LOOPING");
  ros::WallRate lp(100);
  while (ros::ok())
  {
    lp.sleep();
    if (!m_gh)
    {
      CNR_ERROR(*this->logger(), "Goal handle is not initialized");
      break;
    }

    if ((m_preempted) 
    || (m_gh->getGoalStatus().status == actionlib_msgs::GoalStatus::RECALLED) 
    || (m_gh->getGoalStatus().status == actionlib_msgs::GoalStatus::PREEMPTED))
    {
      CNR_WARN(*this->logger(), "Action Server Thread Preempted");
      CNR_DEBUG(*this->logger(), "(m_gh->getGoalStatus().status == actionlib_msgs::GoalStatus::PREEMPTED)  = "
                << (int)(m_gh->getGoalStatus().status == actionlib_msgs::GoalStatus::PREEMPTED));
      CNR_DEBUG(*this->logger(), "m_preempted  = %d" << (int)m_preempted);
      CNR_DEBUG(*this->logger(), "(m_gh->getGoalStatus().status == actionlib_msgs::GoalStatus::RECALLED)  = "
                << (int)(m_gh->getGoalStatus().status == actionlib_msgs::GoalStatus::RECALLED));
      this->add_diagnostic_message((m_preempted ? "preempted" : "cancelled"), "INTERPOLATOR", "OK", true);

      control_msgs::FollowJointTrajectoryResult result;
      result.error_code = 0;
      result.error_string = "preempted";
      m_gh->setRejected(result);
      CNR_DEBUG(*this->logger(), "Preempted old goal DONE");

      break;
    }

    if(m_is_finished == 1 || m_is_in_tolerance)
    {
      this->add_diagnostic_message("Goal tolerance achieved!", "INTERPOLATOR", "OK", true);

      control_msgs::FollowJointTrajectoryResult result;
      m_gh->setSucceeded(result);

      break;
    }
    else if (m_is_finished == -2)
    {
      control_msgs::FollowJointTrajectoryResult result;
      result.error_code = -4;
      result.error_string = "Some problem occurs";

      this->add_diagnostic_message("Some problem occurs", "INTERPOLATOR", "ERROR", true);
      m_gh->setAborted(result);
      break;
    }
  }
  m_gh.reset();
  CNR_DEBUG(*this->logger(), "START ACTION GOAL END");
}

template< class T>
void FollowJointTrajectoryController<T>::actionGoalCallback(
                                    actionlib::ActionServer<control_msgs::FollowJointTrajectoryAction>::GoalHandle gh)
{
  CNR_TRACE_START(*this->logger());
  CNR_INFO(*this->logger(), "Received a goal");
  auto goal = gh.getGoal();

  boost::shared_ptr<actionlib::ActionServer<control_msgs::FollowJointTrajectoryAction>::GoalHandle> current_gh;

  if (m_gh)
  {
    // PREEMPTED OLD GOAL
    CNR_INFO(*this->logger(), "preempting old goal");
    m_gh->setAborted();
    joinActionServerThread();

    CNR_INFO(*this->logger(), "Goal Stopped");
  }
  else
  {
    CNR_INFO(*this->logger(), "No goals running yet");
  }

  current_gh.reset(new actionlib::ActionServer<control_msgs::FollowJointTrajectoryAction>::GoalHandle(gh));
  m_gh = current_gh;
  unsigned  int nPnt = goal->trajectory.points.size();

  if (nPnt == 0)
  {
    CNR_DEBUG(*this->logger(),"TRAJECTORY WITH NO POINT");
    control_msgs::FollowJointTrajectoryResult result;
    m_gh->setAccepted();
    current_gh->setSucceeded(result);
    m_gh.reset();
    return;
  }

  cnr_interpolator_interface::JointTrajectoryPtr interp_trj(new cnr_interpolator_interface::JointTrajectory());
  if (!trajectory_processing::sort_trajectory(this->jointNames(), goal->trajectory, *interp_trj->trj))
  {
    CNR_ERROR(*this->logger(), "Names are different");
    m_gh->setAborted();
    joinActionServerThread();
    return;
  }

  try
  {
    std::lock_guard<std::mutex> lock(m_mtx);
    m_interpolator->setTrajectory(interp_trj);
    m_regulator->starting(ros::Time::now());
    CNR_DEBUG(*this->logger(), "Starting managing new goal");
    std::stringstream ss1; ss1 << "[ "; for(const auto & q  : interp_trj->trj->points.front().positions ) ss1 << std::to_string( q ) << " "; ss1 << "]";
    std::stringstream ss2; ss2 << "[ "; for(const auto & qd : interp_trj->trj->points.front().velocities ) ss2 << std::to_string( qd ) << " ";  ss2 << "]";
    CNR_DEBUG(*this->logger(), "First Point of the trajectory:\n q : " + ss1.str() + "\n" + " qd:" + ss2.str() );
    m_is_finished=0;
  }
  catch(std::exception& e)
  {
     CNR_ERROR(*this->logger(), "Set Trajectory Failed");
  }

  m_gh->setAccepted();

  joinActionServerThread();

  m_as_thread = std::thread(&FollowJointTrajectoryController<T>::actionServerThread, this);

  CNR_RETURN_OK(*this->logger(), void() );
}

template< class T>
void FollowJointTrajectoryController<T>::actionCancelCallback(
                                  actionlib::ActionServer< control_msgs::FollowJointTrajectoryAction >::GoalHandle gh)
{
  CNR_TRACE_START(*this->logger());
  CNR_DEBUG(*this->logger(), "Cancel active goal Callback");
  if (m_gh)
  {
    m_gh->setCanceled();
    joinActionServerThread();
    m_gh.reset();

    try
    {
      std::lock_guard<std::mutex> lock(m_mtx);
      cnr_interpolator_interface::JointTrajectoryPtr interp_trj(new cnr_interpolator_interface::JointTrajectory());
      interp_trj->append(m_interpolator->getLastInterpolatedPoint());
      m_interpolator->setTrajectory(interp_trj);
      m_regulator->setRegulatorTime(ros::Duration(0));
    }
    catch(std::exception& e)
    {
      CNR_ERROR(*this->logger(), "Set Trajectory Failed. Exception:" << std::string(e.what()) );
    }
  }
  else
  {
    CNR_WARN(*this->logger(), "No goal to cancel");
  }
  CNR_RETURN_OK(*this->logger(), void());
}

}  // namespace control
}  // namespace cnr


#endif  // CNR_FOLLOW_JOINT_TRAJECTORY_CONTROLLER_CNR_FOLLOW_JOINT_TRAJECTORY_CONTROLLER_IMPL_H
