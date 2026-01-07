#pragma once

#include "websocket_client.h"
#include "transfer_format.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string>
#include <functional>
#include <memory>

namespace signalr {

// Forward declaration
struct signalr_client_config;

/**
 * ESP32 WebSocket client adapter
 * Wraps ESP-IDF esp_websocket_client to provide SignalR-compatible interface
 * Implements the websocket_client abstract interface
 */
class esp32_websocket_client : public websocket_client {
public:
    explicit esp32_websocket_client(const signalr_client_config& config);
    virtual ~esp32_websocket_client();

    // Implement websocket_client interface
    void start(const std::string& url, std::function<void(std::exception_ptr)> callback) override;
    void stop(std::function<void(std::exception_ptr)> callback) override;
    void send(const std::string& payload, transfer_format transfer_format, 
             std::function<void(std::exception_ptr)> callback) override;
    void receive(std::function<void(const std::string&, std::exception_ptr)> callback) override;

private:
    static void websocket_event_handler(void* handler_args, esp_event_base_t base, 
                                       int32_t event_id, void* event_data);
    
    void handle_connected();
    void handle_disconnected();
    void handle_data(const char* data, int data_len);
    void handle_error(const char* error_msg);

    esp_websocket_client_handle_t m_client;
    EventGroupHandle_t m_event_group;
    
    std::function<void(const std::string&, std::exception_ptr)> m_receive_callback;

    bool m_is_connected;
    bool m_is_stopping;
    std::string m_receive_buffer;
    
    static constexpr int CONNECTED_BIT = BIT0;
    static constexpr int DISCONNECTED_BIT = BIT1;
};

} // namespace signalr
