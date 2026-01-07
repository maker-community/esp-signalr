#include "esp32_websocket_client.h"
#include "signalr_client_config.h"
#include "esp_log.h"
#include <cstring>
#include <exception>
#include <stdexcept>

static const char* TAG = "ESP32_WS_CLIENT";

// Configuration constants
namespace {
    constexpr size_t WEBSOCKET_BUFFER_SIZE = 2048;
    constexpr size_t WEBSOCKET_TASK_STACK_SIZE = 6144;
    constexpr uint32_t CONNECTION_TIMEOUT_MS = 10000;
}

namespace signalr {

esp32_websocket_client::esp32_websocket_client(const signalr_client_config& config)
    : m_client(nullptr)
    , m_event_group(nullptr)
    , m_is_connected(false)
    , m_is_stopping(false) {
    
    m_event_group = xEventGroupCreate();
    if (!m_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
    }
}

esp32_websocket_client::~esp32_websocket_client() {
    if (m_client) {
        stop([](std::exception_ptr) {});
    }
    if (m_event_group) {
        vEventGroupDelete(m_event_group);
    }
}

void esp32_websocket_client::start(const std::string& url, std::function<void(std::exception_ptr)> callback) {
    ESP_LOGI(TAG, "Starting WebSocket connection to %s", url.c_str());
    
    if (m_client) {
        ESP_LOGW(TAG, "Client already exists, stopping first");
        stop([](std::exception_ptr) {});
    }

    m_is_stopping = false;
    xEventGroupClearBits(m_event_group, CONNECTED_BIT | DISCONNECTED_BIT);

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = url.c_str();
    ws_cfg.buffer_size = WEBSOCKET_BUFFER_SIZE;
    ws_cfg.task_stack = WEBSOCKET_TASK_STACK_SIZE;

    m_client = esp_websocket_client_init(&ws_cfg);
    if (!m_client) {
        ESP_LOGE(TAG, "Failed to create websocket client");
        callback(std::make_exception_ptr(std::runtime_error("Failed to create websocket client")));
        return;
    }

    esp_websocket_register_events(m_client, WEBSOCKET_EVENT_ANY, 
                                  websocket_event_handler, this);

    esp_err_t err = esp_websocket_client_start(m_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket client start failed: %s", esp_err_to_name(err));
        callback(std::make_exception_ptr(std::runtime_error("WebSocket client start failed")));
        esp_websocket_client_destroy(m_client);
        m_client = nullptr;
        return;
    }

    // Wait for connection with timeout
    EventBits_t bits = xEventGroupWaitBits(m_event_group,
                                          CONNECTED_BIT | DISCONNECTED_BIT,
                                          pdFALSE, pdFALSE,
                                          pdMS_TO_TICKS(CONNECTION_TIMEOUT_MS));

    if (bits & CONNECTED_BIT) {
        ESP_LOGI(TAG, "WebSocket connected successfully");
        callback(nullptr);
    } else {
        ESP_LOGE(TAG, "Connection timeout or failed");
        callback(std::make_exception_ptr(std::runtime_error("Connection timeout")));
    }
}

void esp32_websocket_client::stop(std::function<void(std::exception_ptr)> callback) {
    if (m_client) {
        ESP_LOGI(TAG, "Stopping websocket");
        m_is_stopping = true;
        esp_websocket_client_close(m_client, portMAX_DELAY);
        esp_websocket_client_stop(m_client);
        esp_websocket_client_destroy(m_client);
        m_client = nullptr;
    }
    m_is_connected = false;
    xEventGroupClearBits(m_event_group, CONNECTED_BIT);
    callback(nullptr);
}

void esp32_websocket_client::send(const std::string& payload, transfer_format transfer_format,
                                  std::function<void(std::exception_ptr)> callback) {
    if (!m_client || !m_is_connected) {
        ESP_LOGE(TAG, "Cannot send: not connected");
        callback(std::make_exception_ptr(std::runtime_error("Not connected")));
        return;
    }

    int sent = esp_websocket_client_send_text(m_client, payload.c_str(), 
                                              payload.length(), portMAX_DELAY);
    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send message");
        callback(std::make_exception_ptr(std::runtime_error("Failed to send message")));
    } else {
        ESP_LOGD(TAG, "Sent %d bytes", sent);
        callback(nullptr);
    }
}

void esp32_websocket_client::receive(std::function<void(const std::string&, std::exception_ptr)> callback) {
    m_receive_callback = callback;
}

void esp32_websocket_client::websocket_event_handler(void* handler_args, 
                                                     esp_event_base_t base,
                                                     int32_t event_id, 
                                                     void* event_data) {
    auto* client = static_cast<esp32_websocket_client*>(handler_args);
    esp_websocket_event_data_t* data = static_cast<esp_websocket_event_data_t*>(event_data);

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            client->handle_connected();
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WebSocket disconnected");
            client->handle_disconnected();
            break;

        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 0x01) {  // Text frame
                client->handle_data(data->data_ptr, data->data_len);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            client->handle_error("WebSocket error occurred");
            break;

        default:
            break;
    }
}

void esp32_websocket_client::handle_connected() {
    m_is_connected = true;
    xEventGroupSetBits(m_event_group, CONNECTED_BIT);
}

void esp32_websocket_client::handle_disconnected() {
    m_is_connected = false;
    xEventGroupSetBits(m_event_group, DISCONNECTED_BIT);
    xEventGroupClearBits(m_event_group, CONNECTED_BIT);
}

void esp32_websocket_client::handle_data(const char* data, int data_len) {
    if (!data || data_len <= 0) {
        return;
    }

    // Accumulate fragmented messages
    m_receive_buffer.append(data, data_len);
    
    // Check for SignalR record separator
    size_t separator_pos = m_receive_buffer.find('\x1e');
    while (separator_pos != std::string::npos) {
        std::string message = m_receive_buffer.substr(0, separator_pos);
        m_receive_buffer.erase(0, separator_pos + 1);
        
        ESP_LOGI(TAG, "Received complete message: %d bytes: %s", message.length(), message.c_str());
        
        // Invoke the receive callback if set
        if (m_receive_callback) {
            m_receive_callback(message, nullptr);
        }
        
        separator_pos = m_receive_buffer.find('\x1e');
    }
}

void esp32_websocket_client::handle_error(const char* error_msg) {
    if (m_receive_callback && error_msg && !m_is_stopping) {
        m_receive_callback("", std::make_exception_ptr(std::runtime_error(error_msg)));
    }
}

} // namespace signalr
