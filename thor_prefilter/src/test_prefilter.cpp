#include <thor_prefilter/thor_prefilter.h>
#include <ros/ros.h>

int main(int argc, char **argv)
{
  ros::init(argc, argv, "test_thor_math");
  ros::NodeHandle nh;

  ros::Publisher pub = nh.advertise<trajectory_msgs::JointTrajectoryPoint>("points", 1);
  thor::ThorPrefilter prefilter;
  unsigned int nAx = 1;
  double time = 5;
  trajectory_msgs::JointTrajectoryPoint p0;
  p0.positions.resize(nAx, 0);
  p0.velocities.resize(nAx, 0);
  p0.accelerations.resize(nAx, 0);
  p0.effort.resize(nAx, 0);
  p0.time_from_start = ros::Duration(0);

  trajectory_msgs::JointTrajectoryPoint pf;
  pf.positions.resize(nAx, -5);
  pf.velocities.resize(nAx, 0);
  pf.accelerations.resize(nAx, 0);
  pf.effort.resize(nAx, 0);
  pf.time_from_start = ros::Duration(time);

  cnr_interpolator_interface::JointTrajectoryPtr ttrj(new cnr_interpolator_interface::JointTrajectory());
  ttrj->trj->points.push_back(p0);
  ttrj->trj->points.push_back(pf);

  prefilter.setTrajectory(ttrj);
  prefilter.setSplineOrder(4);
  for (double t = 0; t < 2 * time; t += 0.01)
  {
    ROS_INFO("Time = %f", t);
    cnr_interpolator_interface::JointInputPtr input(new cnr_interpolator_interface::JointInput());
    cnr_interpolator_interface::JointOutputPtr output(new cnr_interpolator_interface::JointOutput());
    if (!prefilter.interpolate(ros::Duration(t), output))
    {
      ROS_WARN("Something wrong");
      return 1;
    }


    std::cout << "positions: ";
    for (double& p : output->pnt.positions)
      std::cout << p << ",";
    std::cout << std::endl;

    std::cout << "velocities: ";
    for (double& v : output->pnt.velocities)
      std::cout << v << ",";
    std::cout << std::endl;

    std::cout << "accelerations: ";
    for (double& a : output->pnt.accelerations)
      std::cout << a << ",";
    std::cout << std::endl;

    pub.publish(output->pnt);
    ros::Duration(0.01).sleep();
  }
  return 0;
}
