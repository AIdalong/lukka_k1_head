#include "ble_broadcaster.h"
#include "esp_app_desc.h"
#include "esp_mac.h"

static const char* TAG_BLE = "BleBroadcaster";

BleBroadcaster& BleBroadcaster::GetInstance() {
    static BleBroadcaster instance;
    return instance;
}


esp_err_t BleBroadcaster::Start(const char* device_name) {
#if !BT_ENABLE
    ESP_LOGW(TAG_BLE, "BLE/NimBLE not enabled in sdkconfig.");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!device_name || *device_name == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (advertising_) {
        ESP_LOGI(TAG_BLE, "BLE advertising already started");
        return ESP_OK;
    }

    name_ = device_name;
    use_custom_payload_ = false;
    pending_start_ = true;

    if (!initialized_) {
        return InitStack_();
    }
    
    // If already initialized and synced, start immediately
    if (synced_) {
        StartAdvertising_();
    }
    
    return ESP_OK;
#endif
}

esp_err_t BleBroadcaster::StartCustom() {
#if !BT_ENABLE
    ESP_LOGW(TAG_BLE, "BLE/NimBLE not enabled in sdkconfig.");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (advertising_) {
        ESP_LOGI(TAG_BLE, "BLE advertising already started");
        return ESP_OK;
    }

    // --- Prepare the Custom Payload (Exact encoding as original) ---
    // 1. Fixed Name 'LUKKA'
    // 2. Manufacturer Data: 3 bytes BCD Version + 6 bytes MAC

    uint8_t adv_raw[31];
    uint8_t idx = 0;

    // AD1: Complete Local Name 'LUKKA'
    const uint8_t name_bytes[5] = {0x4C, 0x55, 0x4B, 0x4B, 0x41};
    adv_raw[idx++] = 1 + sizeof(name_bytes); // Length
    adv_raw[idx++] = 0x09; // Type: Complete Local Name
    memcpy(&adv_raw[idx], name_bytes, sizeof(name_bytes)); 
    idx += sizeof(name_bytes);

    // AD2: Manufacturer Specific Data
    auto app_desc = esp_app_get_description();
    std::string ver_str = app_desc->version;
    uint8_t ver[3] = {0x00, 0x00, 0x00};
    
    // Parse version string (e.g., "1.4.9")
    if (ver_str.length() >= 5) {
        ver[0] = ver_str[0] - '0';
        ver[1] = ver_str[2] - '0';
        ver[2] = ver_str[4] - '0';
        ESP_LOGD(TAG_BLE, "Parsed version: %02X.%02X.%02X", ver[0], ver[1], ver[2]);
    }

    uint8_t wifi_mac[6];
    esp_read_mac(wifi_mac, ESP_MAC_WIFI_STA);

    adv_raw[idx++] = 1 + 3 + 6; // Length (Type + 3 ver + 6 mac)
    adv_raw[idx++] = 0xFF;      // Type: Manufacturer Specific Data
    adv_raw[idx++] = ver[0];
    adv_raw[idx++] = ver[1];
    adv_raw[idx++] = ver[2];
    memcpy(&adv_raw[idx], wifi_mac, 6); 
    idx += 6;

    // Store payload for usage in the advertising task
    custom_payload_.assign(adv_raw, adv_raw + idx);
    use_custom_payload_ = true;
    name_ = "LUKKA"; // Keep name consistent
    pending_start_ = true;

    // --- Initialize Stack if needed ---
    if (!initialized_) {
        esp_err_t err = InitStack_();
        if (err != ESP_OK) return err;
        // InitStack_ starts a FreeRTOS task. The actual advertising will 
        // trigger in OnSync_ callback because pending_start_ is true.
        return ESP_OK;
    }

    // If already up, trigger immediately
    if (synced_) {
        StartAdvertising_();
    }

    ESP_LOGI(TAG_BLE, "Custom BLE configured. Waiting for stack sync...");
    return ESP_OK;
#endif
}

