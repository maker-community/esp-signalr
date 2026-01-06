#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "signalrclient/hub_connection.h"
#include "signalrclient/hub_connection_builder.h"
#include "signalrclient/signalr_value.h"
#include "signalrclient/esp32_websocket_client.h"
#include "signalrclient/esp32_http_client.h"

static const char *TAG = "SIGNALR_EXAMPLE";

// WiFi configuration - CHANGE THESE!
#define WIFI_SSID      "YOUR_SSID"
#define WIFI_PASSWORD  "YOUR_PASSWORD"

// SignalR server configuration - CHANGE THIS!
#define SIGNALR_HUB_URL "http://your-server.com:5000/chatHub"

// WiFi event group
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Retry connecting to WiFi...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Initialize WiFi
void wifi_init_sta(void)
{
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
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 SignalR Client Example");
    ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize WiFi
    wifi_init_sta();

    ESP_LOGI(TAG, "Creating SignalR connection to: %s", SIGNALR_HUB_URL);
    ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());

    try {
        // Create SignalR hub connection
        auto connection = signalr::hub_connection_builder::create(SIGNALR_HUB_URL)
            .with_websocket_factory([](const signalr::signalr_client_config& config) {
                return std::make_shared<signalr::esp32_websocket_client>(config);
            })
            .with_http_client_factory([](const signalr::signalr_client_config& config) {
                return std::make_shared<signalr::esp32_http_client>(config);
            })
            .build();

        ESP_LOGI(TAG, "SignalR connection object created");

        // Register message handler for "ReceiveMessage"
        connection.on("ReceiveMessage", [](const std::vector<signalr::value>& args) {
            if (args.size() >= 2) {
                ESP_LOGI(TAG, "Message from %s: %s", 
                         args[0].as_string().c_str(),
                         args[1].as_string().c_str());
            } else {
                ESP_LOGI(TAG, "Received message with %d arguments", args.size());
            }
        });

        ESP_LOGI(TAG, "Message handlers registered");

        // Start the connection
        ESP_LOGI(TAG, "Starting SignalR connection...");
        connection.start([](std::exception_ptr exception) {
            if (exception) {
                try {
                    std::rethrow_exception(exception);
                } catch (const std::exception& e) {
                    ESP_LOGE(TAG, "Connection failed: %s", e.what());
                    return;
                }
            }
            ESP_LOGI(TAG, "Connected to SignalR hub successfully!");
        });

        // Wait for connection to establish
        vTaskDelay(pdMS_TO_TICKS(3000));

        ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());

        // Send a test message
        ESP_LOGI(TAG, "Sending test message...");
        
        std::vector<signalr::value> args;
        args.push_back(signalr::value("ESP32"));
        args.push_back(signalr::value("Hello from ESP32 SignalR client!"));
        
        connection.invoke("SendMessage", args, [](const signalr::value& result, 
                                                   std::exception_ptr exception) {
            if (exception) {
                try {
                    std::rethrow_exception(exception);
                } catch (const std::exception& e) {
                    ESP_LOGE(TAG, "Invoke failed: %s", e.what());
                }
            } else {
                ESP_LOGI(TAG, "Message sent successfully");
            }
        });

        // Keep running and periodically send messages
        int message_count = 0;
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(10000));  // Wait 10 seconds
            
            message_count++;
            ESP_LOGI(TAG, "Sending message #%d", message_count);
            ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());
            
            std::vector<signalr::value> periodic_args;
            periodic_args.push_back(signalr::value("ESP32"));
            
            char msg_buf[64];
            snprintf(msg_buf, sizeof(msg_buf), "Periodic message #%d", message_count);
            periodic_args.push_back(signalr::value(msg_buf));
            
            connection.invoke("SendMessage", periodic_args);
        }

    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception in main: %s", e.what());
    } catch (...) {
        ESP_LOGE(TAG, "Unknown exception in main");
    }

    ESP_LOGI(TAG, "Example finished");
}
