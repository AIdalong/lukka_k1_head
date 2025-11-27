#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>

#include "application.h"
#include "system_info.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // print task list and CPU usage every 5s
    // TaskHandle_t task_monitor_handle;
    // xTaskCreate(
    //     [](void* arg) {
    //     while (true) {
    //         vTaskDelay(pdMS_TO_TICKS(5000));
    //         SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
    //         SystemInfo::PrintTaskList();
    //     }
    // }, "task_monitor", 4096, nullptr, 1, &task_monitor_handle);


    // Launch the application
    Application::GetInstance().Start();
}
