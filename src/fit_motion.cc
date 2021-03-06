// Auto-calibration and integration of IMU measurements (acceleration +
// gyroscope) using GPS data as coarse-grained refernce points.
//
// Fits IMU calibration parameters by matching IMU integrated travel distances
// to the GPS data. Because some of the drift is not eliminated (not sure
// whether it white noise or something systematic, e.g. gyroscope drift),
// instead of calibrating globally on the whole recorded track, we repeatedly
// calibrate independently using a relatively small sliding window (of e.g. 40
// seconds) with overlaps and averaging the results for every IMU timestamp.
//
// Writes out resulting timestamped velocity magnitudes to a JSON file.

#include <cmath>
#include <cstdlib>
#include <numeric>

#include <Eigen/Geometry>

#include <LBFGS.h>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <json.hpp>

#include <calibration/rotation.hpp>
#include <calibration/velocity.hpp>
#include <geometry/geometry.hpp>
#include <io/json_converters.hpp>
#include <slam/smoothing.hpp>

// Inputs

DEFINE_string(rotations_json, "",
              "JSON file with raw timestamped 3D rotations from the "
              "smartphone gyroscope. Comes from PilotGuru Recorder raw data.");
DEFINE_string(accelerations_json, "",
              "JSON file with raw timestamped 3D accelerations from the "
              "smartphone gyroscope. Comes from PilotGuru Recorder raw data. "
              "Preliminary accelerometer calibration is not necessary, nor "
              "detecting and subtracting the gravity component. This binary "
              "auto-calibrates the data by matching velocity magnitudes with "
              "GPS data.");
DEFINE_string(locations_json, "", "JSON file with GPS locations and derived "
                                  "absolute velocities. Comes from PilotGuru "
                                  "Recorder raw data.");

// Outputs

DEFINE_string(velocities_out_json, "",
              "JSON file to write timestamped absolute "
              "velocities derived from accelerometer "
              "data calibrated using GPS coarse-grained "
              "velocities.");
DEFINE_string(steering_out_json, "",
              "JSON file to write rotations in the inferred horizontal plane, "
              "intended to closely match the 3D rotation component due to "
              "vehicle steering. Horizontal plane is detected via main "
              "principal axis of the raw 3D rotations from the gyro data.");

// Inference parameters.

DEFINE_int64(locations_batch_size, 40,
             "Size of sliding window (in terms of the number of GPS "
             "measurements) to use for calibration. This should not be too "
             "large, as the results become less accurate for long windows "
             "because of accumulating IMU drift.");
DEFINE_int64(locations_shift_step, 5,
             "Step size (in terms of number of GPS measurements) by which to "
             "shift the sliding window for the subsequent calibration runs.");
DEFINE_double(
    optimization_iters, 500,
    "Max number of L-BFGS iterations to use for every calibration run.");
DEFINE_double(post_smoothing_sigma_sec, 0.003,
              "Smoothing Gaussian kernel width (in seconds) for the final "
              "smoothing of the integrated velocities.");

namespace {
template <typename T>
std::vector<T> ReadTimestamp3DData(const std::string &filename,
                                   const std::string &field_name) {
  std::unique_ptr<nlohmann::json> json_root = pilotguru::ReadJsonFile(filename);
  const auto &entries_list = (*json_root)[field_name];
  CHECK(!entries_list.empty());

  std::vector<T> result;
  for (const auto &entry : entries_list) {
    result.push_back({entry[pilotguru::kX], entry[pilotguru::kY],
                      entry[pilotguru::kZ], entry[pilotguru::kTimeUsec]});
  }
  return result;
}

std::vector<pilotguru::TimestampedVelocity>
ReadGpsVelocities(const string &filename) {
  std::unique_ptr<nlohmann::json> locations_json_root =
      pilotguru::ReadJsonFile(filename);
  const auto &locations_json = (*locations_json_root)[pilotguru::kLocations];
  CHECK(!locations_json.empty());
  std::vector<pilotguru::TimestampedVelocity> result;
  for (const auto &location : locations_json) {
    result.push_back(
        {location[pilotguru::kSpeedMS], location[pilotguru::kTimeUsec]});
  }
  return result;
}
}

