#include "esp32_websocket_client.h"
#include "signalr_client_config.h"
#include "esp_log.h"
#include <cstring>
#include <exception>
#include <stdexcept>

static const char* TAG = "ESP32_WS_CLIENT";

// Configuration constants
namespace {
    constexpr size_t WEBSOCKET_BUFFER_SIZE = 4096;
    constexpr size_t WEBSOCKET_TASK_STACK_SIZE = 8192;
    constexpr size_t CALLBACK_TASK_STACK_SIZE = 16384;  // Large stack for SignalR callbacks
    constexpr UBaseType_t CALLBACK_TASK_PRIORITY = 5;
    constexpr uint32_t CONNECTION_TIMEOUT_MS = 10000;
}

namespace signalr {

esp32_websocket_client::esp32_websocket_client(const signalr_client_config& config)
    : m_client(nullptr)
    , m_event_group(nullptr)
    , m_callback_task(nullptr)
    , m_callback_semaphore(nullptr)
    , m_callback_task_running(false)
    , m_is_connected(false)
    , m_is_stopping(false) {
    
    m_event_group = xEventGroupCreate();
    if (!m_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
    }
    
    m_callback_semaphore = xSemaphoreCreateBinary();
    if (!m_callback_semaphore) {
        ESP_LOGE(TAG, "Failed to create callback semaphore");
    }
}

esp32_websocket_client::~esp32_websocket_client() {
    stop_callback_processor();
    if (m_client) {
        stop([](std::exception_ptr) {});
    }
    if (m_event_group) {
        vEventGroupDelete(m_event_group);
    }
    if (m_callback_semaphore) {
        vSemaphoreDelete(m_callback_semaphore);
    }
}

void esp32_websocket_client::start(const std::string& url, std::function<void(std::exception_ptr)> callback) {
    ESP_LOGI(TAG, "Starting WebSocket connection to %s", url.c_str());
    
    if (m_client) {
        ESP_LOGW(TAG, "Client already exists, stopping first");
        stop([](std::exception_ptr) {});
    }

    m_is_stopping = false;
    xEventGroupClearBits(m_event_group, CONNECTED_BIT | DISCONNECTED_BIT | MESSAGE_RECEIVED_BIT);
    
    // Clear any pending state
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        while (!m_message_queue.empty()) {
            m_message_queue.pop();
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_pending_receive_callback = nullptr;
    }

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
        start_callback_processor();
        callback(nullptr);
    } else {
        ESP_LOGE(TAG, "Connection timeout or failed");
        callback(std::make_exception_ptr(std::runtime_error("Connection timeout")));
    }
}

void esp32_websocket_client::stop(std::function<void(std::exception_ptr)> callback) {
    ESP_LOGI(TAG, "Stopping websocket");
    m_is_stopping = true;
    
    // Stop callback processor first
    stop_callback_processor();
    
    // Notify any pending receive callback about disconnection
    // Use consistent lock order: queue_mutex -> callback_mutex
    std::function<void(const std::string&, std::exception_ptr)> pending_cb;
    {
        std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
        std::lock_guard<std::mutex> cb_lock(m_callback_mutex);
        if (m_pending_receive_callback) {
            pending_cb = m_pending_receive_callback;
            m_pending_receive_callback = nullptr;
        }
    }
    if (pending_cb) {
        // Signal message received to unblock any waiting task
        xEventGroupSetBits(m_event_group, MESSAGE_RECEIVED_BIT);
        pending_cb("", std::make_exception_ptr(std::runtime_error("WebSocket stopped")));
    }
    
    if (m_client) {
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

/**
 * receive() - Called by websocket_transport::receive_loop()
 * 
 * This method bridges ESP32's event-driven WebSocket model to SignalR's
 * callback-per-message model. The SignalR transport calls receive() and
 * expects the callback to be invoked with exactly ONE message, then it
 * calls receive() again for the next message (recursive loop).
 * 
 * Implementation:
 * 1. If there's already a message in the queue, deliver it immediately
 * 2. Otherwise, save the callback and wait for a message to arrive
 * 
 * Lock ordering: queue_mutex -> callback_mutex (must be consistent everywhere)
 */
void esp32_websocket_client::receive(std::function<void(const std::string&, std::exception_ptr)> callback) {
    ESP_LOGI(TAG, "receive() called");
    
    // Check if there's already a message available, use consistent lock order
    {
        std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
        if (!m_message_queue.empty()) {
            std::string message = m_message_queue.front();
            m_message_queue.pop();
            ESP_LOGI(TAG, "receive(): Delivering queued message immediately (%d bytes)", message.length());
            callback(message, nullptr);
            return;
        }
        
        // No message available, save callback for later (still holding queue_lock)
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_pending_receive_callback = callback;
    }
    
    ESP_LOGI(TAG, "receive(): No message available, callback saved, waiting...");
}

/**
 * try_deliver_message() - Called when a new message is added to the queue
 * 
 * If there's a pending receive callback, deliver the first message from
 * the queue to it.
 * 
 * IMPORTANT: We acquire locks in the same order as receive() to avoid deadlock:
 * 1. queue_mutex first
 * 2. callback_mutex second
 */
void esp32_websocket_client::try_deliver_message() {
    std::function<void(const std::string&, std::exception_ptr)> callback;
    std::string message;
    
    {
        // Acquire locks in same order as receive() to prevent deadlock
        std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
        std::lock_guard<std::mutex> cb_lock(m_callback_mutex);
        
        if (!m_pending_receive_callback) {
            ESP_LOGI(TAG, "try_deliver_message: No pending callback");
            return; // No pending callback, message stays in queue
        }
        
        if (m_message_queue.empty()) {
            ESP_LOGI(TAG, "try_deliver_message: Queue is empty");
            return; // No message to deliver
        }
        
        message = m_message_queue.front();
        m_message_queue.pop();
        callback = m_pending_receive_callback;
        m_pending_receive_callback = nullptr;
    }
    
    // Call callback OUTSIDE the lock to avoid potential deadlock with SignalR
    ESP_LOGI(TAG, "try_deliver_message: Delivering message to callback (%d bytes)", message.length());
    
    try {
        callback(message, nullptr);
        ESP_LOGI(TAG, "try_deliver_message: Callback returned successfully");
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "try_deliver_message: Callback threw exception: %s", e.what());
    } catch (...) {
        ESP_LOGE(TAG, "try_deliver_message: Callback threw unknown exception");
    }
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
            } else if (data->op_code == 0x0A) {  // Pong frame
                ESP_LOGD(TAG, "Received pong");
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
    
    // Notify pending receive callback about disconnection
    // Use consistent lock order: queue_mutex -> callback_mutex
    if (!m_is_stopping) {
        std::function<void(const std::string&, std::exception_ptr)> cb;
        {
            std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
            std::lock_guard<std::mutex> cb_lock(m_callback_mutex);
            if (m_pending_receive_callback) {
                cb = m_pending_receive_callback;
                m_pending_receive_callback = nullptr;
            }
        }
        if (cb) {
            cb("", std::make_exception_ptr(std::runtime_error("WebSocket disconnected")));
        }
    }
}

void esp32_websocket_client::handle_data(const char* data, int data_len) {
    if (!data || data_len <= 0) {
        return;
    }

    // Accumulate fragmented messages
    m_receive_buffer.append(data, data_len);
    
    // Check for SignalR record separator (0x1E)
    size_t separator_pos = m_receive_buffer.find('\x1e');
    while (separator_pos != std::string::npos) {
        std::string message = m_receive_buffer.substr(0, separator_pos);
        m_receive_buffer.erase(0, separator_pos + 1);
        
        ESP_LOGI(TAG, "Received complete message: %d bytes: %s", message.length(), message.c_str());
        
        // Add message to queue
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            m_message_queue.push(message);
        }
        
        // Signal callback processor task to deliver message
        // DON'T call try_deliver_message() directly - it would run SignalR
        // callbacks on the WebSocket event handler thread with limited stack
        schedule_callback_delivery();
        
        separator_pos = m_receive_buffer.find('\x1e');
    }
}

