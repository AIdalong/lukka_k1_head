#include "motion_detector.h"
#include <esp_log.h>
#include <cmath>
#include "application.h"
#include "board.h"
#include "esp_timer.h"
#include "mmap_generate_moji_emoji.h"

static const char* TAG_MOTION = "MotionDetector";

# ifdef GIMBAL_MODE

#include "kalman_filter.h"

Eigen::Vector3d accel_meas = Eigen::Vector3d::Zero();
Eigen::Vector3d gyro_meas = Eigen::Vector3d::Zero();
Eigen::Quaterniond q(1, 0, 0, 0);
float roll = 0, pitch = 0, yaw = 0;
Eigen::Vector<double, 9> lin_state_vec;
Eigen::Vector3d g(0, 0, -9.80665);
QueueHandle_t imu_measurement_queue;


Eigen::Matrix<double, 9, 9> get_A(double dt)
{
    Eigen::Matrix<double, 9, 9> A = Eigen::Matrix<double, 9, 9>::Identity();

    for (int i = 0; i < 6; i++)
    {
        A(i,i+3) = dt;
    }

    for (int i = 0; i < 3; i++)
    {
        A(i,i+6) = 0.5 * pow(dt,2);
    }

    return A;
}

Eigen::Vector3d to_euler(const Eigen::Quaterniond& q) {
    // https://stackoverflow.com/questions/31589901/euler-to-quaternion-quaternion-to-euler-using-eigen
    Eigen::Vector3d angles;
    const auto x = q.x();
    const auto y = q.y();
    const auto z = q.z();
    const auto w = q.w();

    // roll (x-axis rotation)
    double sinr_cosp = 2 * (w * x + y * z);
    double cosr_cosp = 1 - 2 * (x * x + y * y);
    angles[0] = std::atan2(sinr_cosp, cosr_cosp);

    // pitch (y-axis rotation)
    double sinp = 2 * (w * y - z * x);
    if (std::abs(sinp) >= 1)
        angles[1] = std::copysign(M_PI / 2, sinp); // use 90 degrees if out of range
    else
        angles[1] = std::asin(sinp);

    // yaw (z-axis rotation)
    double siny_cosp = 2 * (w * z + x * y);
    double cosy_cosp = 1 - 2 * (y * y + z * z);
    angles[2] = std::atan2(siny_cosp, cosy_cosp);
    return angles;
}

void state_est_task(void *)
{

    Eigen::Vector<double, 9> x_init = Eigen::Vector<double, 9>::Zero();
    Eigen::Matrix<double, 9, 9> P_init = Eigen::Matrix<double, 9, 9>::Identity();Eigen::Matrix<double, 9, 9> Q_init = 0.1 * Eigen::Matrix<double, 9, 9>::Identity();
    Eigen::Matrix<double, 3, 3> R_init = 0.1 * Eigen::Matrix<double, 3, 3>::Identity();
    Eigen::Matrix<double, 3, 9> H = Eigen::Matrix<double, 3, 9>::Zero();
    for (int i = 0; i<3; i++)
    {
        H(i,i+6) = 1;
    }

    std::function<Eigen::Matrix<double, 9, 9>(double)> A_ptr {&get_A};

    KalmanFilter<9,3> kf(x_init, P_init, Q_init, R_init, A_ptr, H);

    Eigen::Vector3d accel_meas_raw;
    Eigen::Vector3d gyro_meas_raw;
    while (1)
    {
        vTaskDelay(1);
        IMUMeasurement imu_meas;
        if(xQueueReceive(imu_measurement_queue,&imu_meas,0 ) == pdTRUE)
        {
            accel_meas_raw = imu_meas.accel_meas;
            gyro_meas_raw = imu_meas.gyro_meas;
            // gyro integration w/ quaternions
            double angle = gyro_meas_raw.norm() * dt;
            Eigen::Vector3d axis = gyro_meas_raw.normalized();
            Eigen::Quaterniond q_delta(Eigen::AngleAxisd(angle, axis));
            q = q * q_delta;

            // tilt correction using complementary filter
            Eigen::Quaterniond q_accel_body;
            q_accel_body.w() = 0;
            q_accel_body.vec() = accel_meas_raw;
            Eigen::Quaterniond q_accel_world = q * q_accel_body * q.inverse();

            Eigen::Vector3d v_accel_world(
                q_accel_world.x()/q_accel_world.norm(), 
                q_accel_world.y()/q_accel_world.norm(), 
                q_accel_world.z()/q_accel_world.norm()
            );
            Eigen::Vector3d n = v_accel_world.cross(Eigen::Vector3d(0, 0, 1));
            double phi = std::acos(v_accel_world.dot(Eigen::Vector3d(0, 0, 1)));
            float alpha = std::min(std::max(
                ((1-0.1)/(9.80665f*2)) * (accel_meas.norm()-9.80665f)+0.1, 
            0.0),1.0);
            //printf("%f ", alpha);
            Eigen::Quaterniond q_tilt(Eigen::AngleAxisd((1-alpha) * phi, n));
            Eigen::Quaterniond q_c = q_tilt * q;
            Eigen::Vector3d euler = to_euler(q_c);
            roll = euler(1) * 180 / M_PI;
            pitch = euler(0) * 180 / M_PI;
            yaw = -euler(2) * 180 / M_PI;

            if (yaw > 180.f)
                yaw -= 360.f;
            else if (yaw < -180.f)
                yaw += 360.f;

            q_accel_world = q_c * q_accel_body * q_c.inverse();
            accel_meas = q_accel_world.vec();//q_accel_world.vec() + g;
            gyro_meas = gyro_meas_raw;

            kf.Predict(dt);
            kf.Update(accel_meas);
            lin_state_vec = kf.GetState();  
        }
    }

    vTaskDelete(nullptr);
}

