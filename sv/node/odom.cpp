// ros
#include <cv_bridge/cv_bridge.h>
#include <image_transport/camera_subscriber.h>
#include <image_transport/image_transport.h>
#include <pcl_ros/point_cloud.h>
#include <ros/ros.h>
#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/Imu.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_ros/transform_listener.h>

#include "sv/llol/factor.h"
#include "sv/llol/grid.h"
#include "sv/llol/match.h"
#include "sv/llol/pano.h"
#include "sv/llol/scan.h"
#include "sv/node/conv.h"
#include "sv/node/viz.h"
#include "sv/util/manager.h"

// ceres
#include <ceres/ceres.h>

namespace sv {

namespace cs = ceres;
using visualization_msgs::MarkerArray;

LidarScan Msg2Scan(const sensor_msgs::Image& image_msg,
                   const sensor_msgs::CameraInfo& cinfo_msg) {
  cv_bridge::CvImageConstPtr cv_ptr;
  cv_ptr = cv_bridge::toCvCopy(image_msg, "32FC4");

  return {image_msg.header.stamp.toSec(),    // t
          cinfo_msg.K[0],                    // dt
          cv_ptr->image,                     // xyzr
          cv::Range(cinfo_msg.roi.x_offset,  // col_rg
                    cinfo_msg.roi.x_offset + cinfo_msg.roi.width)};
}

class OdomNode {
 private:
  ros::NodeHandle pnh_;
  image_transport::ImageTransport it_;
  image_transport::CameraSubscriber sub_camera_;
  ros::Subscriber sub_imu_;
  ros::Publisher pub_marray_;
  ros::Publisher pub_pano_;
  tf2_ros::Buffer tf_buffer_;
  tf2_ros::TransformListener tf_listener_;
  tf2_ros::TransformBroadcaster tf_broadcaster_;

  std::string lidar_frame_;
  std::string odom_frame_{"odom"};
  std::string pano_frame_{"pano"};
  std::optional<geometry_msgs::TransformStamped> tf_imu_lidar_;

  bool vis_{};
  bool tbb_{};
  bool init_{false};

  LidarSweep sweep_;
  SweepGrid grid_;
  DepthPano pano_;
  ProjMatcher matcher_;

  // Pose
  double t_;
  Sophus::SE3d T_p_s_;

  TimerManager tm_{"llol"};
  StatsManager sm_{"llol"};

  visualization_msgs::MarkerArray marray_;

 public:
  explicit OdomNode(const ros::NodeHandle& pnh)
      : pnh_{pnh}, it_{pnh}, tf_listener_{tf_buffer_} {
    sub_imu_ = pnh_.subscribe("imu", 100, &OdomNode::ImuCb, this);
    sub_camera_ = it_.subscribeCamera("image", 10, &OdomNode::CameraCb, this);
    pub_marray_ = pnh_.advertise<MarkerArray>("marray", 1);
    pub_pano_ = pnh_.advertise<Cloud>("pano", 1);

    vis_ = pnh_.param<bool>("vis", true);
    ROS_INFO_STREAM("Visualize: " << (vis_ ? "True" : "False"));

    tbb_ = pnh_.param<bool>("tbb", false);
    ROS_INFO_STREAM("Use tbb: " << (tbb_ ? "True" : "False"));

    auto pano_nh = ros::NodeHandle{pnh_, "pano"};
    const auto pano_rows = pano_nh.param<int>("rows", 256);
    const auto pano_cols = pano_nh.param<int>("cols", 1024);
    const auto pano_hfov = pano_nh.param<double>("hfov", -1.0);
    pano_ = DepthPano({pano_cols, pano_rows}, Deg2Rad(pano_hfov));
    ROS_INFO_STREAM(pano_);
  }

  void ImuCb(const sensor_msgs::Imu& imu_msg) {
    return;
    // tf stuff
    if (lidar_frame_.empty()) {
      ROS_WARN_STREAM_THROTTLE(1, "Lidar frame is not set, waiting");
      return;
    }

    if (!tf_imu_lidar_.has_value()) {
      try {
        tf_imu_lidar_ = tf_buffer_.lookupTransform(
            imu_msg.header.frame_id, lidar_frame_, ros::Time(0));

        const auto& t = tf_imu_lidar_->transform.translation;
        const auto& q = tf_imu_lidar_->transform.rotation;
        const Eigen::Vector3d t_imu_lidar{t.x, t.y, t.z};
        const Eigen::Quaterniond q_imu_lidar{q.w, q.x, q.y, q.z};
        const Sophus::SE3d T_imu_lidar{q_imu_lidar, t_imu_lidar};

        ROS_INFO_STREAM("Transform from lidar to imu\n"
                        << T_imu_lidar.matrix());
      } catch (tf2::TransformException& ex) {
        ROS_WARN_STREAM(ex.what());
        return;
      }
    }
  }

