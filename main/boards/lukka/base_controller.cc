#include "base_controller.h"
#include <string.h>
#include "config.h"
#include <esp_log.h>
#include "driver/uart.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"

static const char* TAG_BASE = "BaseController";

BaseController::BaseController() {
    uart_mutex_ = xSemaphoreCreateMutex();
    if (uart_mutex_ == nullptr) {
        ESP_LOGE(TAG_BASE, "Failed to create UART mutex");
    }
}

BaseController::~BaseController() { 
    StopProbeTask();
    if (uart_mutex_) {
        vSemaphoreDelete(uart_mutex_);
        uart_mutex_ = nullptr;
    }
}

bool BaseController::Initialize() {
    if (initialized_) return true;
    uart_config_t uart_config = {};
    uart_config.baud_rate = MOJI_UART_BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;
    int intr_alloc_flags = 0;
    if (uart_driver_install(MOJI_UART_PORT_NUM, 1024, 0, 0, NULL, intr_alloc_flags) != ESP_OK) {
        ESP_LOGE(TAG_BASE, "Failed to install UART driver for motor");
        return false;
    }
    if (uart_param_config(MOJI_UART_PORT_NUM, &uart_config) != ESP_OK) {
        ESP_LOGE(TAG_BASE, "Failed to config UART for motor");
        return false;
    }
    if (uart_set_pin(MOJI_UART_PORT_NUM, MOJI_UART_TXD, MOJI_UART_RXD, MOJI_UART_RTS, MOJI_UART_CTS) != ESP_OK) {
        ESP_LOGE(TAG_BASE, "Failed to set UART pins for motor");
        return false;
    }
    initialized_ = true;
    return true;
}

bool BaseController::IsInitialized() const {
    return initialized_;
}

void BaseController::ControlMotor(char direction, int steps) {
    if (!IsInitialized()) {
        // try to initialize lazily
        const_cast<BaseController*>(this)->Initialize();
    }
    if (!IsInitialized()) return;
    char buffer[16];
    int n = snprintf(buffer, sizeof(buffer), "%c%d\r\n", direction, steps);
    if (n > 0) SendMotorCommand(buffer);
}

void BaseController::ResetMotor() {
    if (!IsInitialized()) {
        const_cast<BaseController*>(this)->Initialize();
    }
    if (!IsInitialized()) return;
    SendMotorCommand("X\r\n");
}

bool BaseController::SendMotorCommand(const char* cmd) {
    if (!IsInitialized()) Initialize();
    if (!IsInitialized()) return false;
    if (uart_mutex_ == nullptr) return false;
    
    // Acquire mutex to protect UART write
    if (xSemaphoreTake(uart_mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG_BASE, "Failed to acquire UART mutex for write");
        return false;
    }
    
    bool result = SendMotorCommandInternal(cmd);
    xSemaphoreGive(uart_mutex_);
    return result;
}

bool BaseController::SendMotorCommandInternal(const char* cmd) {
    if (!IsInitialized()) return false;
    size_t len = strlen(cmd);
    uart_write_bytes(MOJI_UART_PORT_NUM, cmd, len);
    ESP_LOGD(TAG_BASE, "Motor cmd: %s", cmd);
    return true;
}

void BaseController::SetMotion(int motion) {
    current_motion_ = (EmojiMotion)motion;
    if (motion_task_handle_ == nullptr) {
        BaseType_t rt = xTaskCreate(MotionTask, "base_motion_task", 2048, this, 1, &motion_task_handle_);
        if (rt != pdPASS) {
            ESP_LOGE(TAG_BASE, "Failed to create base motion task");
            motion_task_handle_ = nullptr;
        }
    }
    xTaskNotifyGive(motion_task_handle_);
}

void BaseController::SetPlacementState(PlacementState s) {
    if (placement_state_ == s) return;
    PlacementState old = placement_state_;
    placement_state_ = s;
    if (placement_changed_cb_) placement_changed_cb_(placement_state_, old);
}

bool BaseController::StartProbeTask() {
    if (probe_task_handle_) return true;
    BaseType_t rt = xTaskCreate(ProbeTask, "base_probe_task", 3072, this, 1, &probe_task_handle_);
    if (rt != pdPASS) {
        ESP_LOGE(TAG_BASE, "Failed to create base probe task");
        probe_task_handle_ = nullptr;
        return false;
    }
    return true;
}

