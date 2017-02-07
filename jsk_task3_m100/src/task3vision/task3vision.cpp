/*
 jsk_mbzirc_task
 */

// Author: Chen

//ros
#include <ros/ros.h>
#include <image_transport/image_transport.h>
#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <tf/tf.h>
#include <dynamic_reconfigure/server.h>
#include <jsk_task3_m100/FilterparamConfig.h>

#include <string.h>
#include <vector>
//OPENCV
#include <opencv2/highgui/highgui.hpp>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/gpu/gpu.hpp>
#include <opencv2/core/core.hpp>

//msg headers.
#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
//#include <jsk_mbzirc_msgs/ProjectionMatrix.h>
#include <nav_msgs/Odometry.h>
#include <tf/transform_listener.h>
#include <geometry_msgs/Pose.h>
#include <geometry_msgs/Twist.h>
#include <std_msgs/Float64.h>
#include <iostream>
#include <std_msgs/Bool.h>
#include <geometry_msgs/PoseArray.h>
//for test
#include <tf/transform_broadcaster.h>
#include <ctime>




class task3_vision
{
typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::CameraInfo,nav_msgs::Odometry> MySyncPolicy;

private:

    message_filters::Subscriber<sensor_msgs::Image> *img_sub_;
    message_filters::Subscriber<sensor_msgs::CameraInfo> *camera_info_sub_;
    message_filters::Subscriber<nav_msgs::Odometry> *uav_odom_sub_;
    ros::NodeHandle nh_;
    message_filters::Synchronizer<MySyncPolicy> *sync;
    tf::Transform BaseToCamera;
    geometry_msgs::PoseArray object_clusteres;

    ros::Publisher obj_clus_pub_;
    //task3
    ros::Subscriber pick_state_sub_;
    std_msgs::Bool pick_state;
    //dynamic reconfigure
    dynamic_reconfigure::Server<jsk_mbzirc_tasks::FilterparamConfig> server;
    dynamic_reconfigure::Server<jsk_mbzirc_tasks::FilterparamConfig>::CallbackType f;
    int s_max,v_max,s_min,v_min;
    int r_off,g_off,b_off,ye_off,or_off;
    bool debug_show;
    int cluster_thre_dist;
    int downsample_cluster_size, min_cluster_size;
    std::vector<std::vector<cv::Point> > clusters;
#define HSVRED 0
#define HSVGREEN 60
#define HSVBLUE 120
#define HSVYELLOW 30
#define HSVORANGE 20
    //functor
    struct EuclideanDistanceFunctor
    {
        int _dist2;
        EuclideanDistanceFunctor(int dist) : _dist2(dist*dist) {}

        bool operator()(const cv::Point& lhs, const cv::Point& rhs) const
        {
            return ((lhs.x - rhs.x)*(lhs.x - rhs.x) + (lhs.y - rhs.y)*(lhs.y - rhs.y)) < _dist2;
        }
    };
#define Ground_Z 0.0
   //test
    tf::TransformBroadcaster br;
public:
    void Init()
    {
        img_sub_  = new message_filters::Subscriber<sensor_msgs::Image>(nh_,"/image_zenmus",1);
        camera_info_sub_ = new message_filters::Subscriber<sensor_msgs::CameraInfo>(nh_,"/dji_sdk/camera_info", 1);
        uav_odom_sub_ = new message_filters::Subscriber<nav_msgs::Odometry>(nh_,"/dji_sdk/odometry",1);
        sync = new message_filters::Synchronizer<MySyncPolicy>(MySyncPolicy(10), *img_sub_, *camera_info_sub_, *uav_odom_sub_);
        sync->registerCallback(boost::bind(&task3_vision::ImageCallback,this,_1,_2,_3));

        obj_clus_pub_ = nh_.advertise<geometry_msgs::PoseArray>("/obj_cluster/centroid_pose",1);

        /*** dynamic reconfigure ***/
        f = boost::bind(&task3_vision::DynamicReconfigureCallback, this, _1,_2);
        server.setCallback(f);
        cluster_thre_dist = 18;
        downsample_cluster_size =1000;
        min_cluster_size = 10;

        //initialize base_link to camera optical link
        BaseToCamera.setOrigin(tf::Vector3(0,0,-0.05));
        BaseToCamera.setRotation(tf::Quaternion(0.707, -0.707, 0.000, -0.000));
        std::cout<<"initialization finished"<<std::endl;

    }

    void ImageCallback(const sensor_msgs::ImageConstPtr& img,
                       const sensor_msgs::CameraInfoConstPtr& cam_info,
                       const nav_msgs::OdometryConstPtr& odom);
    void DynamicReconfigureCallback(jsk_mbzirc_tasks::FilterparamConfig &config, uint32_t level)
    {
      s_max = config.hsv_s_max; v_max = config.hsv_v_max;
      s_min = config.hsv_s_min; v_min = config.hsv_v_min;
      r_off = config.r_offset;g_off = config.g_offset;b_off = config.b_offset;
      ye_off = config.ye_offset;or_off = config.or_offset;
      debug_show = config.debug_show;

      cluster_thre_dist = config.cluster_threhold_dist;
      downsample_cluster_size = config.downsample_cluster_size;
      min_cluster_size = config.min_cluster_size;
    }