#if BT_ENABLE
esp_err_t BleBroadcaster::InitStack_() {
    esp_err_t ret;

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_BLE, "Failed to init NimBLE port");
        return ret;
    }

    // Set the sync callback - this is where we know the stack is ready
    ble_hs_cfg.sync_cb = BleBroadcaster::OnSync_;

    // Start the NimBLE task
    nimble_port_freertos_init(BleBroadcaster::HostTask_);
    
    initialized_ = true;
    ESP_LOGI(TAG_BLE, "NimBLE stack initialization started");
    return ESP_OK;
}

void BleBroadcaster::OnSync_(void) {
    ESP_LOGI(TAG_BLE, "NimBLE Host synced");
    
    auto& self = BleBroadcaster::GetInstance();
    
    // Ensure the device address is set (Public)
    // int rc = ble_hs_util_ensure_addr(0);
    // if (rc != 0) {
    //     ESP_LOGE(TAG_BLE, "Failed to ensure device address: %d", rc);
    //     return;
    // }

    self.synced_ = true;

    // If Start() or StartCustom() was called before sync, trigger it now.
    if (self.pending_start_) {
        self.StartAdvertising_();
    }
}

void BleBroadcaster::HostTask_(void* param) {
    ESP_LOGI(TAG_BLE, "NimBLE Host Task Started");
    nimble_port_run(); // This function blocks indefinitely
    nimble_port_freertos_deinit();
}

void BleBroadcaster::StartAdvertising_() {
    int rc;
    auto& self = BleBroadcaster::GetInstance();

    // 1. Set Advertising Data (Raw)
    if (self.use_custom_payload_) {
        // Use the raw bytes constructed in StartCustom
        rc = ble_gap_adv_set_data(self.custom_payload_.data(), self.custom_payload_.size());
    } else {
        // Fallback: Construct standard Name payload (similar to original Start())
        uint8_t adv_raw[31];
        uint8_t idx = 0;
        uint8_t name_len = (uint8_t)self.name_.length();
        if (name_len > 29) name_len = 29;

        adv_raw[idx++] = 1 + name_len;
        adv_raw[idx++] = 0x09; // Complete Local Name
        memcpy(&adv_raw[idx], self.name_.c_str(), name_len);
        idx += name_len;

        rc = ble_gap_adv_set_data(adv_raw, idx);
    }

    if (rc != 0) {
        ESP_LOGE(TAG_BLE, "Failed to set adv data: %d", rc);
        return;
    }

    // 2. Configure Advertising Parameters
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));

    // Non-connectable, undirected
    adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_NON;

    // Interval: Original was 0x40 (40ms) to 0x60 (60ms).
    // NimBLE units are 0.625ms. 
    // 40ms / 0.625 = 64
    // 60ms / 0.625 = 96
    adv_params.itvl_min = 64;
    adv_params.itvl_max = 96;

    // 3. Start Advertising
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, 
                           &adv_params, BleBroadcaster::GapEventCallback_, NULL);
    
    if (rc == 0) {
        self.advertising_ = true;
        self.pending_start_ = false; // Request fulfilled
        ESP_LOGI(TAG_BLE, "Advertising started (Name: %s)", self.name_.c_str());
        if(self.use_custom_payload_) {
            ESP_LOGD(TAG_BLE, "Payload size: %d", self.custom_payload_.size());
        }
    } else {
        ESP_LOGE(TAG_BLE, "Failed to start advertising: %d", rc);
    }
}

int BleBroadcaster::GapEventCallback_(struct ble_gap_event *event, void *arg) {
    // Handle GAP events if necessary (e.g., ADV stopped)
    // For a simple broadcaster, we mostly care about termination
    switch (event->type) {
        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGI(TAG_BLE, "Advertising stopped (reason: %d)", event->adv_complete.reason);
            BleBroadcaster::GetInstance().advertising_ = false;
            break;
        default:
            break;
    }
    return 0;
}
#endif