void esp32_websocket_client::handle_error(const char* error_msg) {
    if (error_msg && !m_is_stopping) {
        // Use consistent lock order: queue_mutex -> callback_mutex
        std::function<void(const std::string&, std::exception_ptr)> cb;
        {
            std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
            std::lock_guard<std::mutex> cb_lock(m_callback_mutex);
            if (m_pending_receive_callback) {
                cb = m_pending_receive_callback;
                m_pending_receive_callback = nullptr;
            }
        }
        if (cb) {
            cb("", std::make_exception_ptr(std::runtime_error(error_msg)));
        }
    }
}

// ============================================================================
// Callback Processor Task
// ============================================================================
// SignalR callbacks involve heavy processing (JSON parsing, state management)
// and can trigger recursive calls. Running them directly on the WebSocket
// event handler thread causes stack overflow. This task provides a dedicated
// thread with sufficient stack for callback execution.

void esp32_websocket_client::callback_processor_task(void* param) {
    auto* client = static_cast<esp32_websocket_client*>(param);
    ESP_LOGI(TAG, "Callback processor task started");
    
    while (client->m_callback_task_running) {
        // Wait for signal that a message is ready
        if (xSemaphoreTake(client->m_callback_semaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Process all available messages
            while (client->m_callback_task_running) {
                client->try_deliver_message();
                
                // Check if there are more messages
                bool has_more = false;
                {
                    std::lock_guard<std::mutex> lock(client->m_queue_mutex);
                    std::lock_guard<std::mutex> cb_lock(client->m_callback_mutex);
                    has_more = !client->m_message_queue.empty() && 
                               client->m_pending_receive_callback != nullptr;
                }
                if (!has_more) {
                    break;
                }
            }
        }
    }
    
    ESP_LOGI(TAG, "Callback processor task exiting");
    vTaskDelete(NULL);
}

void esp32_websocket_client::start_callback_processor() {
    if (m_callback_task != nullptr) {
        return; // Already running
    }
    
    m_callback_task_running = true;
    
    BaseType_t result = xTaskCreate(
        callback_processor_task,
        "signalr_cb",
        CALLBACK_TASK_STACK_SIZE,
        this,
        CALLBACK_TASK_PRIORITY,
        &m_callback_task
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create callback processor task");
        m_callback_task = nullptr;
        m_callback_task_running = false;
    } else {
        ESP_LOGI(TAG, "Callback processor task created");
    }
}

void esp32_websocket_client::stop_callback_processor() {
    if (m_callback_task == nullptr) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping callback processor task");
    m_callback_task_running = false;
    
    // Signal the task to wake up and exit
    xSemaphoreGive(m_callback_semaphore);
    
    // Wait for task to exit (with timeout)
    for (int i = 0; i < 50 && m_callback_task != nullptr; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    
    m_callback_task = nullptr;
}

void esp32_websocket_client::schedule_callback_delivery() {
    if (m_callback_semaphore != nullptr) {
        xSemaphoreGive(m_callback_semaphore);
    }
}

} // namespace signalr