  void Init(const sensor_msgs::CameraInfo& cinfo_msg) {
    /// Init sweep
    sweep_ = LidarSweep{cv::Size(cinfo_msg.width, cinfo_msg.height)};
    ROS_INFO_STREAM(sweep_);

    /// Init grid
    auto gnh = ros::NodeHandle{pnh_, "grid"};
    GridParams gp;
    gp.cell_rows = gnh.param<int>("cell_rows", 2);
    gp.cell_cols = gnh.param<int>("cell_cols", 16);
    gp.nms = gnh.param<bool>("nms", false);
    gp.max_score = gnh.param<double>("max_score", 0.05);
    grid_ = SweepGrid(sweep_.size(), gp);
    ROS_INFO_STREAM(grid_);

    /// Init matcher
    auto mnh = ros::NodeHandle{pnh_, "match"};
    MatcherParams mp;
    mp.half_rows = mnh.param<int>("half_rows", 2);
    mp.min_dist = mnh.param<double>("min_dist", 2.0);
    mp.range_ratio = mnh.param<double>("range_ratio", 0.1);
    matcher_ = ProjMatcher(grid_.size(), mp);
    ROS_INFO_STREAM(matcher_);

    /// Init pose
    t_ = cinfo_msg.header.stamp.toSec();
    ROS_INFO_STREAM("First time: " << t_);

    /// Init tf (for now assume pano does not move)
    geometry_msgs::TransformStamped tf_o_p;
    tf_o_p.header.frame_id = odom_frame_;
    tf_o_p.header.stamp = cinfo_msg.header.stamp;
    tf_o_p.child_frame_id = pano_frame_;
    tf_o_p.transform.rotation.w = 1.0;
    tf_broadcaster_.sendTransform(tf_o_p);

    init_ = true;
  }

  void Preprocess(const LidarScan& scan) {
    int npoints = 0;
    {  /// Add scan to sweep
      auto _ = tm_.Scoped("Sweep.AddScan");
      npoints = sweep_.AddScan(scan);
    }
    ROS_INFO_STREAM("Num scan points: " << npoints);

    int ncells = 0;
    int ncells2 = 0;
    {  /// Reduce scan to grid
      auto _ = tm_.Scoped("Grid.Reduce");
      ncells = grid_.Reduce(scan, tbb_);
    }
    {  /// Filter grid
      auto _ = tm_.Scoped("Grid.Filter");
      ncells2 = grid_.Filter();
    }
    ROS_INFO_STREAM("Num cells: " << ncells);
    ROS_INFO_STREAM("Num cells after filter: " << ncells2);

    if (vis_) {
      cv::Mat sweep_disp;
      cv::extractChannel(sweep_.xyzr, sweep_disp, 3);
      Imshow("sweep", ApplyCmap(sweep_disp, 1 / 32.0, cv::COLORMAP_PINK, 0));
      Imshow("score", ApplyCmap(grid_.score, 5, cv::COLORMAP_VIRIDIS, 255));
      Imshow("mask", (1 - grid_.mask) * 255);
    }
  }

  bool Register(const std_msgs::Header& header) {
    {  /// Query grid poses
      auto _ = tm_.Scoped("Traj.GridPose");
      for (int i = 0; i < grid_.tf_p_s.size(); ++i) {
        grid_.tf_p_s[i] = T_p_s_.cast<float>();
      }
    }

    int num_matches = 0;
    {  /// Match Features
      auto _ = tm_.Scoped("Matcher.Match");
      num_matches = matcher_.Match(sweep_, grid_, pano_, tbb_);
    }

    ROS_INFO_STREAM("Num matches: " << num_matches);

    if (vis_) {
      // display good match
      Imshow("match",
             ApplyCmap(DrawMatches(grid_, matcher_.matches),
                       1.0 / matcher_.pano_win_size.area(),
                       cv::COLORMAP_VIRIDIS));
    }

    cs::Solver::Summary summary;
    std::unique_ptr<ceres::LocalParameterization> local_params =
        std::make_unique<LocalParamSE3>();
    // std::unique_ptr<ceres::LossFunction> loss =
    //   std::make_unique<cs::HuberLoss>(3);
    cs::Problem::Options problem_opt;
    problem_opt.loss_function_ownership = cs::DO_NOT_TAKE_OWNERSHIP;
    problem_opt.local_parameterization_ownership = cs::DO_NOT_TAKE_OWNERSHIP;
    cs::Problem problem{problem_opt};

    problem.AddParameterBlock(
        T_p_s_.data(), Sophus::SE3d::num_parameters, local_params.get());

    {  /// Build problem
      auto _ = tm_.Scoped("Icp.Build");
      for (const auto& match : matcher_.matches) {
        if (!match.ok()) continue;
        auto cost = new cs::AutoDiffCostFunction<GicpFactor,
                                                 IcpFactorBase::kNumResiduals,
                                                 IcpFactorBase::kNumParams>(
            new GicpFactor(match));
        problem.AddResidualBlock(cost, nullptr, T_p_s_.data());
      }
    }

    cs::Solver::Options solver_opt;
    solver_opt.linear_solver_type = ceres::DENSE_QR;
    solver_opt.max_num_iterations = 5;
    solver_opt.num_threads = tbb_ ? 4 : 1;
    solver_opt.minimizer_progress_to_stdout = false;
    {
      auto _ = tm_.Scoped("Icp.Solve");
      cs::Solve(solver_opt, &problem, &summary);
    }
    ROS_INFO_STREAM("Pose: \n" << T_p_s_.matrix3x4());
    ROS_INFO_STREAM(summary.BriefReport());
    return summary.IsSolutionUsable();
  }

