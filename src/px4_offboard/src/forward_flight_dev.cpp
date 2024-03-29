#include <ros/ros.h>
#include <math.h>
#include <geometry_msgs/Vector3.h>
#include <geometry_msgs/Point.h>
#include <geometry_msgs/Pose.h>
#include <geometry_msgs/PoseStamped.h>
#include <std_msgs/Header.h>
#include <std_msgs/Float32.h>
#include <mavros_msgs/PositionTarget.h>
#include <mavros_msgs/RCIn.h>
#include <mavros_msgs/ManualControl.h>
#include <mavros_msgs/State.h>
#include <mavros_msgs/SetMode.h>
#include <mavros_msgs/ParamGet.h>
#include <mavros_msgs/EstimatorStatus.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <boost/bind.hpp>

#include "forward_flight.h"
#include "math_utils.h"
#include "pid.h"
#include "px4_offboard/ControlCmd.h"
#include "px4_offboard/Pid.h"

class ForwardCtrl
{
public:
    float LOOP_RATE_DEFAULT = 30; // Hz
    double CMD_TIME_OUT = 0.3;  // second

public:
    ForwardCtrl()
    {  
        // Get ROS parameters
        std::string control_source;
        bool hover_test;

        ros::param::param<float>("~forward_speed", forward_speed, 0.5);
        ros::param::param<float>("~max_yawrate", max_yawrate, 45);
        ros::param::param<int>("~yaw_channel", yaw_channel, 3);
        ros::param::param<std::string>("~control_source", control_source, "rc");
        ros::param::param<bool>("~hover_test", hover_test, false);
        ros::param::param<bool>("~enable_ai", enable_ai, false);

        // Get PWM Range from vehicle
        ros::service::waitForService("/mavros/param/get");
        mavros_msgs::ParamGet srv1, srv2;
        srv1.request.param_id = "RC" + std::to_string(yaw_channel+1) + "_MIN";
        srv2.request.param_id = "RC" + std::to_string(yaw_channel+1) + "_MAX";

        if (ros::service::call("/mavros/param/get", srv1) && srv1.response.value.real != 0 
            && ros::service::call("/mavros/param/get", srv2) && srv2.response.value.real != 0)
        {
            ROS_INFO("Read PWM Range from vehicle successfully!");
            yaw_pwm_min = int(srv1.response.value.real);
            yaw_pwm_max = int(srv2.response.value.real);
        } else {
            ROS_WARN("Read PWM Range from vehicle failed!");
            yaw_pwm_min = 1000;
            yaw_pwm_max = 2000;
        }

        // set forward speed to 0 for hover test
        if (hover_test) {
            forward_speed = 0.0f;
        }
        
        // position z pid parameters
        float kp, ki, kd;
        float integral_limit, output_limit;
        ros::param::param<float>("~kp", kp, 1.0);
        ros::param::param<float>("~ki", ki, 0.0);
        ros::param::param<float>("~kd", kd, 0.0);
        ros::param::param<float>("~integral_limit", integral_limit, INF);
        ros::param::param<float>("~output_limit", output_limit, INF);

        ROS_INFO("Forward speed is set to %.2f m/s", forward_speed);
        ROS_INFO("Maximum yaw rate is set to %.2f deg/s", max_yawrate);
        ROS_INFO("Yaw channel index is %d", yaw_channel);
        ROS_INFO("Yaw Channel PWM range in [%d, %d]", yaw_pwm_min, yaw_pwm_max);
        ROS_INFO("Control source is %s", control_source.c_str());
        ROS_INFO("Position Z PID: kp=%.2f, ki=%.2f, kd=%.2f", kp, ki, kd);
        ROS_INFO("Position Z PID: integral_limit=%.2f, output_limit=%.2f", integral_limit, output_limit);

        // force manual control
        force_manual = false;

        // init estimator flag
        estimator_is_healthy = true;

        // ROS Publisher:
        target_setpoint_pub = nh.advertise<mavros_msgs::PositionTarget>("/mavros/setpoint_raw/local", 5);
        pos_z_pid_pub = nh.advertise<px4_offboard::Pid>("/my_controller/pos_z_pid", 5);

        // ROS Subscriber:
        // 1) state
        state_sub = nh.subscribe<mavros_msgs::State>("/mavros/state", 5, 
                &ForwardCtrl::StateCallback, this);
        // 2) local pose
        local_pose_sub = nh.subscribe<geometry_msgs::PoseStamped>("/mavros/local_position/pose", 5,
                &ForwardCtrl::LocalPoseCallback, this);
        // 3) local pose setpoint
        setpoint_raw_sub = nh.subscribe<mavros_msgs::PositionTarget>("/mavros/setpoint_raw/target_local", 5, 
                    boost::bind(&ForwardCtrl::SetpointRawCallback, this, _1));
        // 4) control command
        custom_cmd_sub = nh.subscribe<px4_offboard::ControlCmd>("/my_controller/yaw_cmd", 5, 
                boost::bind(&ForwardCtrl::CustomCmdCallback, this, _1));
        if (control_source == "joystick")
        {
            rc_sub = nh.subscribe<mavros_msgs::ManualControl>("/mavros/manual_control/control", 5, 
                    boost::bind(&ForwardCtrl::JoystickCallback, this, _1));
        }
        else
        {
            rc_sub = nh.subscribe<mavros_msgs::RCIn>("/mavros/rc/in", 5, 
                    boost::bind(&ForwardCtrl::RCInCallback, this, _1, yaw_channel));
        }
        last_custom_cmd_time = ros::Time::now();
        last_rc_cmd_time = ros::Time::now();
        // 5) estimator status
        estimator_status_sub = nh.subscribe<mavros_msgs::EstimatorStatus>("/mavros/estimator_status", 5,
                &ForwardCtrl::EstimatorStatusCallback, this); 

        // init set_point_local_target
        InitializeTarget();

        // init position z pid controller
        pos_z_pid = new PID(kp, ki, kd, (1./LOOP_RATE_DEFAULT), integral_limit, output_limit);

        // reset FSM
        reset();
    }