int main(int argc, char **argv) {
  google::InitGoogleLogging(argv[0]);
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InstallFailureSignalHandler();

  // Sanity checks.
  CHECK(!FLAGS_rotations_json.empty());
  CHECK(!FLAGS_accelerations_json.empty());
  CHECK(!FLAGS_locations_json.empty());
  CHECK(!FLAGS_velocities_out_json.empty());
  CHECK(!FLAGS_steering_out_json.empty());
  CHECK_GT(FLAGS_optimization_iters, 0);
  CHECK_GT(FLAGS_locations_batch_size, 0);
  CHECK_GT(FLAGS_locations_shift_step, 0);
  CHECK_GE(FLAGS_locations_batch_size, FLAGS_locations_shift_step);
  CHECK_GT(FLAGS_post_smoothing_sigma_sec, 0);

  // Read input JSONs.
  const std::vector<pilotguru::TimestampedRotationVelocity> rotations =
      ReadTimestamp3DData<pilotguru::TimestampedRotationVelocity>(
          FLAGS_rotations_json, pilotguru::kRotations);
  const std::vector<pilotguru::TimestampedAcceleration> accelerations =
      ReadTimestamp3DData<pilotguru::TimestampedAcceleration>(
          FLAGS_accelerations_json, pilotguru::kAccelerations);
  const std::vector<pilotguru::TimestampedVelocity> gps_velocities =
      ReadGpsVelocities(FLAGS_locations_json);

  // Infer the main principal rotation axis (assumed to be the vertical axis of
  // the vehicle) and project all rotations onto that axis to get approximate
  // rotations in the horizontal plane (corresponding to steering).
  const cv::Mat pca_axes =
      pilotguru::GetPrincipalRotationAxes(rotations, 500000);
  const cv::Vec3d vertical_axis(pca_axes.at<double>(0, 0),
                                pca_axes.at<double>(0, 1),
                                pca_axes.at<double>(0, 2));
  const std::vector<double> steering_angles =
      GetHorizontalTurnAngles(rotations, vertical_axis);
  CHECK_EQ(steering_angles.size(), rotations.size());

  // Save the projected horizontal rtotations.
  const std::vector<long> rotation_timestamps =
      pilotguru::ExtractTimestamps(rotations);
  pilotguru::JsonWriteTimestampedRealData(
      rotation_timestamps, steering_angles, FLAGS_steering_out_json,
      pilotguru::kSteering, pilotguru::kAngularVelocity);

  // Accelerometer autocalibration and forward velocity inference.

  // Optimizer parameters are the same for all iterations.
  LBFGSpp::LBFGSParam<double> param;
  param.epsilon = 1e-6;
  param.max_iterations = FLAGS_optimization_iters;

  // Slide an inteval of FLAGS_locations_batch_size with a step of
  // FLAGS_locations_shift_step over the reference GPS velocity measurements,
  // and fit the calibration parameters for the inertial measurements falling
  // within that reference interval.
  std::map<size_t, std::vector<double>> integrated_velocities;
  for (size_t reference_start_idx = 0;
       reference_start_idx < gps_velocities.size();
       reference_start_idx += FLAGS_locations_shift_step) {
    const size_t reference_end_idx =
        std::min(reference_start_idx + FLAGS_locations_batch_size,
                 gps_velocities.size());
    const std::vector<pilotguru::TimestampedVelocity> reference_interval(
        gps_velocities.begin() + reference_start_idx,
        gps_velocities.begin() + reference_end_idx);

    // Calibrator restricted to GPS measurements within the slidin window.
    pilotguru::AccelerometerCalibrator calibrator(reference_interval, rotations,
                                                  accelerations);
    // Fit the calibration parameters.
    LBFGSpp::LBFGSSolver<double> solver(param);
    Eigen::VectorXd x = Eigen::VectorXd::Zero(9);
    double fx;
    int niter = solver.minimize(calibrator, x, fx);

    LOG(INFO) << "Sliding window optimization: " << niter
              << " iterations, result value: " << fx;

    // TODO: group this into a struct and factor out vector-struct conversion
    // to calibrator.
    const Eigen::Vector3d acceleration_global_bias(x[0], x[1], x[2]);
    const Eigen::Vector3d acceleration_local_bias(x[3], x[4], x[5]);
    const Eigen::Vector3d initial_velocity(x[6], x[7], x[8]);

    // Integrate the inertial measuremens within the slifding window using the
    // optimal fitted calibration parameters.
    const std::map<size_t, pilotguru::MotionIntegrationOutcome>
        integrated_trajectory =
            calibrator.IntegrateTrajectory(acceleration_global_bias,
                                           acceleration_local_bias,
                                           initial_velocity);

    // Store velocity magnitudes within the sliding window for averaging later.
    for (const auto &point : integrated_trajectory) {
      integrated_velocities[point.first].push_back(
          point.second.velocity.norm());
    }
  }

  // This is only created to merge rotations and accelerations again and get
  // indices into the merged time series.
  // TODO: factor out merged time series into a class?
  pilotguru::AccelerometerCalibrator calibrator(gps_velocities, rotations,
                                                accelerations);

  std::vector<double> averaged_integrated_velocities;
  std::vector<double> timestamps_sec; // For smoothing.
  std::vector<long> timestamps_usec;  // For writing to JSON.
  // Average the velocities among the sliding windows falling on every IMU
  // measurement.
  for (auto &point : integrated_velocities) {
    timestamps_usec.push_back(
        calibrator.ImuTimes().MergedEventTimeUsec(point.first));
    timestamps_sec.push_back(
        static_cast<double>(timestamps_usec.back() - timestamps_usec.front()) *
        1e-6);
    const double velocity_sum =
        std::accumulate(point.second.begin(), point.second.end(), 0.0);
    averaged_integrated_velocities.push_back(velocity_sum /
                                             point.second.size());
  }
  // Temporal post-smoothing to remove the very high frequency noise.
  const std::vector<double> smoothed_velocities = pilotguru::SmoothTimeSeries(
      averaged_integrated_velocities, timestamps_sec, timestamps_sec,
      FLAGS_post_smoothing_sigma_sec);

  pilotguru::JsonWriteTimestampedRealData(
      timestamps_usec, smoothed_velocities, FLAGS_velocities_out_json,
      pilotguru::kVelocities, pilotguru::kSpeedMS);

  return EXIT_SUCCESS;
}
