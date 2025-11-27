#pragma once

#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include <string>
#include <cstdint>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>

class BaseController {
public:
    enum PlacementState {
        kPlacementIndependent = 0,
        kPlacementRotatingBase = 1,
        kPlacementStaticBase = 2
    };

    enum EmojiMotion {
        NONE,
        LOOKRIGHT,
        LOOKLEFT,
        SHAKE_12_STEPS,
        SHAKE_6_STEPS
    };

    BaseController();
    ~BaseController();

    bool Initialize();
    bool IsInitialized() const;

    // Motor control API
    // Initialize UART for motor control
    bool SendMotorCommand(const char* cmd);
    void ControlMotor(char direction, int steps);
    void ResetMotor();

    // Probe task control
    bool StartProbeTask();
    void StopProbeTask();

    // Set a series motion
    bool SetMotion(const EmojiMotion motion);

    PlacementState GetPlacementState() const { return placement_state_; }
    void SetPlacementState(PlacementState s);

    // Callback invoked when placement state changes: (new, old)
    void SetPlacementChangedCallback(std::function<void(PlacementState, PlacementState)> cb) { placement_changed_cb_ = cb; }

private:
    std::mutex mutex_;
    // Motor initialized flag (replaces separate MotorController instance)
    bool initialized_ = false;
    TaskHandle_t probe_task_handle_ = nullptr;
    esp_timer_handle_t motion_start_handle_ = nullptr;
    esp_timer_handle_t motion_compelete_timer_ = nullptr;
    PlacementState placement_state_ = kPlacementIndependent;
    EmojiMotion current_motion_ = NONE;
    std::function<void(PlacementState, PlacementState)> placement_changed_cb_;

    // set a tolerance for retries before switching to independent
    const int MAX_TRIALS = 2;
    int trial_count_ = 0;

    static void ProbeTask(void* arg);
    static void MotionTask(void* arg);
};