    void reset()
    {
        yaw_cmd = 0.0f;
        fsm_state = IDLE;
    }

    void run()
    {   
        ros::Rate loop_rate(LOOP_RATE_DEFAULT);
        ROS_INFO("Node Started!");
        while (ros::ok() && estimator_is_healthy) {
            target.header.stamp = ros::Time::now();
            target.header.seq++;

            // if cmd callback timeout
            if (fsm_state == AI && ros::Time::now() - last_custom_cmd_time > ros::Duration(CMD_TIME_OUT)) {
                ROS_WARN_THROTTLE(1, "Custom Command Time Out!");
                fsm_state = MANUAL;
            }

            if (ros::Time::now() - last_rc_cmd_time > ros::Duration(CMD_TIME_OUT)) {
                ROS_WARN_THROTTLE(1, "RC Command Time Out!");
                reset();
            }
            
            // ROS_INFO_THROTTLE(1, "FSM = %i", fsm_state);
            if (current_state.mode == "OFFBOARD") { 
                // target.coordinate_frame = target.FRAME_BODY_NED;
                // target.type_mask = 0b010111000111;
                // velocity (2 seconds fade in)
                double ratio = 1.0;
                double dt = (ros::Time::now() - offboard_start_time).toSec();
                if (dt < 2.0) { 
                    ratio = dt / 2.0;
                }
                geometry_msgs::Vector3 velocity;
                // forward flight with a constant speed
                velocity.x = forward_speed * ratio;
                velocity.y = 0.0; 
                // calculate position z pid
                velocity.z = pos_z_pid->calculate(target_pose_z, current_pose_z);
                // update target
                target.velocity = velocity;
                // yaw rate
                float filtered_cmd = yaw_cmd;
                if (abs(filtered_cmd) < 1e-2){
                    filtered_cmd = 0.0f;
                }
                target.yaw_rate = (-filtered_cmd) * max_yawrate * DEG2RAD;
                // publish pid internal states
                publish_pid_internal(target_pose_z, current_pose_z, velocity.z);
            } 
            // publisg setpoint_raw
            target_setpoint_pub.publish(target);
            ros::spinOnce();
            loop_rate.sleep();
        }
        ROS_INFO("Stop Offboard Mode!");

        // if estimator is unhealthy
        if (!estimator_is_healthy && (current_state.mode == "OFFBOARD" || current_state.mode == "POSCTL")) {
            set_mode("ALTCTL");
        }
    }

private:
    void InitializeTarget() {
        // this should hover a drone at current location
        target.header.frame_id = "base_link";
        target.coordinate_frame = target.FRAME_BODY_NED; // {MAV_FRAME_LOCAL_NED:1, MAV_FRAME_BODY_NED:8}
        target.type_mask = 0b010111000111; // bitmask
        target.position = geometry_msgs::Point();
        target.velocity = geometry_msgs::Vector3();
        target.acceleration_or_force = geometry_msgs::Vector3();
        target.yaw = 0.0;
        target.yaw_rate = 0.0;
    }

    void StateCallback(const mavros_msgs::State::ConstPtr& msg) {
        if (msg->mode == "OFFBOARD" && current_state.mode != "OFFBOARD") {
            ROS_INFO("Switched to OFFBOARD Mode!");
            offboard_start_time = ros::Time::now();
            if (!isnan(target_pose_z)) {
                target_pose_z = last_target_position_z;
            }
            else {
                target_pose_z = 5.0;
            }
            // reset position z pid again for safety
            pos_z_pid->reset();
        }

        if (msg->mode != "OFFBOARD" && current_state.mode == "OFFBOARD") {
            ROS_INFO("Switched to %s Mode!", (msg->mode).c_str());
            InitializeTarget();
            // reset position z pid
            pos_z_pid->reset();
            // reset FSM
            reset();
        }
        
        current_state = *msg;
    }
    
    void RCInCallback(const mavros_msgs::RCIn::ConstPtr& msg, int channel_index) {
        if (msg) {
            last_rc_cmd_time = msg->header.stamp;
            if (fsm_state == IDLE) {
                fsm_state = MANUAL;
            }
            if (fsm_state == MANUAL) {
                float cmd = rc_mapping(msg->channels[channel_index], yaw_pwm_min, yaw_pwm_max);
                yaw_cmd = constrain_float(cmd, -1.0, 1.0);
            }
        }
    }