void log_task(void *)
{
    static const float YAW_DEADZONE = 0.5f;
    static const float YAW_KP = 6.0f;
    static const int YAW_MAX_STEPS = 20;
    static const int INTERVAL_MS = 250;
    int cnt = 0;
    while (1)
    {
        vTaskDelay(  pdMS_TO_TICKS(INTERVAL_MS));
        if (++cnt * INTERVAL_MS >= 500)
        {
            cnt = 0;        
            // ESP_LOGI(TAG_MOTION, "==========================");
            // print lin_state_vec
            // printf("%+.6f %+.6f %+.6f %+.6f %+.6f %+.6f %+.6f %+.6f %+.6f ",
            //     lin_state_vec(0), lin_state_vec(1), lin_state_vec(2),
            //     lin_state_vec(3), lin_state_vec(4), lin_state_vec(5),
            //     lin_state_vec(6), lin_state_vec(7), lin_state_vec(8));
            printf("imu:%.6f,%.6f,%.6f,", accel_meas(0), accel_meas(1), accel_meas(2));
            printf("%.6f,%.6f, %.6f,", gyro_meas(0), gyro_meas(1), gyro_meas(2));
            printf("%.6f,%.6f,%.6f\n", roll, pitch, yaw);
        }
        if ( yaw > YAW_DEADZONE)
            Board::GetInstance().MojiControlMotor('L', std::min((int)(yaw * YAW_KP), YAW_MAX_STEPS));
        else if (yaw < -YAW_DEADZONE)
            Board::GetInstance().MojiControlMotor('R', std::min((int)(-yaw * YAW_KP), YAW_MAX_STEPS));
    }

}

#endif

MotionDetector::MotionDetector(std::function<void(MotionEvent)> on_motion,
                               std::function<void()> on_shake)
        : on_motion_(on_motion), on_shake_(on_shake) {
# ifdef GIMBAL_MODE
            imu_measurement_queue = xQueueCreate(10, sizeof(IMUMeasurement));
            xTaskCreate(state_est_task,"EstTask",12 * 1024,nullptr,9,nullptr);
            xTaskCreate(log_task,"LogTask",2 * 1024,nullptr,8,nullptr);
# endif
}

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

# ifdef GIMBAL_MODE
    IMUMeasurement imu_meas;
    imu_meas.accel_meas = Eigen::Vector3d(accel_x, accel_y, accel_z);
    imu_meas.gyro_meas = Eigen::Vector3d(gyro_x, gyro_y, gyro_z);
    // convert gyro from deg/s to rad/s
    imu_meas.gyro_meas *= M_PI / 180.0;
    if(xQueueSend(imu_measurement_queue,&imu_meas,0) != pdTRUE)
    {
        printf("Failed to send IMU measurement to queue\n");
        xQueueReceive(imu_measurement_queue,&imu_meas,0);
    }
