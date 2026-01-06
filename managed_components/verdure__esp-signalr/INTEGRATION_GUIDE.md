# Integration Guide - ESP32 SignalR Client

Complete step-by-step guide for integrating the ESP32 SignalR client into your project.

## Phase 1: Project Setup

### 1.1 Install ESP-IDF

Ensure ESP-IDF 5.0 or later is installed:

```bash
esp-idf version
# Should output: v5.0 or higher
```

### 1.2 Add Component to Project

```bash
cd your-esp32-project
mkdir -p managed_components
cd managed_components
git clone https://github.com/maker-community/esp-signalr.git verdure__esp-signalr
```

Or add to `main/idf_component.yml`:

```yaml
dependencies:
  verdure/esp-signalr: "^1.0.0"
```

### 1.3 Configure C++ Exceptions

Create or modify `sdkconfig.defaults`:

```
# Enable C++ exceptions (required)
CONFIG_COMPILER_CXX_EXCEPTIONS=y
CONFIG_COMPILER_CXX_EXCEPTIONS_EMG_POOL_SIZE=512

# Optional: Enable PSRAM for large messages
CONFIG_SPIRAM_SUPPORT=y
CONFIG_SPIRAM_USE_MALLOC=y

# Optional: Optimize for size
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
```

## Phase 2: WiFi Setup

SignalR requires an active network connection. Here's a complete WiFi setup:

```cpp
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/event_groups.h"

#define WIFI_SSID      "your-ssid"
#define WIFI_PASSWORD  "your-password"

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void event_handler(void* arg, esp_event_base_t event_base,
                         int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI("WIFI", "Retry connecting to WiFi");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI("WIFI", "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASSWORD);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI("WIFI", "Waiting for WiFi connection...");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI("WIFI", "Connected to WiFi");
    } else {
        ESP_LOGE("WIFI", "Failed to connect to WiFi");
    }
}
```

## Phase 3: SignalR Connection Setup

### 3.1 Basic Connection

```cpp
#include "signalrclient/hub_connection_builder.h"
#include "signalrclient/esp32_websocket_client.h"
#include "signalrclient/esp32_http_client.h"

// Global connection object
signalr::hub_connection g_connection;

void init_signalr(const char* hub_url) {
    g_connection = signalr::hub_connection_builder::create(hub_url)
        .with_websocket_factory([](const signalr::signalr_client_config& config) {
            return std::make_shared<signalr::esp32_websocket_client>(config);
        })
        .with_http_client_factory([](const signalr::signalr_client_config& config) {
            return std::make_shared<signalr::esp32_http_client>(config);
        })
        .build();
}
```

### 3.2 Register Message Handlers

```cpp
void setup_message_handlers() {
    // Simple message handler
    g_connection.on("Notify", [](const std::vector<signalr::value>& args) {
        ESP_LOGI("SignalR", "Notification received");
    });

    // Handler with parameters
    g_connection.on("UpdateSensor", [](const std::vector<signalr::value>& args) {
        if (args.size() >= 2) {
            std::string sensor = args[0].as_string();
            double value = args[1].as_double();
            ESP_LOGI("SignalR", "Sensor %s = %.2f", sensor.c_str(), value);
        }
    });

    // JSON object handler
    g_connection.on("ConfigUpdate", [](const std::vector<signalr::value>& args) {
        if (args.size() >= 1 && args[0].is_map()) {
            auto config = args[0].as_map();
            for (const auto& pair : config) {
                ESP_LOGI("SignalR", "Config: %s = %s", 
                        pair.first.c_str(), 
                        pair.second.as_string().c_str());
            }
        }
    });
}
```

### 3.3 Start Connection

```cpp
void start_signalr() {
    g_connection.start([](std::exception_ptr exception) {
        if (exception) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::exception& e) {
                ESP_LOGE("SignalR", "Connection failed: %s", e.what());
                // Implement reconnection logic
            }
        } else {
            ESP_LOGI("SignalR", "Connected to SignalR hub!");
        }
    });
}
```

## Phase 4: Sending Messages

### 4.1 Simple Invoke

```cpp
void send_message(const char* user, const char* message) {
    std::vector<signalr::value> args;
    args.push_back(signalr::value(user));
    args.push_back(signalr::value(message));

    g_connection.invoke("SendMessage", args, [](const signalr::value& result, 
                                                 std::exception_ptr exception) {
        if (exception) {
            ESP_LOGE("SignalR", "Send failed");
        } else {
            ESP_LOGI("SignalR", "Message sent");
        }
    });
}
```

