#include "motion_detector.h"
#include <esp_log.h>
#include <cmath>
#include "esp_timer.h"
#include "mmap_generate_moji_emoji.h"

static const char* TAG_MOTION = "MotionDetector";

MotionDetector::MotionDetector(std::function<void(MotionEvent)> on_motion,
                               std::function<void()> on_shake)
        : on_motion_(on_motion), on_shake_(on_shake) {}

MotionDetector::~MotionDetector() {}

void MotionDetector::SetPlacementIndependent(bool independent) {
    placement_independent_ = independent;
    // reset stabilization timer when placement changes
    if (independent) {
        detection_enabled = false;
        init_start_time = 0;
    } else {
        init_start_time = 0; // will be set on next data
    }
}

void MotionDetector::OnSensorData(float accel_x, float accel_y, float accel_z, float gyro_x, float gyro_y, float gyro_z) {
    last_.ax = accel_x; last_.ay = accel_y; last_.az = accel_z;
    last_.gx = gyro_x; last_.gy = gyro_y; last_.gz = gyro_z;

    // // print data to serial for debugging
    // ESP_LOGI(TAG_MOTION, "IMU:0, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f", last_.ax, last_.ay, last_.az, last_.gx, last_.gy, last_.gz);

    if (placement_independent_) {
    DetectShake(esp_timer_get_time());
        return; // skip when independent
    }

    int64_t current_time = esp_timer_get_time();
    if (init_start_time == 0) {
        init_start_time = current_time;
        ESP_LOGI(TAG_MOTION, "Vehicle motion detection starting, stabilization period: 5 seconds");
    }

    if (current_time - init_start_time < INIT_STABILIZATION_US) {
        accel_x_buffer.push_back(last_.ax);
        accel_z_buffer.push_back(last_.az);
        gyro_z_buffer.push_back(last_.gz);
        if (accel_x_buffer.size() > (size_t)BUFFER_SIZE) accel_x_buffer.erase(accel_x_buffer.begin());
        if (accel_z_buffer.size() > (size_t)BUFFER_SIZE) accel_z_buffer.erase(accel_z_buffer.begin());
        if (gyro_z_buffer.size() > (size_t)BUFFER_SIZE) gyro_z_buffer.erase(gyro_z_buffer.begin());
        mean_index ++;
        if (mean_index >= BUFFER_SIZE) {
            mean_index = 0;

            // calculate averages
            float sum_ax = 0.0f;
            float sum_az = 0.0f;
            float sum_gz = 0.0f;
            for (float val : accel_x_buffer) sum_ax += val;
            for (float val : accel_z_buffer) sum_az += val;
            for (float val : gyro_z_buffer) sum_gz += val;

            // store means for CUSUM
            accel_x_means.push_back(sum_ax / accel_x_buffer.size());
            accel_z_means.push_back(sum_az / accel_z_buffer.size());
            gyro_z_means.push_back(sum_gz / gyro_z_buffer.size());
            if (accel_x_means.size() > (size_t)CUSUM_SIZE) accel_x_means.erase(accel_x_means.begin());
            if (accel_z_means.size() > (size_t)CUSUM_SIZE) accel_z_means.erase(accel_z_means.begin());
            if (gyro_z_means.size() > (size_t)CUSUM_SIZE) gyro_z_means.erase(gyro_z_means.begin());
        }
        return;
    }

    if (!detection_enabled) {
        detection_enabled = true;
        ESP_LOGI(TAG_MOTION, "Vehicle motion detection enabled after stabilization period");
    }

    ProcessVehicleMotion();
}

void MotionDetector::ProcessVehicleMotion() {


    accel_x_buffer.push_back(last_.ax);
    accel_z_buffer.push_back(last_.az);
    gyro_z_buffer.push_back(last_.gz);
    if (accel_x_buffer.size() > (size_t)BUFFER_SIZE) accel_x_buffer.erase(accel_x_buffer.begin());
    if (accel_z_buffer.size() > (size_t)BUFFER_SIZE) accel_z_buffer.erase(accel_z_buffer.begin());
    if (gyro_z_buffer.size() > (size_t)BUFFER_SIZE) gyro_z_buffer.erase(gyro_z_buffer.begin());
    mean_index ++;

    if (mean_index >= BUFFER_SIZE) {
        mean_index = 0;
        
        // calculate averages
        float sum_ax = 0.0f;
        float sum_az = 0.0f;
        float sum_gz = 0.0f;
        for (float val : accel_x_buffer) sum_ax += val;
        for (float val : accel_z_buffer) sum_az += val;
        for (float val : gyro_z_buffer) sum_gz += val;

        // store means for CUSUM
        accel_x_means.push_back(sum_ax / accel_x_buffer.size());
        accel_z_means.push_back(sum_az / accel_z_buffer.size());
        gyro_z_means.push_back(sum_gz / gyro_z_buffer.size());
        if (accel_x_means.size() > (size_t)CUSUM_SIZE) accel_x_means.erase(accel_x_means.begin());
        if (accel_z_means.size() > (size_t)CUSUM_SIZE) accel_z_means.erase(accel_z_means.begin());
        if (gyro_z_means.size() > (size_t)CUSUM_SIZE) gyro_z_means.erase(gyro_z_means.begin());

        DetectVehiclePosture(esp_timer_get_time());
    }
}