# else
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
        accel_y_buffer.push_back(last_.ay);
        gyro_z_buffer.push_back(last_.gz);
        if (accel_x_buffer.size() > (size_t)BUFFER_SIZE) accel_x_buffer.erase(accel_x_buffer.begin());
        if (accel_y_buffer.size() > (size_t)BUFFER_SIZE) accel_y_buffer.erase(accel_y_buffer.begin());
        if (gyro_z_buffer.size() > (size_t)BUFFER_SIZE) gyro_z_buffer.erase(gyro_z_buffer.begin());
        mean_index ++;
        if (mean_index >= BUFFER_SIZE) {
            mean_index = 0;

            // calculate averages
            float sum_ax = 0.0f;
            float sum_gz = 0.0f;
            for (float val : accel_x_buffer) sum_ax += val;
            for (float val : gyro_z_buffer) sum_gz += val;

            // store means for CUSUM
            accel_x_means.push_back(sum_ax / accel_x_buffer.size());
            gyro_z_means.push_back(sum_gz / gyro_z_buffer.size());
            if (accel_x_means.size() > (size_t)CUSUM_SIZE) accel_x_means.erase(accel_x_means.begin());
            if (gyro_z_means.size() > (size_t)CUSUM_SIZE) gyro_z_means.erase(gyro_z_means.begin());
        }
        return;
    }

    if (!detection_enabled) {
        detection_enabled = true;
        ESP_LOGI(TAG_MOTION, "Vehicle motion detection enabled after stabilization period");
    }

    ProcessVehicleMotion();
# endif
}

void MotionDetector::ProcessVehicleMotion() {


    accel_x_buffer.push_back(last_.ax);
    gyro_z_buffer.push_back(last_.gz);
    if (accel_x_buffer.size() > (size_t)BUFFER_SIZE) accel_x_buffer.erase(accel_x_buffer.begin());
    if (gyro_z_buffer.size() > (size_t)BUFFER_SIZE) gyro_z_buffer.erase(gyro_z_buffer.begin());
    mean_index ++;

    if (mean_index >= BUFFER_SIZE) {
        mean_index = 0;
        
        // calculate averages
        float sum_ax = 0.0f;
        float sum_gz = 0.0f;
        for (float val : accel_x_buffer) sum_ax += val;
        for (float val : gyro_z_buffer) sum_gz += val;

        // store means for CUSUM
        accel_x_means.push_back(sum_ax / accel_x_buffer.size());
        gyro_z_means.push_back(sum_gz / gyro_z_buffer.size());
        if (accel_x_means.size() > (size_t)CUSUM_SIZE) accel_x_means.erase(accel_x_means.begin());
        if (gyro_z_means.size() > (size_t)CUSUM_SIZE) gyro_z_means.erase(gyro_z_means.begin());

        DetectVehiclePosture(esp_timer_get_time());
    }
}

void MotionDetector::DetectVehiclePosture(int64_t current_time) {
    // Board is responsible for gating animations; MotionDetector only emits events
    const float ACCEL_X_THRESHOLD_ACCEL = 0.25f * 9.80665f;
    const float ACCEL_X_THRESHOLD_BRAKE = 0.25f * 9.80665f;
    const float GYRO_Z_THRESHOLD = 18.0f;

    float train_mean = 0.0f;
    for (size_t i = 0; i < accel_x_means.size() * 3 / 4; i++) {
        train_mean += accel_x_means[i];
    }
    train_mean /= (accel_x_means.size() * 3 / 4);

    float detect_val = 0.0f;
    for (size_t i = accel_x_means.size() * 3 / 4; i < accel_x_means.size(); i++) {
        detect_val += accel_x_means[i];
    }   
    detect_val /= (accel_x_means.size() / 4);

    float gyro_z_filtered = gyro_z_means.back();

    if (detect_val < train_mean - ACCEL_X_THRESHOLD_ACCEL) {
        ESP_LOGI(TAG_MOTION, "Vehicle accelerating: ax=%.3f g",  detect_val / 9.80665f);
        if (on_motion_) on_motion_(MotionEvent::Speeding);
        init_start_time = 0;
        return;
    }

    if (detect_val > train_mean + ACCEL_X_THRESHOLD_BRAKE) {
        ESP_LOGI(TAG_MOTION, "Vehicle braking: ax=%.3f g", detect_val / 9.80665f);
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
