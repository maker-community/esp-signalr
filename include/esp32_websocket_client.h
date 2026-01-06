#pragma once

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
 */
class esp32_websocket_client {
public:
    using message_callback = std::function<void(const std::string&)>;
    using error_callback = std::function<void(const std::string&)>;
    using connected_callback = std::function<void()>;
    using disconnected_callback = std::function<void()>;

    explicit esp32_websocket_client(const signalr_client_config& config);
    ~esp32_websocket_client();

    // Connection management
    void connect(const std::string& url);
    void disconnect();
    bool is_connected() const;

    // Send data
    void send(const std::string& message);

    // Callback registration
    void set_message_received_callback(message_callback callback);
    void set_error_callback(error_callback callback);
    void set_connected_callback(connected_callback callback);
    void set_disconnected_callback(disconnected_callback callback);

private:
    static void websocket_event_handler(void* handler_args, esp_event_base_t base, 
                                       int32_t event_id, void* event_data);
    
    void handle_connected();
    void handle_disconnected();
    void handle_data(const char* data, int data_len);
    void handle_error(const char* error_msg);

    esp_websocket_client_handle_t m_client;
    EventGroupHandle_t m_event_group;
    
    message_callback m_message_callback;
    error_callback m_error_callback;
    connected_callback m_connected_callback;
    disconnected_callback m_disconnected_callback;

    bool m_is_connected;
    std::string m_receive_buffer;
    
    static constexpr int CONNECTED_BIT = BIT0;
    static constexpr int DISCONNECTED_BIT = BIT1;
};

} // namespace signalr
