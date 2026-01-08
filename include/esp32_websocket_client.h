#pragma once

#include "websocket_client.h"
#include "transfer_format.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

namespace signalr {

// Forward declaration
struct signalr_client_config;

/**
 * ESP32 WebSocket client adapter
 * Wraps ESP-IDF esp_websocket_client to provide SignalR-compatible interface
 * Implements the websocket_client abstract interface
 * 
 * Key implementation notes:
 * - The official SignalR C++ client expects receive() to be called repeatedly
 *   in a loop, with each call waiting for exactly one message
 * - ESP32's esp_websocket_client uses an event-driven model
 * - This adapter bridges the two models using a message queue
 * - Callbacks are executed on a dedicated task to avoid stack overflow
 *   in the WebSocket event handler
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
    
    // Callback processor task - runs callbacks on dedicated task with larger stack
    static void callback_processor_task(void* param);
    void start_callback_processor();
    void stop_callback_processor();
    void schedule_callback_delivery();
    
    void handle_connected();
    void handle_disconnected();
    void handle_data(const char* data, int data_len);
    void handle_error(const char* error_msg);
    
    // Process pending receive request if message is available
    // Called from callback_processor_task, NOT from event handler
    void try_deliver_message();

    esp_websocket_client_handle_t m_client;
    EventGroupHandle_t m_event_group;
    
    // Callback processor task handle and semaphore
    TaskHandle_t m_callback_task;
    SemaphoreHandle_t m_callback_semaphore;
    volatile bool m_callback_task_running;

    // Limit concurrent per-message callback executor tasks to avoid exhaustion
    SemaphoreHandle_t m_callback_exec_limiter;
    
    // Message queue for bridging event-driven to callback model
    std::queue<std::string> m_message_queue;
    std::mutex m_queue_mutex;
    
    // Pending receive callback (set when receive() is called but no message is available)
    std::function<void(const std::string&, std::exception_ptr)> m_pending_receive_callback;
    std::mutex m_callback_mutex;

    bool m_is_connected;
    bool m_is_stopping;
    std::string m_receive_buffer;
    
    static constexpr int CONNECTED_BIT = BIT0;
    static constexpr int DISCONNECTED_BIT = BIT1;
    static constexpr int MESSAGE_RECEIVED_BIT = BIT2;
};

} // namespace signalr
