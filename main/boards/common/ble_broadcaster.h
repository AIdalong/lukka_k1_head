#pragma once

#include <string>
#include <cstring>
#include <vector>

#include "sdkconfig.h"
#include <esp_err.h>
#include <esp_log.h>
 

#define BT_ENABLE CONFIG_BT_NIMBLE_ENABLED

#if BT_ENABLE
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#else
#warning "NimBLE Bluetooth is disabled in sdkconfig!"
#endif

class BleBroadcaster {
public:
    static BleBroadcaster& GetInstance();

    // Start non-connectable advertising with the given device name.
    esp_err_t Start(const char* device_name);

    // Configure custom advertising payload: Name 'LUKKA' + manufacturer data.
    esp_err_t StartCustom();


private:
    BleBroadcaster() = default;
    BleBroadcaster(const BleBroadcaster&) = delete;
    BleBroadcaster& operator=(const BleBroadcaster&) = delete;

#if BT_ENABLE
    // Internal helper to initialize the stack
    esp_err_t InitStack_();
    
    // Internal helper to apply parameters and start advertising
    void StartAdvertising_();

    // NimBLE Callbacks
    static void OnSync_(void);
    static void HostTask_(void* param);
    static int GapEventCallback_(struct ble_gap_event *event, void *arg);
#endif

    bool initialized_ = false;
    bool synced_ = false;           // True when NimBLE stack is synced and ready
    bool advertising_ = false;
    
    // State to track pending actions if Start() is called before Sync
    bool pending_start_ = false;
    std::string name_;
    bool use_custom_payload_ = false;
    std::vector<uint8_t> custom_payload_;
};