/*
 * ElevationMapping.cpp
 *
 *  Created on: Nov 12, 2013
 *      Author: Péter Fankhauser
 *   Institute: ETH Zurich, ANYbotics
 */

#include <cmath>
#include <string>

#include <grid_map_msgs/GridMap.h>
#include <pcl/PCLPointCloud2.h>
#include <pcl/conversions.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/point_cloud.h>
#include <pcl_conversions/pcl_conversions.h>
#include <boost/bind.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <grid_map_ros/grid_map_ros.hpp>
#include <kindr/Core>
#include <kindr_ros/kindr_ros.hpp>

#include "elevation_mapping/ElevationMap.hpp"
#include "elevation_mapping/ElevationMapping.hpp"
#include "elevation_mapping/PointXYZRGBConfidenceRatio.hpp"
#include "elevation_mapping/sensor_processors/LaserSensorProcessor.hpp"
#include "elevation_mapping/sensor_processors/PerfectSensorProcessor.hpp"
#include "elevation_mapping/sensor_processors/StereoSensorProcessor.hpp"
#include "elevation_mapping/sensor_processors/StructuredLightSensorProcessor.hpp"

namespace elevation_mapping {

ElevationMapping::ElevationMapping(ros::NodeHandle& nodeHandle)
    : nodeHandle_(nodeHandle),
      inputSources_(nodeHandle_),
      robotPoseCacheSize_(200),
      map_(nodeHandle),
      robotMotionMapUpdater_(nodeHandle),
      ignoreRobotMotionUpdates_(false),
      updatesEnabled_(true),
      isContinuouslyFusing_(false),
      receivedFirstMatchingPointcloudAndPose_(false),
      initializeElevationMap_(false),
      initializationMethod_(0),
      lengthInXInitSubmap_(1.2),
      lengthInYInitSubmap_(1.8),
      marginInitSubmap_(0.3),
      initSubmapHeightOffset_(0.0) {
  ROS_INFO("Elevation mapping node started.");

  readParameters();
  setupSubscribers();

  mapUpdateTimer_ = nodeHandle_.createTimer(maxNoUpdateDuration_, &ElevationMapping::mapUpdateTimerCallback, this, true, false);

  // Multi-threading for fusion.
  ros::AdvertiseServiceOptions advertiseServiceOptionsForTriggerFusion = ros::AdvertiseServiceOptions::create<std_srvs::Empty>(
      "trigger_fusion", boost::bind(&ElevationMapping::fuseEntireMap, this, _1, _2), ros::VoidConstPtr(), &fusionServiceQueue_);
  fusionTriggerService_ = nodeHandle_.advertiseService(advertiseServiceOptionsForTriggerFusion);

  ros::AdvertiseServiceOptions advertiseServiceOptionsForGetFusedSubmap = ros::AdvertiseServiceOptions::create<grid_map_msgs::GetGridMap>(
      "get_submap", boost::bind(&ElevationMapping::getFusedSubmap, this, _1, _2), ros::VoidConstPtr(), &fusionServiceQueue_);
  fusedSubmapService_ = nodeHandle_.advertiseService(advertiseServiceOptionsForGetFusedSubmap);

  ros::AdvertiseServiceOptions advertiseServiceOptionsForGetRawSubmap = ros::AdvertiseServiceOptions::create<grid_map_msgs::GetGridMap>(
      "get_raw_submap", boost::bind(&ElevationMapping::getRawSubmap, this, _1, _2), ros::VoidConstPtr(), &fusionServiceQueue_);
  rawSubmapService_ = nodeHandle_.advertiseService(advertiseServiceOptionsForGetRawSubmap);

  if (!fusedMapPublishTimerDuration_.isZero()) {
    ros::TimerOptions timerOptions =
        ros::TimerOptions(fusedMapPublishTimerDuration_, boost::bind(&ElevationMapping::publishFusedMapCallback, this, _1),
                          &fusionServiceQueue_, false, false);
    fusedMapPublishTimer_ = nodeHandle_.createTimer(timerOptions);
  }

  // Multi-threading for visibility cleanup. Visibility clean-up does not help when continuous clean-up is enabled.
  if (map_.enableVisibilityCleanup_ && !visibilityCleanupTimerDuration_.isZero() && !map_.enableContinuousCleanup_) {
    ros::TimerOptions timerOptions =
        ros::TimerOptions(visibilityCleanupTimerDuration_, boost::bind(&ElevationMapping::visibilityCleanupCallback, this, _1),
                          &visibilityCleanupQueue_, false, false);
    visibilityCleanupTimer_ = nodeHandle_.createTimer(timerOptions);
  }

  filteredPointCloudPublisher_ = nodeHandle_.advertise<sensor_msgs::PointCloud2>("filered_points", 1);

  clearMapService_ = nodeHandle_.advertiseService("clear_map", &ElevationMapping::clearMap, this);
  enableUpdatesService_ = nodeHandle_.advertiseService("enable_updates", &ElevationMapping::enableUpdates, this);
  disableUpdatesService_ = nodeHandle_.advertiseService("disable_updates", &ElevationMapping::disableUpdates, this);
  maskedReplaceService_ = nodeHandle_.advertiseService("masked_replace", &ElevationMapping::maskedReplace, this);
  saveMapService_ = nodeHandle_.advertiseService("save_map", &ElevationMapping::saveMap, this);
  loadMapService_ = nodeHandle_.advertiseService("load_map", &ElevationMapping::loadMap, this);

  initialize();
}

void ElevationMapping::setupSubscribers() {  // Handle deprecated point_cloud_topic and input_sources configuration.
  const bool configuredInputSources = inputSources_.configureFromRos("input_sources");
  const bool hasDeprecatedPointcloudTopic = nodeHandle_.hasParam("point_cloud_topic");
  if (hasDeprecatedPointcloudTopic) {
    ROS_WARN("Parameter 'point_cloud_topic' is deprecated, please use 'input_sources' instead.");
  }
  if (!configuredInputSources && hasDeprecatedPointcloudTopic) {
    pointCloudSubscriber_ = nodeHandle_.subscribe<sensor_msgs::PointCloud2>(
        pointCloudTopic_, 1,
        std::bind(&ElevationMapping::pointCloudCallback, this, std::placeholders::_1, true, std::ref(sensorProcessor_)));
  }
  if (configuredInputSources) {
    inputSources_.registerCallbacks(*this, make_pair("pointcloud", &ElevationMapping::pointCloudCallback));
  }

  if (!robotPoseTopic_.empty()) {
    robotPoseSubscriber_.subscribe(nodeHandle_, robotPoseTopic_, 1);
    robotPoseCache_.connectInput(robotPoseSubscriber_);
    robotPoseCache_.setCacheSize(robotPoseCacheSize_);
  } else {
    ignoreRobotMotionUpdates_ = true;
  }
}

ElevationMapping::~ElevationMapping() {
  // Shutdown all services.

  {  // Fusion Service Queue
    rawSubmapService_.shutdown();
    fusionTriggerService_.shutdown();
    fusedSubmapService_.shutdown();
    fusedMapPublishTimer_.stop();

    fusionServiceQueue_.disable();
    fusionServiceQueue_.clear();
  }

  {  // Visibility cleanup queue
    visibilityCleanupTimer_.stop();

    visibilityCleanupQueue_.disable();
    visibilityCleanupQueue_.clear();
  }

  nodeHandle_.shutdown();

  // Join threads.
  if (fusionServiceThread_.joinable()) {
    fusionServiceThread_.join();
  }
  if (visibilityCleanupThread_.joinable()) {
    visibilityCleanupThread_.join();
  }
}

bool ElevationMapping::readParameters() {
  // ElevationMapping parameters.
  nodeHandle_.param("point_cloud_topic", pointCloudTopic_, std::string("/points"));
  nodeHandle_.param("robot_pose_with_covariance_topic", robotPoseTopic_, std::string("/pose"));
  nodeHandle_.param("track_point_frame_id", trackPointFrameId_, std::string("/robot"));
  nodeHandle_.param("track_point_x", trackPoint_.x(), 0.0);
  nodeHandle_.param("track_point_y", trackPoint_.y(), 0.0);
  nodeHandle_.param("track_point_z", trackPoint_.z(), 0.0);

  nodeHandle_.param("vgf_leaf_horizontal", leafSizeH_, 0.1);
  nodeHandle_.param("vgf_leaf_vertical", leafSizeV_, 0.05);

  nodeHandle_.param("robot_pose_cache_size", robotPoseCacheSize_, 200);
  ROS_ASSERT(robotPoseCacheSize_ >= 0);

  double minUpdateRate;
  nodeHandle_.param("min_update_rate", minUpdateRate, 2.0);
  if (minUpdateRate == 0.0) {
    maxNoUpdateDuration_.fromSec(0.0);
    ROS_WARN("Rate for publishing the map is zero.");
  } else {
    maxNoUpdateDuration_.fromSec(1.0 / minUpdateRate);
  }
  ROS_ASSERT(!maxNoUpdateDuration_.isZero());

  double timeTolerance;
  nodeHandle_.param("time_tolerance", timeTolerance, 0.0);
  timeTolerance_.fromSec(timeTolerance);

  double fusedMapPublishingRate;
  nodeHandle_.param("fused_map_publishing_rate", fusedMapPublishingRate, 1.0);
  if (fusedMapPublishingRate == 0.0) {
    fusedMapPublishTimerDuration_.fromSec(0.0);
    ROS_WARN(
        "Rate for publishing the fused map is zero. The fused elevation map will not be published unless the service `triggerFusion` is "
        "called.");
  } else if (std::isinf(fusedMapPublishingRate)) {
    isContinuouslyFusing_ = true;
    fusedMapPublishTimerDuration_.fromSec(0.0);
  } else {
    fusedMapPublishTimerDuration_.fromSec(1.0 / fusedMapPublishingRate);
  }

  double visibilityCleanupRate;
  nodeHandle_.param("visibility_cleanup_rate", visibilityCleanupRate, 1.0);
  if (visibilityCleanupRate == 0.0) {
    visibilityCleanupTimerDuration_.fromSec(0.0);
    ROS_WARN("Rate for visibility cleanup is zero and therefore disabled.");
  } else {
    visibilityCleanupTimerDuration_.fromSec(1.0 / visibilityCleanupRate);
    map_.visibilityCleanupDuration_ = 1.0 / visibilityCleanupRate;
  }

  // ElevationMap parameters. TODO Move this to the elevation map class.
  nodeHandle_.param("map_frame_id", mapFrameId_, std::string("/map"));
  map_.setFrameId(mapFrameId_);

  grid_map::Length length;
  grid_map::Position position;
  double resolution;
  nodeHandle_.param("length_in_x", length(0), 1.5);
  nodeHandle_.param("length_in_y", length(1), 1.5);
  nodeHandle_.param("position_x", position.x(), 0.0);
  nodeHandle_.param("position_y", position.y(), 0.0);
  nodeHandle_.param("resolution", resolution, 0.01);
  map_.setGeometry(length, resolution, position);

  nodeHandle_.param("min_variance", map_.minVariance_, pow(0.003, 2));
  nodeHandle_.param("max_variance", map_.maxVariance_, pow(0.03, 2));
  nodeHandle_.param("mahalanobis_distance_threshold", map_.mahalanobisDistanceThreshold_, 2.5);
  nodeHandle_.param("multi_height_noise", map_.multiHeightNoise_, pow(0.003, 2));
  nodeHandle_.param("min_horizontal_variance", map_.minHorizontalVariance_, pow(resolution / 2.0, 2));  // two-sigma
  nodeHandle_.param("max_horizontal_variance", map_.maxHorizontalVariance_, 0.5);
  nodeHandle_.param("underlying_map_topic", map_.underlyingMapTopic_, std::string());
  nodeHandle_.param("enable_visibility_cleanup", map_.enableVisibilityCleanup_, true);
  nodeHandle_.param("enable_continuous_cleanup", map_.enableContinuousCleanup_, false);
  nodeHandle_.param("scanning_duration", map_.scanningDuration_, 1.0);
  nodeHandle_.param("masked_replace_service_mask_layer_name", maskedReplaceServiceMaskLayerName_, std::string("mask"));

  // Settings for initializing elevation map
  nodeHandle_.param("initialize_elevation_map", initializeElevationMap_, false);
  nodeHandle_.param("initialization_method", initializationMethod_, 0);
  nodeHandle_.param("length_in_x_init_submap", lengthInXInitSubmap_, 1.2);
  nodeHandle_.param("length_in_y_init_submap", lengthInYInitSubmap_, 1.8);
  nodeHandle_.param("margin_init_submap", marginInitSubmap_, 0.3);
  nodeHandle_.param("init_submap_height_offset", initSubmapHeightOffset_, 0.0);
  nodeHandle_.param("target_frame_init_submap", targetFrameInitSubmap_, std::string("/footprint"));

  // SensorProcessor parameters. Deprecated, use the sensorProcessor from within input sources instead!
  std::string sensorType;
  nodeHandle_.param("sensor_processor/type", sensorType, std::string("structured_light"));

  SensorProcessorBase::GeneralParameters generalSensorProcessorConfig{nodeHandle_.param("robot_base_frame_id", std::string("/robot")),
                                                                      mapFrameId_};
  if (sensorType == "structured_light") {
    sensorProcessor_.reset(new StructuredLightSensorProcessor(nodeHandle_, generalSensorProcessorConfig));
  } else if (sensorType == "stereo") {
    sensorProcessor_.reset(new StereoSensorProcessor(nodeHandle_, generalSensorProcessorConfig));
  } else if (sensorType == "laser") {
    sensorProcessor_.reset(new LaserSensorProcessor(nodeHandle_, generalSensorProcessorConfig));
  } else if (sensorType == "perfect") {
    sensorProcessor_.reset(new PerfectSensorProcessor(nodeHandle_, generalSensorProcessorConfig));
  } else {
    ROS_ERROR("The sensor type %s is not available.", sensorType.c_str());
  }
  if (!sensorProcessor_->readParameters()) {
    return false;
  }
  if (!robotMotionMapUpdater_.readParameters()) {
    return false;
  }

  return true;
}

bool ElevationMapping::initialize() {
  ROS_INFO("Elevation mapping node initializing ... ");
  fusionServiceThread_ = boost::thread(boost::bind(&ElevationMapping::runFusionServiceThread, this));
  ros::Duration(1.0).sleep();  // Need this to get the TF caches fill up.
  resetMapUpdateTimer();
  fusedMapPublishTimer_.start();
  visibilityCleanupThread_ = boost::thread(boost::bind(&ElevationMapping::visibilityCleanupThread, this));
  visibilityCleanupTimer_.start();
  initializeElevationMap();
  ROS_INFO("Done initializing.");
  return true;
}

void ElevationMapping::runFusionServiceThread() {
  ros::Rate loopRate(20);

  while (nodeHandle_.ok()) {
    fusionServiceQueue_.callAvailable();

    // Sleep until the next execution.
    loopRate.sleep();
  }
}

void ElevationMapping::visibilityCleanupThread() {
  ros::Rate loopRate(20);

  while (nodeHandle_.ok()) {
    visibilityCleanupQueue_.callAvailable();

    // Sleep until the next execution.
    loopRate.sleep();
  }
}

void ElevationMapping::pointCloudCallback(const sensor_msgs::PointCloud2ConstPtr& pointCloudMsg, bool publishPointCloud,
                                          const SensorProcessorBase::Ptr& sensorProcessor_) {
  ROS_DEBUG("Processing data from: %s", pointCloudMsg->header.frame_id.c_str());
  if (!updatesEnabled_) {
    ROS_WARN_THROTTLE(10, "Updating of elevation map is disabled. (Warning message is throttled, 10s.)");
    if (publishPointCloud) {
      map_.setTimestamp(ros::Time::now());
      map_.publishRawElevationMap();
    }
    return;
  }

  // Check if point cloud has corresponding robot pose at the beginning
  if (!receivedFirstMatchingPointcloudAndPose_) {
    const double oldestPoseTime = robotPoseCache_.getOldestTime().toSec();
    const double currentPointCloudTime = pointCloudMsg->header.stamp.toSec();

    if (currentPointCloudTime < oldestPoseTime) {
      ROS_WARN_THROTTLE(5, "No corresponding point cloud and pose are found. Waiting for first match. (Warning message is throttled, 5s.)");
      return;
    } else {
      ROS_INFO("First corresponding point cloud and pose found, elevation mapping started. ");
      receivedFirstMatchingPointcloudAndPose_ = true;
    }
  }

  stopMapUpdateTimer();

  // Convert the sensor_msgs/PointCloud2 data to pcl/PointCloud.
  // TODO(max): Double check with http://wiki.ros.org/hydro/Migration
  pcl::PCLPointCloud2::Ptr pcl_pc(new pcl::PCLPointCloud2 ());
  pcl_conversions::toPCL(*pointCloudMsg, *pcl_pc);

  // Dowmsampling
  pcl::PCLPointCloud2::Ptr cloud_filtered (new pcl::PCLPointCloud2 ());
  std::cerr << "PointCloud before filtering: " << pcl_pc->width * pcl_pc->height
  << " data points (" << pcl::getFieldsList (*pcl_pc) << ")." << std::endl;

  // Create the filtering object
  pcl::VoxelGrid<pcl::PCLPointCloud2> sor;
  sor.setInputCloud(pcl_pc);
  sor.setLeafSize(leafSizeH_, leafSizeH_, leafSizeV_);
  sor.filter(*cloud_filtered);

  // Convert to ROS data type
  sensor_msgs::PointCloud2 filteredPointCloud;
  pcl_conversions::fromPCL(*cloud_filtered, filteredPointCloud);

  std::cerr << "PointCloud after filtering: " << cloud_filtered->width * cloud_filtered->height
  << " data points (" << pcl::getFieldsList (*cloud_filtered) << ")." << std::endl;

  PointCloudType::Ptr pointCloud(new PointCloudType);
  pcl::fromPCLPointCloud2(*cloud_filtered, *pointCloud);
  lastPointCloudUpdateTime_.fromNSec(1000 * pointCloud->header.stamp);

  ROS_DEBUG("ElevationMap received a point cloud (%i points) for elevation mapping.", static_cast<int>(pointCloud->size()));

  // Get robot pose covariance matrix at timestamp of point cloud.
  Eigen::Matrix<double, 6, 6> robotPoseCovariance;
  robotPoseCovariance.setZero();
  if (!ignoreRobotMotionUpdates_) {
    boost::shared_ptr<geometry_msgs::PoseWithCovarianceStamped const> poseMessage =
        robotPoseCache_.getElemBeforeTime(lastPointCloudUpdateTime_);
    if (!poseMessage) {
      // Tell the user that either for the timestamp no pose is available or that the buffer is possibly empty
      if (robotPoseCache_.getOldestTime().toSec() > lastPointCloudUpdateTime_.toSec()) {
        ROS_ERROR("The oldest pose available is at %f, requested pose at %f", robotPoseCache_.getOldestTime().toSec(),
                  lastPointCloudUpdateTime_.toSec());
      } else {
        ROS_ERROR("Could not get pose information from robot for time %f. Buffer empty?", lastPointCloudUpdateTime_.toSec());
      }
      return;
    }
    robotPoseCovariance = Eigen::Map<const Eigen::MatrixXd>(poseMessage->pose.covariance.data(), 6, 6);
  }

  // Process point cloud.
  PointCloudType::Ptr pointCloudProcessed(new PointCloudType);
  Eigen::VectorXf measurementVariances;
  if (!sensorProcessor_->process(pointCloud, robotPoseCovariance, pointCloudProcessed, measurementVariances,
                                 pointCloudMsg->header.frame_id)) {
    if (!sensorProcessor_->isTfAvailableInBuffer()) {
      ROS_INFO_THROTTLE(10, "Waiting for tf transformation to be available. (Message is throttled, 10s.)");
      return;
    }
    ROS_ERROR("Point cloud could not be processed.");
    resetMapUpdateTimer();
    return;
  }

  boost::recursive_mutex::scoped_lock scopedLock(map_.getRawDataMutex());

  // Update map location.
  updateMapLocation();

  // Update map from motion prediction.
  if (!updatePrediction(lastPointCloudUpdateTime_)) {
    ROS_ERROR("Updating process noise failed.");
    resetMapUpdateTimer();
    return;
  }

  // Clear the map if continuous clean-up was enabled.
  if (map_.enableContinuousCleanup_) {
    ROS_DEBUG("Clearing elevation map before adding new point cloud.");
    map_.clear();
  }

  // Add point cloud to elevation map.
  if (!map_.add(pointCloudProcessed, measurementVariances, lastPointCloudUpdateTime_,
                Eigen::Affine3d(sensorProcessor_->transformationSensorToMap_))) {
    ROS_ERROR("Adding point cloud to elevation map failed.");
    resetMapUpdateTimer();
    return;
  }

  if (publishPointCloud) {

    if(filteredPointCloudPublisher_.getNumSubscribers() > 0){
        filteredPointCloudPublisher_.publish(filteredPointCloud);
    }
    // Publish elevation map.
    map_.publishRawElevationMap();
    if (isContinuouslyFusing_ && map_.hasFusedMapSubscribers()) {
      map_.fuseAll();
      map_.publishFusedElevationMap();
    }
  }

  resetMapUpdateTimer();
}

void ElevationMapping::mapUpdateTimerCallback(const ros::TimerEvent&) {
  if (!updatesEnabled_) {
    ROS_WARN_THROTTLE(10, "Updating of elevation map is disabled. (Warning message is throttled, 10s.)");
    map_.setTimestamp(ros::Time::now());
    map_.publishRawElevationMap();
    return;
  }

  ros::Time time = ros::Time::now();
  if ((lastPointCloudUpdateTime_ - time) <= maxNoUpdateDuration_) {  // there were updates from sensordata, no need to force an update.
    return;
  }
  ROS_WARN_THROTTLE(5, "Elevation map is updated without data from the sensor. (Warning message is throttled, 5s.)");

  boost::recursive_mutex::scoped_lock scopedLock(map_.getRawDataMutex());

  stopMapUpdateTimer();

  // Update map from motion prediction.
  if (!updatePrediction(time)) {
    ROS_ERROR("Updating process noise failed.");
    resetMapUpdateTimer();
    return;
  }

  // Publish elevation map.
  map_.publishRawElevationMap();
  if (isContinuouslyFusing_ && map_.hasFusedMapSubscribers()) {
    map_.fuseAll();
    map_.publishFusedElevationMap();
  }

  resetMapUpdateTimer();
}

void ElevationMapping::publishFusedMapCallback(const ros::TimerEvent&) {
  if (!map_.hasFusedMapSubscribers()) {
    return;
  }
  ROS_DEBUG("Elevation map is fused and published from timer.");
  boost::recursive_mutex::scoped_lock scopedLock(map_.getFusedDataMutex());
  map_.fuseAll();
  map_.publishFusedElevationMap();
}

void ElevationMapping::visibilityCleanupCallback(const ros::TimerEvent&) {
  ROS_DEBUG("Elevation map is running visibility cleanup.");
  // Copy constructors for thread-safety.
  map_.visibilityCleanup(ros::Time(lastPointCloudUpdateTime_));
}

bool ElevationMapping::fuseEntireMap(std_srvs::Empty::Request&, std_srvs::Empty::Response&) {
  boost::recursive_mutex::scoped_lock scopedLock(map_.getFusedDataMutex());
  map_.fuseAll();
  map_.publishFusedElevationMap();
  return true;
}

bool ElevationMapping::updatePrediction(const ros::Time& time) {
  if (ignoreRobotMotionUpdates_) {
    return true;
  }

  ROS_DEBUG("Updating map with latest prediction from time %f.", robotPoseCache_.getLatestTime().toSec());

  if (time + timeTolerance_ < map_.getTimeOfLastUpdate()) {
    ROS_ERROR("Requested update with time stamp %f, but time of last update was %f.", time.toSec(), map_.getTimeOfLastUpdate().toSec());
    return false;
  } else if (time < map_.getTimeOfLastUpdate()) {
    ROS_DEBUG("Requested update with time stamp %f, but time of last update was %f. Ignoring update.", time.toSec(),
              map_.getTimeOfLastUpdate().toSec());
    return true;
  }

  // Get robot pose at requested time.
  boost::shared_ptr<geometry_msgs::PoseWithCovarianceStamped const> poseMessage = robotPoseCache_.getElemBeforeTime(time);
  if (!poseMessage) {
    // Tell the user that either for the timestamp no pose is available or that the buffer is possibly empty
    if (robotPoseCache_.getOldestTime().toSec() > lastPointCloudUpdateTime_.toSec()) {
      ROS_ERROR("The oldest pose available is at %f, requested pose at %f", robotPoseCache_.getOldestTime().toSec(),
                lastPointCloudUpdateTime_.toSec());
    } else {
      ROS_ERROR("Could not get pose information from robot for time %f. Buffer empty?", lastPointCloudUpdateTime_.toSec());
    }
    return false;
  }

  kindr::HomTransformQuatD robotPose;
  kindr_ros::convertFromRosGeometryMsg(poseMessage->pose.pose, robotPose);
  // Covariance is stored in row-major in ROS: http://docs.ros.org/api/geometry_msgs/html/msg/PoseWithCovariance.html
  Eigen::Matrix<double, 6, 6> robotPoseCovariance =
      Eigen::Map<const Eigen::Matrix<double, 6, 6, Eigen::RowMajor>>(poseMessage->pose.covariance.data(), 6, 6);

  // Compute map variance update from motion prediction.
  robotMotionMapUpdater_.update(map_, robotPose, robotPoseCovariance, time);

  return true;
}

bool ElevationMapping::updateMapLocation() {
  ROS_DEBUG("Elevation map is checked for relocalization.");

  geometry_msgs::PointStamped trackPoint;
  trackPoint.header.frame_id = trackPointFrameId_;
  trackPoint.header.stamp = ros::Time(0);
  kindr_ros::convertToRosGeometryMsg(trackPoint_, trackPoint.point);
  geometry_msgs::PointStamped trackPointTransformed;

  try {
    transformListener_.transformPoint(map_.getFrameId(), trackPoint, trackPointTransformed);
  } catch (tf::TransformException& ex) {
    ROS_ERROR("%s", ex.what());
    return false;
  }

  kindr::Position3D position3d;
  kindr_ros::convertFromRosGeometryMsg(trackPointTransformed.point, position3d);
  grid_map::Position position = position3d.vector().head(2);
  map_.move(position);
  return true;
}

bool ElevationMapping::getFusedSubmap(grid_map_msgs::GetGridMap::Request& request, grid_map_msgs::GetGridMap::Response& response) {
  grid_map::Position requestedSubmapPosition(request.position_x, request.position_y);
  grid_map::Length requestedSubmapLength(request.length_x, request.length_y);
  ROS_DEBUG("Elevation submap request: Position x=%f, y=%f, Length x=%f, y=%f.", requestedSubmapPosition.x(), requestedSubmapPosition.y(),
            requestedSubmapLength(0), requestedSubmapLength(1));
  boost::recursive_mutex::scoped_lock scopedLock(map_.getFusedDataMutex());
  map_.fuseArea(requestedSubmapPosition, requestedSubmapLength);

  bool isSuccess;
  grid_map::Index index;
  grid_map::GridMap subMap = map_.getFusedGridMap().getSubmap(requestedSubmapPosition, requestedSubmapLength, index, isSuccess);
  scopedLock.unlock();

  if (request.layers.empty()) {
    grid_map::GridMapRosConverter::toMessage(subMap, response.map);
  } else {
    std::vector<std::string> layers;
    for (const std::string& layer : request.layers) {
      layers.push_back(layer);
    }
    grid_map::GridMapRosConverter::toMessage(subMap, layers, response.map);
  }

  ROS_DEBUG("Elevation submap responded with timestamp %f.", map_.getTimeOfLastFusion().toSec());
  return isSuccess;
}

bool ElevationMapping::getRawSubmap(grid_map_msgs::GetGridMap::Request& request, grid_map_msgs::GetGridMap::Response& response) {
  grid_map::Position requestedSubmapPosition(request.position_x, request.position_y);
  grid_map::Length requestedSubmapLength(request.length_x, request.length_y);
  ROS_DEBUG("Elevation raw submap request: Position x=%f, y=%f, Length x=%f, y=%f.", requestedSubmapPosition.x(),
            requestedSubmapPosition.y(), requestedSubmapLength(0), requestedSubmapLength(1));
  boost::recursive_mutex::scoped_lock scopedLock(map_.getRawDataMutex());

  bool isSuccess;
  grid_map::Index index;
  grid_map::GridMap subMap = map_.getRawGridMap().getSubmap(requestedSubmapPosition, requestedSubmapLength, index, isSuccess);
  scopedLock.unlock();

  if (request.layers.empty()) {
    grid_map::GridMapRosConverter::toMessage(subMap, response.map);
  } else {
    std::vector<std::string> layers;
    for (const std::string& layer : request.layers) {
      layers.push_back(layer);
    }
    grid_map::GridMapRosConverter::toMessage(subMap, layers, response.map);
  }
  return isSuccess;
}

bool ElevationMapping::disableUpdates(std_srvs::Empty::Request& /*request*/, std_srvs::Empty::Response& /*response*/) {
  ROS_INFO("Disabling updates.");
  updatesEnabled_ = false;
  return true;
}

bool ElevationMapping::enableUpdates(std_srvs::Empty::Request& /*request*/, std_srvs::Empty::Response& /*response*/) {
  ROS_INFO("Enabling updates.");
  updatesEnabled_ = true;
  return true;
}

bool ElevationMapping::initializeElevationMap() {
  if (initializeElevationMap_) {
    if (static_cast<elevation_mapping::InitializationMethods>(initializationMethod_) ==
        elevation_mapping::InitializationMethods::PlanarFloorInitializer) {
      tf::StampedTransform transform;

      // Listen to transform between mapFrameId_ and targetFrameInitSubmap_ and use z value for initialization
      try {
        transformListener_.waitForTransform(mapFrameId_, targetFrameInitSubmap_, ros::Time(0), ros::Duration(5.0));
        transformListener_.lookupTransform(mapFrameId_, targetFrameInitSubmap_, ros::Time(0), transform);
        ROS_DEBUG_STREAM("Initializing with x: " << transform.getOrigin().x() << " y: " << transform.getOrigin().y()
                                                 << " z: " << transform.getOrigin().z());

        const grid_map::Position positionRobot(transform.getOrigin().x(), transform.getOrigin().y());

        // Move map before we apply the height values. This prevents unwanted behavior from intermediate move() calls in
        // updateMapLocation().
        map_.move(positionRobot);

        map_.setRawSubmapHeight(positionRobot, transform.getOrigin().z() + initSubmapHeightOffset_, lengthInXInitSubmap_,
                                lengthInYInitSubmap_, marginInitSubmap_);
        return true;
      } catch (tf::TransformException& ex) {
        ROS_ERROR("%s", ex.what());
        ROS_ERROR("Could not initialize elevation map with constant height.");
        return false;
      }
    }
  }
  return true;
}

bool ElevationMapping::clearMap(std_srvs::Empty::Request& /*request*/, std_srvs::Empty::Response& /*response*/) {
  ROS_INFO("Clearing map...");
  bool success = map_.clear();
  success &= initializeElevationMap();
  ROS_INFO("Map cleared.");

  return success;
}

bool ElevationMapping::maskedReplace(grid_map_msgs::SetGridMap::Request& request, grid_map_msgs::SetGridMap::Response& /*response*/) {
  ROS_INFO("Masked replacing of map.");
  grid_map::GridMap sourceMap;
  grid_map::GridMapRosConverter::fromMessage(request.map, sourceMap);

  // Use the supplied mask or do not use a mask
  grid_map::Matrix mask;
  if (sourceMap.exists(maskedReplaceServiceMaskLayerName_)) {
    mask = sourceMap[maskedReplaceServiceMaskLayerName_];
  } else {
    mask = Eigen::MatrixXf::Ones(sourceMap.getSize()(0), sourceMap.getSize()(1));
  }

  boost::recursive_mutex::scoped_lock scopedLockRawData(map_.getRawDataMutex());

  // Loop over all layers that should be set
  for (auto sourceLayerIterator = sourceMap.getLayers().begin(); sourceLayerIterator != sourceMap.getLayers().end();
       sourceLayerIterator++) {
    // skip "mask" layer
    if (*sourceLayerIterator == maskedReplaceServiceMaskLayerName_) {
      continue;
    }
    grid_map::Matrix& sourceLayer = sourceMap[*sourceLayerIterator];
    // Check if the layer exists in the elevation map
    if (map_.getRawGridMap().exists(*sourceLayerIterator)) {
      grid_map::Matrix& destinationLayer = map_.getRawGridMap()[*sourceLayerIterator];
      for (grid_map::GridMapIterator destinationIterator(map_.getRawGridMap()); !destinationIterator.isPastEnd(); ++destinationIterator) {
        // Use the position to find corresponding indices in source and destination
        const grid_map::Index destinationIndex(*destinationIterator);
        grid_map::Position position;
        map_.getRawGridMap().getPosition(*destinationIterator, position);

        if (!sourceMap.isInside(position)) {
          continue;
        }

        grid_map::Index sourceIndex;
        sourceMap.getIndex(position, sourceIndex);
        // If the mask allows it, set the value from source to destination
        if (!std::isnan(mask(sourceIndex(0), sourceIndex(1)))) {
          destinationLayer(destinationIndex(0), destinationIndex(1)) = sourceLayer(sourceIndex(0), sourceIndex(1));
        }
      }
    } else {
      ROS_ERROR("Masked replace service: Layer %s does not exist!", sourceLayerIterator->c_str());
    }
  }

  return true;
}

bool ElevationMapping::saveMap(grid_map_msgs::ProcessFile::Request& request, grid_map_msgs::ProcessFile::Response& response) {
  ROS_INFO("Saving map to file.");
  boost::recursive_mutex::scoped_lock scopedLock(map_.getFusedDataMutex());
  map_.fuseAll();
  std::string topic = nodeHandle_.getNamespace() + "/elevation_map";
  if (!request.topic_name.empty()) {
    topic = nodeHandle_.getNamespace() + "/" + request.topic_name;
  }
  response.success = static_cast<unsigned char>(grid_map::GridMapRosConverter::saveToBag(map_.getFusedGridMap(), request.file_path, topic));
  response.success = static_cast<unsigned char>(
      (grid_map::GridMapRosConverter::saveToBag(map_.getRawGridMap(), request.file_path + "_raw", topic + "_raw")) &&
      static_cast<bool>(response.success));
  return static_cast<bool>(response.success);
}

bool ElevationMapping::loadMap(grid_map_msgs::ProcessFile::Request& request, grid_map_msgs::ProcessFile::Response& response) {
  ROS_WARN("Loading from bag file.");
  boost::recursive_mutex::scoped_lock scopedLockFused(map_.getFusedDataMutex());
  boost::recursive_mutex::scoped_lock scopedLockRaw(map_.getRawDataMutex());

  std::string topic = nodeHandle_.getNamespace();
  if (!request.topic_name.empty()) {
    topic += "/" + request.topic_name;
  } else {
    topic += "/elevation_map";
  }

  response.success =
      static_cast<unsigned char>(grid_map::GridMapRosConverter::loadFromBag(request.file_path, topic, map_.getFusedGridMap()));
  response.success = static_cast<unsigned char>(
      grid_map::GridMapRosConverter::loadFromBag(request.file_path + "_raw", topic + "_raw", map_.getRawGridMap()) &&
      static_cast<bool>(response.success));

  // Update timestamp for visualization in ROS
  map_.setTimestamp(ros::Time::now());
  map_.publishRawElevationMap();
  return static_cast<bool>(response.success);
}

void ElevationMapping::resetMapUpdateTimer() {
  mapUpdateTimer_.stop();
  ros::Duration periodSinceLastUpdate = ros::Time::now() - map_.getTimeOfLastUpdate();
  if (periodSinceLastUpdate > maxNoUpdateDuration_) {
    periodSinceLastUpdate.fromSec(0.0);
  }
  mapUpdateTimer_.setPeriod(maxNoUpdateDuration_ - periodSinceLastUpdate);
  mapUpdateTimer_.start();
}

void ElevationMapping::stopMapUpdateTimer() {
  mapUpdateTimer_.stop();
}

}  // namespace elevation_mapping