    //clustering of one filtered image
     std::vector<std::vector<cv::Point> > EuclideanCluster(
             cv::Mat *image_input, cv::Mat3b *lbl, int n_factor, char c);

    // class's family...
    ~task3_vision()
    {    }
};


//find zeros function
inline std::vector<cv::Point> DownSamplePoints(cv::Mat *img_input, int n)
{
    std::vector<cv::Point> pts_raw;
    std::vector<cv::Point> pts;
    if(cv::countNonZero(*img_input))
    {
        findNonZero(*img_input, pts_raw);
        //clustered n_labels objects, however the problem is when getting near,
        //too many points to be calculated, need to downsample...
        //downsample the points if there are too much
        pts.resize(pts_raw.size()/n);
        for(long i = 0; i < pts.size(); i+=n)
        {
            pts[i] = pts_raw[i*n];
        }
    }
    return pts;
}

//Euclideanclustering function
std::vector<std::vector<cv::Point> > task3_vision::EuclideanCluster(
        cv::Mat *image_input, cv::Mat3b *lbl, int n_factor, char c)
{
    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Point> pts = DownSamplePoints(image_input, n_factor);

    if(pts.size())
    {
        std::vector<int> labels;
        int n_labels = cv::partition(pts, labels, EuclideanDistanceFunctor(cluster_thre_dist));
        contours.resize(n_labels);  //contours are the clusteres with points.
        for (int i = 0; i < pts.size(); ++i)
        {
            contours[labels[i]].push_back(pts[i]);
        }
        /****
     * labels vector gives every point a label by labels[pts[n]], we need a parameter to remove
     * the labels that only contains small amount of points which could be considered as noise.
    ****/
        //check contours size and remove the clusteres with less points... also get the center...
        for (std::vector<std::vector<cv::Point> >::iterator itr = contours.begin(); itr != contours.end(); ++itr)
        {
            if(itr->size() < min_cluster_size)
            {
                contours.erase(itr);
                if(itr == contours.end())
                    break;
            }
            else
            {
                //get the centroid of each contours
            }
        }
        int label_size = contours.size();
        for (int i = 0; i < label_size; i++)
          {
            cv::Vec3b color;
            switch(c)
            {
            case 'a':
                color = cv::Vec3b(rand() & 255, rand() & 255, rand() & 255);
                break;
            case 'r':
                color = cv::Vec3b(0, 0, 255);
                break;
            case 'g':
                color = cv::Vec3b(0, 255, 0);
                break;
            case 'b':
                color = cv::Vec3b(255, 0, 0);
                break;
            case 'y':
                color = cv::Vec3b(0, 255, 255);
                break;
            case 'o':
                color = cv::Vec3b(0, 130, 255);
                break;
            default:
                break;
            }
            for(int j = 0; j< contours[i].size(); j++)
            {
                (*lbl)(contours[i].at(j)) = color;
            }
          }
    }
    return contours;
}



