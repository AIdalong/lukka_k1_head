#pragma once

#include <string>
#include <cstdint>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>


enum EmojiMotion {
    NONE,
    LOOKRIGHT,
    LOOKLEFT,
    MUSIC,
    DIZZY,
    ANGRY
};

class BaseController {
public:
    enum PlacementState {
        kPlacementIndependent = 0,
        kPlacementRotatingBase = 1,
        kPlacementStaticBase = 2
    };

    BaseController();
    ~BaseController();

    bool Initialize();
    bool IsInitialized() const;

    // Motor control API
    // Initialize UART for motor control
    bool SendMotorCommand(const char* cmd);  // Public API with mutex protection
    void ControlMotor(char direction, int steps);
    void ResetMotor();

    // Probe task control
    bool StartProbeTask();
    void StopProbeTask();

    void SetMotion(int motion);

    PlacementState GetPlacementState() const { return placement_state_; }
    void SetPlacementState(PlacementState s);
    bool SendMotorCommandInternal(const char* cmd);  // Internal version without mutex (caller must hold mutex)

    // Callback invoked when placement state changes: (new, old)
    void SetPlacementChangedCallback(std::function<void(PlacementState, PlacementState)> cb) { placement_changed_cb_ = cb; }

private:
    // Motor initialized flag (replaces separate MotorController instance)
    bool initialized_ = false;
    TaskHandle_t probe_task_handle_ = nullptr;
    TaskHandle_t motion_task_handle_ = nullptr;
    SemaphoreHandle_t uart_mutex_ = nullptr;  // Mutex to protect UART access
    PlacementState placement_state_ = kPlacementIndependent;
    std::function<void(PlacementState, PlacementState)> placement_changed_cb_;

    // set a tolerance for retries before switching to independent
    const int MAX_TRIALS = 2;
    int trial_count_ = 0;

    EmojiMotion current_motion_ = NONE;
    EmojiMotion previous_motion_ = NONE;

    static void ProbeTask(void* arg);
    static void MotionTask(void* arg);
};