    void JoystickCallback(const mavros_msgs::ManualControl::ConstPtr& msg) {
        if (msg) {
            last_rc_cmd_time = msg->header.stamp;
            if (fsm_state == IDLE) {
                fsm_state = MANUAL;
            }
            if (fsm_state == MANUAL) {
                float cmd = msg->r;
                yaw_cmd = constrain_float(cmd, -1.0, 1.0);
            }
            // only for sitl test
            if (msg->buttons == 1024) {
                force_manual = true;
            } else {
                force_manual = false;
            }
        }
    }

    void CustomCmdCallback(const px4_offboard::ControlCmd::ConstPtr& msg) {
        if (msg && enable_ai) {
            last_custom_cmd_time = msg->header.stamp;
            if (fsm_state == MANUAL && msg->is_active && !force_manual) {
                fsm_state = AI;
            }
            if (fsm_state == AI) {
                if (!msg->is_active || force_manual) {
                    fsm_state = MANUAL;
                    yaw_cmd = 0.0f;
                } else {
                    float cmd = msg->command;
                    yaw_cmd = constrain_float(cmd, -1.0, 1.0);
                }
            }
        }
    }

    void LocalPoseCallback(const geometry_msgs::PoseStamped::ConstPtr& msg) {
        if (msg) {
            // yaw angle (rad)
            tf2::Quaternion q_tf;
            tf2::convert((msg->pose).orientation, q_tf);
            tf2::Matrix3x3 q_mat(q_tf);
            tf2Scalar yaw, pitch, roll;
            q_mat.getEulerYPR(yaw, pitch, roll);
            yaw_rad = wrap_2PI((float)yaw);
            
            // current local position z
            current_pose_z = msg->pose.position.z;
        }
    }

    void SetpointRawCallback(const mavros_msgs::PositionTarget::ConstPtr& msg) {
        if (msg && !isnan(msg->position.z)) {
            last_target_position_z = msg->position.z;
            // ROS_INFO("Last target position z: %.3f", last_target_position_z);
        }
    }

    void EstimatorStatusCallback(const mavros_msgs::EstimatorStatus::ConstPtr& msg) {
        // if horizontal position estimator has an error
        if (!msg->pos_horiz_rel_status_flag || !msg->pos_horiz_abs_status_flag) {
            ROS_ERROR("Estimator error!!! Land Immediately!!!");
            estimator_is_healthy = false;
            force_manual = true;
            reset();
            set_mode("ALTCTL");
        }
    }

    void set_mode(std::string mode_name) {
        ros::ServiceClient set_mode_client = nh.serviceClient<mavros_msgs::SetMode>("mavros/set_mode");
        mavros_msgs::SetMode new_mode;
        new_mode.request.custom_mode = mode_name;
        while (!new_mode.response.mode_sent) {
            set_mode_client.call(new_mode);
        }
    }

    void publish_pid_internal(float target, float current, float output)
    {
        px4_offboard::Pid pid_msg;
        pid_msg.header.stamp = ros::Time::now();
        pid_msg.k_p = pos_z_pid->get_kp();
        pid_msg.k_i = pos_z_pid->get_ki();
        pid_msg.k_d = pos_z_pid->get_kd();
        pid_msg.error = pos_z_pid->get_error();
        pid_msg.derivative = pos_z_pid->get_derivative();
        pid_msg.integral = pos_z_pid->get_integral();
        pid_msg.target = target;
        pid_msg.current = current;
        pid_msg.output = output;
        pos_z_pid_pub.publish(pid_msg);
    }

private:
    ros::NodeHandle nh;
    ros::Publisher target_setpoint_pub;
    ros::Publisher pos_z_pid_pub;
    ros::Subscriber state_sub;
    ros::Subscriber local_pose_sub;
    ros::Subscriber setpoint_raw_sub;
    ros::Subscriber rc_sub;
    ros::Subscriber custom_cmd_sub;
    ros::Subscriber estimator_status_sub;

    mavros_msgs::State current_state;
    mavros_msgs::PositionTarget target;

    bool enable_ai;
    bool estimator_is_healthy;
    FSMState fsm_state;

    float forward_speed; // m/s
    float max_yawrate; // deg/s
    
    int yaw_channel;
    int yaw_pwm_min, yaw_pwm_max;
    float yaw_cmd; // normalized to [-1.0, 1.0]
    float yaw_rad; // rad (Down is positive)

    float current_pose_z; // m
    float target_pose_z; // m
    float last_target_position_z; // m

    ros::Time last_custom_cmd_time;
    ros::Time last_rc_cmd_time;
    ros::Time offboard_start_time;

    PID *pos_z_pid; // position z pid control

    bool force_manual; // only for sitl test
};

int main(int argc, char **argv)
{
    ros::init(argc, argv, "forward_flight_node");
    ForwardCtrl controller;

    ros::Duration(1.0).sleep(); // sleep for one second
    controller.run();

    return 0;
}