/****
 * Image call back function
****/
void task3_vision::ImageCallback(const sensor_msgs::ImageConstPtr& img,
                                       const sensor_msgs::CameraInfoConstPtr& cam_info,
                                       const nav_msgs::OdometryConstPtr& odom)
{
    try
    {
        cv::Mat raw_image = cv_bridge::toCvCopy(img,"bgr8")->image;
        cv::Mat hsv_image;
        cv::Mat hsv_filtered_r, hsv_filtered_g, hsv_filtered_b, hsv_filtered_ye, hsv_filtered_or, hsv_filtered_all;
        /***********counting the timer********/
        std::clock_t start;
        double duration;
        start = std::clock();

        /**********first HSI Filter**********/
        {
        cv::cvtColor(raw_image,hsv_image,cv::COLOR_BGR2HSV);
        cv::Mat hsv_r_down;
        //r
        cv::inRange(hsv_image, cv::Scalar(180+HSVRED-r_off, s_min, v_min, 0), cv::Scalar(180, s_max, v_max, 0), hsv_filtered_r);
        cv::inRange(hsv_image, cv::Scalar(0, s_min, v_min, 0), cv::Scalar(r_off+HSVRED, s_max, v_max, 0), hsv_r_down);
        hsv_filtered_r |=hsv_r_down;
        //g
        cv::inRange(hsv_image, cv::Scalar(HSVGREEN-g_off, s_min, v_min, 0), cv::Scalar(HSVGREEN+g_off, s_max, v_max, 0), hsv_filtered_g);
        //b
        cv::inRange(hsv_image, cv::Scalar(HSVBLUE-b_off, s_min, v_min, 0), cv::Scalar(HSVBLUE+b_off, s_max, v_max, 0), hsv_filtered_b);
        //ye
        cv::inRange(hsv_image, cv::Scalar(HSVYELLOW-ye_off, s_min, v_min, 0), cv::Scalar(HSVYELLOW+ye_off, s_max, v_max, 0), hsv_filtered_ye);
        //or
        cv::inRange(hsv_image, cv::Scalar(HSVORANGE-or_off, s_min, v_min, 0), cv::Scalar(HSVORANGE+or_off, s_max, v_max, 0), hsv_filtered_or);
        }
        //show image of r g b ye or
        if(debug_show)
        {
            cv::imshow("red space",hsv_filtered_r);
            cv::imshow("green space",hsv_filtered_g);
            cv::imshow("blue space",hsv_filtered_b);
            cv::imshow("yellow space",hsv_filtered_ye);
            cv::imshow("orange space",hsv_filtered_or);
        }
        else
        {
            cv::destroyWindow("red space");cv::destroyWindow("green space");cv::destroyWindow("blue space");
            cv::destroyWindow("yellow space");cv::destroyWindow("orange space");
        }

        /**********Then Euclidean Clustering**********/
        //clustering..
        hsv_filtered_all = hsv_filtered_r | hsv_filtered_g | hsv_filtered_b |  hsv_filtered_ye |  hsv_filtered_or;

        /***
         * Two methods:
         * 1: combine all and do clustering
         * 2: Every filter do clustering
         * 3: Findcontours
         * 4: Hough Circle Transform
         ***/
        int ptr_all = cv::countNonZero(hsv_filtered_r) + cv::countNonZero(hsv_filtered_g)
                + cv::countNonZero(hsv_filtered_b) + cv::countNonZero(hsv_filtered_ye)
                + cv::countNonZero(hsv_filtered_or);
        //n_ds is the downsample factor
        int n_ds = ptr_all/downsample_cluster_size + 1; //begin from  1

        this->clusters.clear();
        // Build a vector of random color, one for each class (label) and draw the labels
        cv::Mat3b lbl(hsv_filtered_all.rows, hsv_filtered_all.cols, cv::Vec3b(0, 0, 0));
        //method 1:
        // this->clusters = this->EuclideanCluster(&hsv_filtered_all, n_ds);
        //method 2:
        {
//                    this->clusters = this->EuclideanCluster(&hsv_filtered_r, &lbl, n_ds, 'r');
//                    this->clusters = this->EuclideanCluster(&hsv_filtered_g, &lbl, n_ds, 'g');
//                    this->clusters = this->EuclideanCluster(&hsv_filtered_b, &lbl, n_ds, 'b');
//                    this->clusters = this->EuclideanCluster(&hsv_filtered_ye, &lbl, n_ds, 'y');
//                    this->clusters = this->EuclideanCluster(&hsv_filtered_or, &lbl, n_ds, 'o');
//                    imshow("Labels", lbl);
        }
        //method 3:
        {
//            std::vector<cv::Vec4i> hierarchy;
//            std::vector<std::vector<cv::Point> > contours;
//            cv::Mat canny_output;
//            /// Detect edges using canny
//            Canny( hsv_filtered_all, canny_output, 100, 100, 3 );
//            cv::findContours(canny_output, contours, hierarchy, CV_RETR_TREE, CV_CHAIN_APPROX_SIMPLE);
//            for( int i = 0; i< contours.size(); i++ )
//            {
//                cv::Scalar color;
//                color = cv::Scalar(rand() & 255, rand() & 255, rand() & 255);
//                drawContours( hsv_filtered_all, contours, i, color, 2, 8, hierarchy, 0, cv::Point() );
//            }
        }
        //method 4:
        {

            std::vector<cv::Vec3f> circles;

            /// Apply the Hough Transform to find the circles
            cv::HoughCircles( hsv_filtered_all, circles, CV_HOUGH_GRADIENT,
                              1, 10, 100, 30, 1, 200);
            /// Draw the circles detected
            for( size_t i = 0; i < circles.size(); i++ )
            {
                cv::Point center(cvRound(circles[i][0]), cvRound(circles[i][1]));
                int radius = cvRound(circles[i][2]);
                cv::Scalar color;
                color = cv::Scalar(rand() & 255, rand() & 255, rand() & 255);
                // circle center
                cv::circle( hsv_filtered_all, center, 3, color, -1, 8, 0 );
                // circle outline
                cv::circle( hsv_filtered_all, center, radius, color, 3, 8, 0 );
             }

        }


        cv::imshow("all space",hsv_filtered_all);

        //timer end
        duration = ( std::clock() - start ) / (double) CLOCKS_PER_SEC;
        std::cout<<"process_time is "<< duration << " second" <<'\n';

        cv::waitKey(10);
    }

    catch (cv_bridge::Exception& e)
    {
        ROS_ERROR("Could not convert from '%s' to 'bgr8'.", img->encoding.c_str());
    }
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "task3_vision");
    task3_vision u_i2p;
    u_i2p.Init();

    ros::spin();

}