  void Postprocess() {
    {  /// Query pose again
      auto _ = tm_.Scoped("Traj.SweepPose");
      for (int i = 0; i < sweep_.tf_p_s.size(); ++i) {
        sweep_.tf_p_s[i] = T_p_s_.cast<float>();
      }
    }

    int num_added = 0;
    {  /// Add sweep to pano
      auto _ = tm_.Scoped("Pano.AddSweep");
      num_added = pano_.AddSweep(sweep_, tbb_);
    }
    ROS_INFO_STREAM("Num added: " << num_added
                                  << ", sweep total: " << sweep_.xyzr.total());

    // int num_rendered = 0;
    // {  /// Render pano at new location
    //   auto _ = tm_.Scoped("Pano/Render");
    //   num_rendered = pano_.Render(tbb_);
    //  }
    //  ROS_INFO_STREAM("Num rendered: " << num_rendered
    //                                   << ", pano total: " <<
    //                                   pano_.total());

    if (vis_) {
      Imshow("pano", ApplyCmap(pano_.dbuf_, 1 / Pixel::kScale / 30.0));
      // Imshow("pano2", ApplyCmap(pano_.dbuf2_, 1 / Pixel::kScale / 30.0));
    }

    // Reset matcher
    matcher_.Reset();
  }

  void CameraCb(const sensor_msgs::ImageConstPtr& image_msg,
                const sensor_msgs::CameraInfoConstPtr& cinfo_msg) {
    if (lidar_frame_.empty()) {
      lidar_frame_ = image_msg->header.frame_id;
      ROS_INFO_STREAM("Lidar frame: " << lidar_frame_);
    }

    if (!init_) {
      Init(*cinfo_msg);
    }

    // Wait for the start of the sweep
    static bool wait_for_scan0{true};
    if (wait_for_scan0) {
      if (cinfo_msg->binning_x == 0) {
        ROS_INFO_STREAM("+++ Start of sweep");
        wait_for_scan0 = false;
      } else {
        ROS_WARN_STREAM("Waiting for the first scan, current "
                        << cinfo_msg->binning_x);
        return;
      }
    }

    const auto scan = Msg2Scan(*image_msg, *cinfo_msg);
    Preprocess(scan);

    /// Check if pano has data, if true then perform match
    if (pano_.num_sweeps() > 0) {
      // Make a copy
      Sophus::SE3d T_p_s = T_p_s_;
      const auto good = Register(image_msg->header);

      if (good) {
        /// Transform from sweep to pano
        geometry_msgs::TransformStamped tf_p_s;
        SE3d2Transform(T_p_s_, tf_p_s.transform);
        tf_p_s.header.frame_id = pano_frame_;
        tf_p_s.header.stamp = cinfo_msg->header.stamp;
        tf_p_s.child_frame_id = cinfo_msg->header.frame_id;
        tf_broadcaster_.sendTransform(tf_p_s);
      } else {
        T_p_s_ = T_p_s;  // set to previous value;
        ROS_ERROR("Optimization failed");
      }

      /// Transform from pano to odom
      geometry_msgs::TransformStamped tf_o_p;
      tf_o_p.header.frame_id = odom_frame_;
      tf_o_p.header.stamp = cinfo_msg->header.stamp;
      tf_o_p.child_frame_id = pano_frame_;
      tf_o_p.transform.rotation.w = 1.0;
      tf_broadcaster_.sendTransform(tf_o_p);

      marray_.markers.clear();
      std_msgs::Header marker_header;
      marker_header.frame_id = pano_frame_;
      marker_header.stamp = cinfo_msg->header.stamp;
      Match2Markers(matcher_.matches, marker_header, marray_.markers);
    }

    /// Got a full sweep
    if (cinfo_msg->binning_x + 1 == cinfo_msg->binning_y) {
      Postprocess();

      // Reset everything
      matcher_.Reset();

      static Cloud pano_cloud;
      std_msgs::Header pano_header;
      pano_header.frame_id = pano_frame_;
      pano_header.stamp = cinfo_msg->header.stamp;
      Pano2Cloud(pano_, pano_header, pano_cloud);
      pub_pano_.publish(pano_cloud);
      ROS_INFO_STREAM("--- End of sweep");
    }

    pub_marray_.publish(marray_);
    ROS_DEBUG_STREAM_THROTTLE(2, tm_.ReportAll());
  }
};

}  // namespace sv

int main(int argc, char** argv) {
  ros::init(argc, argv, "llol_node");
  sv::OdomNode node(ros::NodeHandle("~"));
  ros::spin();
}