void BaseController::StopProbeTask() {
    if (probe_task_handle_) {
        vTaskDelete(probe_task_handle_);
        probe_task_handle_ = nullptr;
    }
}

void BaseController::ProbeTask(void* arg) {
    BaseController* self = static_cast<BaseController*>(arg);
    const TickType_t delay = pdMS_TO_TICKS(500);
    for (;;) {
        if (!self->IsInitialized()) {
            self->Initialize();
        }
        if (self->IsInitialized()) {
            if (self->current_motion_ == NONE && self->previous_motion_ == NONE) {
                // Acquire mutex for entire probe operation (write + read)
                if (self->uart_mutex_ && xSemaphoreTake(self->uart_mutex_, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    // Use internal version to avoid double mutex acquisition
                    self->SendMotorCommandInternal("L0\r\n");

                    uint8_t buf[128];
                    int len = uart_read_bytes(MOJI_UART_PORT_NUM, buf, sizeof(buf) - 1, pdMS_TO_TICKS(120));
                    xSemaphoreGive(self->uart_mutex_);

                    if (len > 0) {
                        buf[len] = 0;
                        bool on_rotating = (strstr((const char*)buf, "step") != nullptr);
                        if (on_rotating) {
                            if (self->placement_state_ != kPlacementRotatingBase) {
                                ESP_LOGI(TAG_BASE, "Detected rotating base (uart contains 'step')");
                                self->SetPlacementState(kPlacementRotatingBase);
                                self->trial_count_ = 0;
                            }
                        } else {
                            if (self->placement_state_ != kPlacementIndependent) {
                                self->trial_count_++;
                                if (self->trial_count_ >= self->MAX_TRIALS) {
                                    ESP_LOGI(TAG_BASE, "No 'step' found in uart response, switch to independent");
                                    self->SetPlacementState(kPlacementIndependent);
                                    self->trial_count_ = 0;
                                }
                            }
                        }
                    } else {
                        if (self->placement_state_ != kPlacementIndependent) {
                            self->trial_count_++;
                            if (self->trial_count_ >= self->MAX_TRIALS) {
                                ESP_LOGI(TAG_BASE, "No uart response, switch to independent");
                                self->SetPlacementState(kPlacementIndependent);
                                self->trial_count_ = 0;
                            }
                        }
                    }
                } else {
                    ESP_LOGW(TAG_BASE, "Failed to acquire UART mutex for probe");
                }
            }
            else if (self->current_motion_ != NONE && self->previous_motion_ == NONE) {
                ESP_LOGI(TAG_BASE, "Emoji motion playing (%d), skipping placement probe", (int)self->current_motion_);
            }
            else {
                ESP_LOGI(TAG_BASE, "Emoji motion ended, resuming placement probe in 0.5s");
            }
            self->previous_motion_ = self->current_motion_;
        }
        vTaskDelay(delay);
    }
}

void BaseController::MotionTask(void* arg) {
    BaseController* self = static_cast<BaseController*>(arg);
    if (!self->IsInitialized()) {
        self->Initialize();
    }
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (self->current_motion_ != NONE) {
            ESP_LOGI(TAG_BASE, "Executing motion command: %d", (int)self->current_motion_);
            
            switch (self->current_motion_) {
                case LOOKRIGHT:
                    self->ControlMotor('L', 10);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    self->ControlMotor('R', 10);
                    break;
                case LOOKLEFT:
                    self->ControlMotor('R', 10);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    self->ControlMotor('L', 10);
                    break;
                case MUSIC:
                    for (;;) {
                        self->ControlMotor('R', 10);
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        self->ControlMotor('L', 10);
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        // check if new motion command arrived
                        if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
                            ESP_LOGI(TAG_BASE, "New motion command received, stopping MUSIC motion");
                            break;
                        }
                    }
                    break;
                case ANGRY:
                    // Delay to allow angry emoji to be visible before motor starts rotating
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    self->ControlMotor('L', 48);
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    self->ControlMotor('R', 48);
                    break;
                case DIZZY:
                    // Delay to allow dizzy emoji to be visible before motor starts rotating
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    self->ControlMotor('R', 96);
                    break;
                default:
                    break;
            }
            self->current_motion_ = NONE;
        }
    }
}