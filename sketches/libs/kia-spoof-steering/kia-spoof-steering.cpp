#include <kia-spoof-steering.h>

#include <Arduino.h>

int TargetVoltageSmoother::SmoothedVoltageState::make_string(
    char *buf, size_t buf_size) const {
  if (buf == nullptr || buf_size < 1) {
    return -1;
  }
  return snprintf(buf, buf_size - 1, "%d,%d,%d,%d", smoothed_blue_voltage_,
                  smoothed_green_voltage_, current_offset_, target_offset_);
}

namespace {
uint16_t smooth_voltage(uint16_t old_smooth_voltage, uint16_t new_voltage,
                        uint16_t voltage_update_hystheresis) {
  return ((max(old_smooth_voltage, new_voltage) -
           min(old_smooth_voltage, new_voltage)) > voltage_update_hystheresis)
             ? new_voltage
             : old_smooth_voltage;
}

uint16_t add_offset(uint16_t base, int16_t offset) {
  if (offset == 0) {
    return base;
  } else if (offset > 0) {
    return (UINT16_MAX - base > offset) ? base + offset : UINT16_MAX;
  } else {
    return (base > -offset) ? base + offset : 0;
  }
}
}

TargetVoltageSmoother::TargetVoltageSmoother(
    const SteeringSpoofSettings &steering_spoof_settings)
    : steering_spoof_settings_(steering_spoof_settings) {}

void TargetVoltageSmoother::set_target_offset(int16_t new_target_offset) {
  voltage_state_.target_offset_ = constrain(
      new_target_offset, -steering_spoof_settings_.max_steering_magnitude,
      steering_spoof_settings_.max_steering_magnitude);
  if (voltage_state_.target_offset_ == voltage_state_.current_offset_) {
    steps_spent_at_current_offset_ =
        min(steps_spent_at_current_offset_,
            steering_spoof_settings_.steps_per_adjustment_level);
  }
}

void TargetVoltageSmoother::step() {
  // Prevent overflow.
  if (steps_spent_at_current_offset_ < UINT16_MAX) {
    ++steps_spent_at_current_offset_;
  }
  if (voltage_state_.target_offset_ != voltage_state_.current_offset_) {
    // We are still in progress of adjusting towards the target offset.
    if (steps_spent_at_current_offset_ >
        steering_spoof_settings_.steps_per_adjustment_level) {
      // Enough steps have been spent at the current offset, can move one unit
      // towards the target.
      voltage_state_.current_offset_ +=
          (voltage_state_.target_offset_ > voltage_state_.current_offset_) ? 1
                                                                           : -1;
      steps_spent_at_current_offset_ = 0;
    }
  } else if (voltage_state_.target_offset_ != 0 &&
             steps_spent_at_current_offset_ >
                 (steering_spoof_settings_.steps_at_target_level +
                  steering_spoof_settings_.steps_per_adjustment_level)) {
    // Enough time has been spent after reaching nonzero target offset, reset
    // the target to 0.
    voltage_state_.target_offset_ = 0;
  }
}

void TargetVoltageSmoother::update_measurments(
    const InstantVoltageData &voltage_data) {
  voltage_state_.smoothed_blue_voltage_ = smooth_voltage(
      voltage_state_.smoothed_blue_voltage_, voltage_data.blue_voltage,
      steering_spoof_settings_.voltage_update_hystheresis);
  voltage_state_.smoothed_green_voltage_ = smooth_voltage(
      voltage_state_.smoothed_green_voltage_, voltage_data.green_voltage,
      steering_spoof_settings_.voltage_update_hystheresis);
}

uint16_t TargetVoltageSmoother::get_smoothed_blue_voltage() const {
  return voltage_state_.smoothed_blue_voltage_;
}

uint16_t TargetVoltageSmoother::get_smoothed_green_voltage() const {
  return voltage_state_.smoothed_green_voltage_;
}

uint16_t TargetVoltageSmoother::get_target_blue_voltage() const {
  return add_offset(voltage_state_.smoothed_blue_voltage_,
                    -voltage_state_.current_offset_);
}

uint16_t TargetVoltageSmoother::get_target_green_voltage() const {
  return add_offset(voltage_state_.smoothed_green_voltage_,
                    voltage_state_.current_offset_);
}

int16_t TargetVoltageSmoother::get_current_offset() const {
  return voltage_state_.current_offset_;
}

int16_t TargetVoltageSmoother::get_target_offset() const {
  return voltage_state_.current_offset_;
}

const TargetVoltageSmoother::SmoothedVoltageState &
TargetVoltageSmoother::get_voltage_state() const {
  return voltage_state_;
}