### 4.2 Send with Numeric Data

```cpp
void send_sensor_data(const char* sensor_id, float value) {
    std::vector<signalr::value> args;
    args.push_back(signalr::value(sensor_id));
    args.push_back(signalr::value(static_cast<double>(value)));

    g_connection.invoke("UpdateSensor", args);
}
```

### 4.3 Send Complex Objects

```cpp
void send_device_status() {
    std::map<std::string, signalr::value> status;
    status["deviceId"] = signalr::value("ESP32-001");
    status["temperature"] = signalr::value(25.5);
    status["humidity"] = signalr::value(60.0);
    status["online"] = signalr::value(true);

    std::vector<signalr::value> args;
    args.push_back(signalr::value(status));

    g_connection.invoke("ReportStatus", args);
}
```

## Phase 5: Error Handling and Reconnection

### 5.1 Connection State Monitoring

```cpp
void setup_connection_callbacks() {
    g_connection.set_disconnected([](std::exception_ptr exception) {
        ESP_LOGW("SignalR", "Disconnected");
        
        // Reconnect after delay
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        g_connection.start([](std::exception_ptr ex) {
            if (!ex) {
                ESP_LOGI("SignalR", "Reconnected!");
            }
        });
    });
}
```

### 5.2 Automatic Reconnection

```cpp
class SignalRManager {
private:
    signalr::hub_connection m_connection;
    bool m_should_reconnect;
    TaskHandle_t m_reconnect_task;

    static void reconnect_task(void* param) {
        auto* manager = static_cast<SignalRManager*>(param);
        
        while (manager->m_should_reconnect) {
            try {
                manager->m_connection.start([](std::exception_ptr ex) {
                    if (!ex) {
                        ESP_LOGI("SignalR", "Reconnected successfully");
                    }
                });
                
                // Wait before next attempt
                vTaskDelay(pdMS_TO_TICKS(5000));
            } catch (...) {
                ESP_LOGE("SignalR", "Reconnection attempt failed");
            }
        }
        
        vTaskDelete(NULL);
    }

public:
    void start_with_auto_reconnect() {
        m_should_reconnect = true;
        
        m_connection.set_disconnected([this](std::exception_ptr ex) {
            ESP_LOGW("SignalR", "Connection lost, scheduling reconnect");
            
            xTaskCreate(reconnect_task, "signalr_reconnect", 
                       4096, this, 5, &m_reconnect_task);
        });

        m_connection.start([](std::exception_ptr ex) {
            if (!ex) {
                ESP_LOGI("SignalR", "Initial connection successful");
            }
        });
    }
};
```

## Phase 6: Memory Management

### 6.1 Monitor Heap Usage

```cpp
void log_memory_usage() {
    ESP_LOGI("MEM", "Free heap: %d bytes", esp_get_free_heap_size());
    ESP_LOGI("MEM", "Min free heap: %d bytes", esp_get_minimum_free_heap_size());
}
```

### 6.2 Task Stack Sizing

```cpp
// Recommended stack sizes for SignalR tasks
#define SIGNALR_MAIN_TASK_STACK    8192   // For main connection handling
#define SIGNALR_CALLBACK_TASK_STACK 4096  // For message callbacks
```

## Best Practices

1. **Always check WiFi connection before starting SignalR**
2. **Implement automatic reconnection for production systems**
3. **Use task notifications for thread-safe communication**
4. **Monitor memory usage during development**
5. **Test with real network conditions (packet loss, latency)**
6. **Implement proper error handling for all callbacks**
7. **Keep message payloads small (< 4KB recommended)**

## Complete Example

See `example/main/signalr_example.cpp` for a fully working implementation.

## Troubleshooting

### Problem: Guru Meditation Error
**Cause**: Stack overflow in task  
**Solution**: Increase task stack size or reduce local variables

### Problem: JSON Parse Errors
**Cause**: Incomplete or malformed JSON  
**Solution**: Check server response format, increase buffer sizes

### Problem: Connection Timeouts
**Cause**: Network issues or server not responding  
**Solution**: Increase timeout values, check network configuration

### Problem: Memory Leaks
**Cause**: Not properly cleaning up connections  
**Solution**: Always call `.stop()` before destroying connection

## Next Steps

- Read the [Quick Start Guide](QUICKSTART.md)
- Setup a [test server](example/TEST_SERVER.md)
- Explore advanced features in the API documentation

## Support

For issues and questions:
- GitHub Issues: https://github.com/maker-community/esp-signalr/issues