void MotionDetector::DetectVehiclePosture(int64_t current_time) {
    // Board is responsible for gating animations; MotionDetector only emits events
    const float ACCEL_X_THRESHOLD_ACCEL = 0.25f * 9.80665f;
    const float ACCEL_X_THRESHOLD_BRAKE = 0.15f * 9.80665f;
    const float GYRO_Z_THRESHOLD = 18.0f;

    float train_mean_x = 0.0f;
    for (size_t i = 0; i < accel_x_means.size() * 3 / 4; i++) {
        train_mean_x += accel_x_means[i];
    }
    train_mean_x /= (accel_x_means.size() * 3 / 4);

    float train_mean_z = 0.0f;
    for (size_t i = 0; i < accel_z_means.size() * 3 / 4; i++) {
        train_mean_z += accel_z_means[i];
    }
    train_mean_z /= accel_z_means.size() * 3 / 4;

    float detect_val_x = 0.0f;
    for (size_t i = accel_x_means.size() * 3 / 4; i < accel_x_means.size(); i++) {
        detect_val_x += accel_x_means[i];
    }   
    detect_val_x /= (accel_x_means.size() / 4);

    float detect_val_z = 0.0f;
    for (size_t i = accel_z_means.size() * 3 / 4; i < accel_z_means.size(); i++) {
        detect_val_z += accel_z_means[i];
    }
    detect_val_z /= (accel_z_means.size() / 4);

    // assume (train_mean_x, train_mean_y) is the gravity vector direction
    // the detection value is the projection of (detect_val_x, detect_val_y) onto the direction perpendicular to gravity
    float detect_val = (detect_val_x - train_mean_x) * (-train_mean_z) + (detect_val_z - train_mean_z) * train_mean_x;
    detect_val /= std::sqrt(train_mean_x * train_mean_x + train_mean_z * train_mean_z);

    // ESP_LOGI(TAG_MOTION, "Motion detection values: ax=%.3f g, az=%.3f g, detect=%.3f g", 
    //          last_.ax / 9.80665f, last_.az / 9.80665f, detect_val / 9.80665f);

    float gyro_z_filtered = gyro_z_means.back();

    if (detect_val < - ACCEL_X_THRESHOLD_ACCEL) {
        ESP_LOGI(TAG_MOTION, "Vehicle accelerating: ax=%.3f g < -%.3f g",  detect_val / 9.80665f, ACCEL_X_THRESHOLD_ACCEL / 9.80665f);
        if (on_motion_) on_motion_(MotionEvent::Speeding);
        init_start_time = 0;
        return;
    }

    if (detect_val >  ACCEL_X_THRESHOLD_BRAKE) {
        ESP_LOGI(TAG_MOTION, "Vehicle braking: ax=%.3f g > %.3f g", detect_val / 9.80665f, ACCEL_X_THRESHOLD_BRAKE / 9.80665f);
        if (on_motion_) on_motion_(MotionEvent::Braking);
        init_start_time = 0;
        return;
    }

    if (gyro_z_filtered > GYRO_Z_THRESHOLD) {
        ESP_LOGI(TAG_MOTION, "Vehicle turning left: gz=%.1f deg/s", gyro_z_filtered);
        if (on_motion_) on_motion_(MotionEvent::TurnLeft);
        init_start_time = 0;
        return;
    }

    if (gyro_z_filtered < -GYRO_Z_THRESHOLD) {
        ESP_LOGI(TAG_MOTION, "Vehicle turning right: gz=%.1f deg/s", gyro_z_filtered);
        if (on_motion_) on_motion_(MotionEvent::TurnRight);
        init_start_time = 0;
        return;
    }
}

void MotionDetector::DetectShake(int64_t current_time) {
    if (current_time - last_shake_animation_time < SHAKE_COOLDOWN_US) return;

    float gx = last_.gx, gy = last_.gy, gz = last_.gz;
    float shake_intensity = std::sqrt(gx*gx + gy*gy + gz*gz);
    if (shake_intensity > SHAKE_THRESHOLD) {
        if (!is_shaking) {
            is_shaking = true;
            shake_start_time = current_time;
            ESP_LOGD(TAG_MOTION, "Independent shake started: intensity=%.1f deg/s", shake_intensity);
        } else {
            int64_t shake_duration = current_time - shake_start_time;
            if (shake_duration >= SHAKE_DETECTION_DURATION_US) {
                ESP_LOGI(TAG_MOTION, "Independent shake detected for %.1f seconds", shake_duration / 1000000.0f);
                if (on_shake_) on_shake_();
                last_shake_animation_time = current_time;
                is_shaking = false;
                shake_start_time = 0;
            }
        }
    } else {
        if (is_shaking) {
            ESP_LOGD(TAG_MOTION, "Independent shake stopped: intensity=%.1f deg/s", shake_intensity);
            is_shaking = false;
            shake_start_time = 0;
        }
    }
}
