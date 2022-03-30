#include <ros/ros.h>
#include <boost/bind.hpp>
#include <geometry_msgs/Vector3.h>
// #include <geometry_msgs/Quaternion.h>
#include <std_msgs/Header.h>
#include <mavros_msgs/AttitudeTarget.h>
#include <mavros_msgs/RCIn.h>
#include <mavros_msgs/State.h>
// #include <tf2/LinearMath/Quaternion.h>
// #include <tf2_geometry_msgs/tf2_geometry_msgs.h>

#include "common.h"
#include "math_utils.h"

#define LOOP_RATE_DEFAULT   10

float linear_mapping(uint16_t value) {
    float result = ((float)(value) - CH_MIN) / (CH_MAX - CH_MIN); 
    result = result * 2.0 - 1.0;
    return result;
}

class AttitudeCtrl
{
public:
    AttitudeCtrl()
    {
        target.header = std_msgs::Header();
        target.header.frame_id = "base_drone";
        
        // Publisher
        target_setpoint_pub = nh.advertise<mavros_msgs::AttitudeTarget>("/mavros/setpoint_raw/attitude", 1);  

        // Subscriber
        rcin_sub = nh.subscribe<mavros_msgs::RCIn>("/mavros/rc/in", 
                10, boost::bind(&AttitudeCtrl::RCInCallback, this, _1, YAW_CHANNEL));
    }

    void run()
    {
        ros::Rate loop_rate(10);
        ROS_INFO("Start Offboard Mode!");
        while (ros::ok()) {
            target.header.stamp = ros::Time::now();
            target.header.seq++;

            // bitmask
            target.type_mask = 128; //ignore orientation
            // body rate
            geometry_msgs::Vector3 body_rate;
            body_rate.x = 0.0;
            body_rate.y = 0.0;
            body_rate.z = yaw_cmd * MAX_YAWRATE;
            target.body_rate = body_rate;
            // thrust           
            target.thrust = 0.7;

            target_setpoint_pub.publish(target);
            ros::spinOnce();
            loop_rate.sleep();
        }
        ROS_INFO("Stop Offboard Mode!");
    }

private:
    void RCInCallback(const mavros_msgs::RCIn::ConstPtr& msg, int channel_index) {
        float cmd = linear_mapping(msg->channels[channel_index]);
        yaw_cmd = constrain_float(cmd, -1.0, 1.0);
    }

private:
    ros::NodeHandle nh;
    ros::Publisher target_setpoint_pub;
    ros::Subscriber rcin_sub;

    mavros_msgs::AttitudeTarget target;
    float yaw_cmd;
};

int main(int argc, char **argv)
{
    ros::init(argc, argv, "test_node");
    ros::NodeHandle nh("~");

    AttitudeCtrl ctrl;
    ros::Duration(1.0).sleep();
    ctrl.run();
}