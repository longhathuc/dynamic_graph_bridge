/*
 * Copyright 2011,
 * Olivier Stasse,
 *
 * CNRS
 *
 */
/* -------------------------------------------------------------------------- */
/* --- INCLUDES ------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

#include <dynamic_graph_bridge/sot_loader.hh>
#include "dynamic_graph_bridge/ros_init.hh"

// POSIX.1-2001
#include <dlfcn.h>

#include <boost/thread/condition.hpp>

boost::condition_variable cond;

using namespace std;
using namespace dynamicgraph::sot;
namespace po = boost::program_options;

struct DataToLog {
  const std::size_t N;
  std::size_t idx;

  std::vector<double> times;

  DataToLog(std::size_t N_) : N(N_), idx(0), times(N, 0) {}

  void record(const double t) {
    times[idx] = t;
    ++idx;
    if (idx == N) idx = 0;
  }

  void save(const char *prefix) {
    std::ostringstream oss;
    oss << prefix << "-times.log";

    std::ofstream aof(oss.str().c_str());
    if (aof.is_open()) {
      for (std::size_t k = 0; k < N; ++k) {
        aof << times[(idx + k) % N] << '\n';
      }
    }
    aof.close();
  }
};

void workThreadLoader(SotLoader *aSotLoader) {
  unsigned period = 1000;  // micro seconds
  if (ros::param::has("/sot_controller/dt")) {
    double periodd;
    ros::param::get("/sot_controller/dt", periodd);
    period = unsigned(1e6 * periodd);
  }
  DataToLog dataToLog(5000);

  while (aSotLoader->isDynamicGraphStopped()) {
    usleep(period);
  }

  struct timeval start, stop;
  ros::NodeHandle nh("/geometric_simu");
  bool paused;
  unsigned long long dt;
  while (!aSotLoader->isDynamicGraphStopped()) {
    nh.param<bool>("paused", paused, false);

    if (!paused) {
      gettimeofday(&start, 0);
      aSotLoader->oneIteration();
      gettimeofday(&stop, 0);

      dt = 1000000 * (stop.tv_sec - start.tv_sec) +
           (stop.tv_usec - start.tv_usec);
      dataToLog.record((double)dt * 1e-6);
    } else
      dt = 0;
    if (period > dt) {
      usleep(period - (unsigned)dt);
    }
  }
  dataToLog.save("/tmp/geometric_simu");
  cond.notify_all();
  ros::waitForShutdown();
}

SotLoader::SotLoader()
    : sensorsIn_(),
      controlValues_(),
      angleEncoder_(),
      angleControl_(),
      forces_(),
      torques_(),
      baseAtt_(),
      accelerometer_(3),
      gyrometer_(3),
      thread_() {
  readSotVectorStateParam();
  initPublication();
}

SotLoader::~SotLoader() {
  dynamic_graph_stopped_ = true;
  thread_.join();
}

void SotLoader::startControlLoop() {
  thread_ = boost::thread(workThreadLoader, this);
}

void SotLoader::initializeRosNode(int argc, char *argv[]) {
  SotLoaderBasic::initializeRosNode(argc, argv);
  // Temporary fix. TODO: where should nbOfJoints_ be initialized from?
  if (ros::param::has("/sot/state_vector_map")) {
    angleEncoder_.resize(nbOfJoints_);
    angleControl_.resize(nbOfJoints_);
  }

  startControlLoop();
}

void SotLoader::fillSensors(map<string, dgs::SensorValues> &sensorsIn) {
  // Update joint values.w
  assert(angleControl_.size() == angleEncoder_.size());

  sensorsIn["joints"].setName("angle");
  for (unsigned int i = 0; i < angleControl_.size(); i++)
    angleEncoder_[i] = angleControl_[i];
  sensorsIn["joints"].setValues(angleEncoder_);
}

void SotLoader::readControl(map<string, dgs::ControlValues> &controlValues) {
  // Update joint values.
  angleControl_ = controlValues["control"].getValues();

  // Debug
  std::map<std::string, dgs::ControlValues>::iterator it =
      controlValues.begin();
  sotDEBUG(30) << "ControlValues to be broadcasted:" << std::endl;
  for (; it != controlValues.end(); it++) {
    sotDEBUG(30) << it->first << ":";
    std::vector<double> ctrlValues_ = it->second.getValues();
    std::vector<double>::iterator it_d = ctrlValues_.begin();
    for (; it_d != ctrlValues_.end(); it_d++) sotDEBUG(30) << *it_d << " ";
    sotDEBUG(30) << std::endl;
  }
  sotDEBUG(30) << "End ControlValues" << std::endl;

  // Check if the size if coherent with the robot description.
  if (angleControl_.size() != (unsigned int)nbOfJoints_) {
    std::cerr << " angleControl_" << angleControl_.size() << " and nbOfJoints"
              << (unsigned int)nbOfJoints_ << " are different !" << std::endl;
    exit(-1);
  }
  // Publish the data.
  joint_state_.header.stamp = ros::Time::now();
  for (int i = 0; i < nbOfJoints_; i++) {
    joint_state_.position[i] = angleControl_[i];
  }
  for (unsigned int i = 0; i < parallel_joints_to_state_vector_.size(); i++) {
    joint_state_.position[i + nbOfJoints_] =
        coefficient_parallel_joints_[i] *
        angleControl_[parallel_joints_to_state_vector_[i]];
  }

  joint_pub_.publish(joint_state_);

  // Publish robot pose
  // get the robot pose values
  std::vector<double> poseValue_ = controlValues["baseff"].getValues();

  freeFlyerPose_.setOrigin(
      tf::Vector3(poseValue_[0], poseValue_[1], poseValue_[2]));
  tf::Quaternion poseQ_(poseValue_[4], poseValue_[5], poseValue_[6],
                        poseValue_[3]);
  freeFlyerPose_.setRotation(poseQ_);
  // Publish
  freeFlyerPublisher_.sendTransform(tf::StampedTransform(
      freeFlyerPose_, ros::Time::now(), "odom", "base_link"));
}

void SotLoader::setup() {
  fillSensors(sensorsIn_);
  sotController_->setupSetSensors(sensorsIn_);
  sotController_->getControl(controlValues_);
  readControl(controlValues_);
}

void SotLoader::oneIteration() {
  fillSensors(sensorsIn_);
  try {
    sotController_->nominalSetSensors(sensorsIn_);
    sotController_->getControl(controlValues_);
  } catch (std::exception &e) {
    throw e;
  }

  readControl(controlValues_);
}
