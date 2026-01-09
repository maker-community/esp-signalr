#include "esp32_websocket_client.h"
#include "signalr_client_config.h"
#include "esp_log.h"
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>

static const char* TAG = "ESP32_WS_CLIENT";

// Pre-created exception objects to avoid throw-catch overhead in send()
// These are created once at startup and reused throughout the lifetime
namespace {
    // Helper function to safely create exception_ptr once
    std::exception_ptr get_not_connected_exception() {
        static std::exception_ptr ex = []() {
            try {
                throw std::runtime_error("Not connected");
            } catch (...) {
                return std::current_exception();
            }
        }();
        return ex;
    }
    
    std::exception_ptr get_send_failed_exception() {
        static std::exception_ptr ex = []() {
            try {
                throw std::runtime_error("Failed to send message");
            } catch (...) {
                return std::current_exception();
            }
        }();
        return ex;
    }
    
    std::exception_ptr get_unknown_error_exception() {
        static std::exception_ptr ex = []() {
            try {
                throw std::runtime_error("Unknown error in send");
            } catch (...) {
                return std::current_exception();
            }
        }();
        return ex;
    }
    
    std::exception_ptr get_client_creation_failed_exception() {
        static std::exception_ptr ex = []() {
            try {
                throw std::runtime_error("Failed to create websocket client");
            } catch (...) {
                return std::current_exception();
            }
        }();
        return ex;
    }
    
    std::exception_ptr get_client_start_failed_exception() {
        static std::exception_ptr ex = []() {
            try {
                throw std::runtime_error("WebSocket client start failed");
            } catch (...) {
                return std::current_exception();
            }
        }();
        return ex;
    }
    
    std::exception_ptr get_connection_timeout_exception() {
        static std::exception_ptr ex = []() {
            try {
                throw std::runtime_error("Connection timeout");
            } catch (...) {
                return std::current_exception();
            }
        }();
        return ex;
    }
    
    std::exception_ptr get_websocket_stopped_exception() {
        static std::exception_ptr ex = []() {
            try {
                throw std::runtime_error("WebSocket stopped");
            } catch (...) {
                return std::current_exception();
            }
        }();
        return ex;
    }
    
    std::exception_ptr get_websocket_disconnected_exception() {
        static std::exception_ptr ex = []() {
            try {
                throw std::runtime_error("WebSocket disconnected");
            } catch (...) {
                return std::current_exception();
            }
        }();
        return ex;
    }
    
    std::exception_ptr get_task_creation_failed_exception() {
        static std::exception_ptr ex = []() {
            try {
                throw std::runtime_error("Task creation failed");
            } catch (...) {
                return std::current_exception();
            }
        }();
        return ex;
    }
}

// Configuration constants - OPTIMIZED FOR ESP32 MEMORY CONSTRAINTS
namespace {
    // Structure to pass callback data to FreeRTOS task
    struct callback_payload {
        std::string msg;
        std::function<void(const std::string&, std::exception_ptr)> cb;
        SemaphoreHandle_t limiter;
    };
    // Reduced from 4096 to 2048 - SignalR messages are typically small
    constexpr size_t WEBSOCKET_BUFFER_SIZE = 2048;
    // Increased to 8192 - ESP32 WebSocket library needs more stack during reconnection
    // This prevents stack overflow during SSL handshake and error handling
    constexpr size_t WEBSOCKET_TASK_STACK_SIZE = 8192;
#ifdef CONFIG_SIGNALR_CALLBACK_STACK_SIZE
    constexpr size_t CALLBACK_TASK_STACK_SIZE = CONFIG_SIGNALR_CALLBACK_STACK_SIZE;
#else
    // 6144 bytes (6KB) - Critical for preventing stack overflow during reconnection
    // Increased from 5120: C++ exception unwinding + std::function + JSON parsing needs more stack
    constexpr size_t CALLBACK_TASK_STACK_SIZE = 6144;
#endif
    constexpr UBaseType_t CALLBACK_TASK_PRIORITY = 5;
    constexpr uint32_t CONNECTION_TIMEOUT_MS = 10000;
#ifdef CONFIG_SIGNALR_MAX_QUEUE_SIZE
    constexpr size_t MAX_MESSAGE_QUEUE_SIZE = CONFIG_SIGNALR_MAX_QUEUE_SIZE;
#else
    // Reduced from 50 to 20 - prevents excessive memory usage
    constexpr size_t MAX_MESSAGE_QUEUE_SIZE = 20;
#endif

#ifdef CONFIG_SIGNALR_MAX_CALLBACK_TASKS
    constexpr UBaseType_t MAX_CALLBACK_EXEC_TASKS = CONFIG_SIGNALR_MAX_CALLBACK_TASKS;
#else
    // Increased to 3 - reduces chance of falling back to inline execution (which causes stack overflow)
    constexpr UBaseType_t MAX_CALLBACK_EXEC_TASKS = 3;
#endif
    
    // Connection retry parameters
    constexpr uint32_t INITIAL_RETRY_DELAY_MS = 1000;
    constexpr uint32_t MAX_RETRY_DELAY_MS = 30000;
    constexpr float RETRY_BACKOFF_MULTIPLIER = 2.0f;
}

namespace signalr {

esp32_websocket_client::esp32_websocket_client(const signalr_client_config& config)
    : m_client(nullptr)
    , m_event_group(nullptr)
    , m_callback_task(nullptr)
    , m_callback_semaphore(nullptr)
    , m_callback_task_running(false)
    , m_callback_exec_limiter(nullptr)
    , m_is_connected(false)
    , m_is_stopping(false) {
    
    m_event_group = xEventGroupCreate();
    if (!m_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
    }
    
    // Counting semaphore to avoid losing wakeups when many frames arrive quickly
    m_callback_semaphore = xSemaphoreCreateCounting(MAX_MESSAGE_QUEUE_SIZE, 0);
    if (!m_callback_semaphore) {
        ESP_LOGE(TAG, "Failed to create callback semaphore");
    }

    m_callback_exec_limiter = xSemaphoreCreateCounting(MAX_CALLBACK_EXEC_TASKS, MAX_CALLBACK_EXEC_TASKS);
    if (!m_callback_exec_limiter) {
        ESP_LOGE(TAG, "Failed to create callback exec limiter semaphore");
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
    if (m_callback_exec_limiter) {
        vSemaphoreDelete(m_callback_exec_limiter);
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
    // Set network timeout to prevent premature disconnection
    // This should be longer than the SignalR server_timeout to allow SignalR-level timeout handling
    ws_cfg.network_timeout_ms = 120000;  // 120 seconds (2x the SignalR server_timeout of 60s)
    // Enable auto-reconnect with exponential backoff
    ws_cfg.reconnect_timeout_ms = INITIAL_RETRY_DELAY_MS;
    // Disable automatic ping to save bandwidth (SignalR has its own keepalive)
    ws_cfg.ping_interval_sec = 0;

    m_client = esp_websocket_client_init(&ws_cfg);
    if (!m_client) {
        ESP_LOGE(TAG, "Failed to create websocket client");
        callback(get_client_creation_failed_exception());
        return;
    }

    esp_websocket_register_events(m_client, WEBSOCKET_EVENT_ANY, 
                                  websocket_event_handler, this);

    esp_err_t err = esp_websocket_client_start(m_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket client start failed: %s", esp_err_to_name(err));
        callback(get_client_start_failed_exception());
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
        callback(get_connection_timeout_exception());
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
        pending_cb("", get_websocket_stopped_exception());
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
    // OPTIMIZED: Use pre-created exceptions to avoid throw-catch overhead
    // No exceptions are thrown at runtime - zero stack unwinding cost!
    
    // Quick path: check connection status without any exception overhead
    if (!m_client || !m_is_connected) {
        ESP_LOGW(TAG, "Cannot send: not connected (payload size: %d bytes)", payload.size());
        // Use pre-created exception - no throw/catch, no stack overhead!
        callback(get_not_connected_exception());
        return;
    }

    // Try to send - wrapped in try-catch only for ESP-IDF API safety
    try {
        int sent = esp_websocket_client_send_text(m_client, payload.c_str(), 
                                                  payload.length(), portMAX_DELAY);
        if (sent < 0) {
            ESP_LOGE(TAG, "Failed to send message (returned: %d)", sent);
            // Use pre-created exception - no throw/catch!
            callback(get_send_failed_exception());
        } else {
            ESP_LOGD(TAG, "Sent %d bytes", sent);
            callback(nullptr);
        }
    }
    catch (const std::exception& e) {
        // This should rarely happen - only if ESP-IDF API throws
        ESP_LOGE(TAG, "Unexpected exception in send(): %s", e.what());
        try {
            callback(std::current_exception());
        } catch (...) {
            // If callback throws, use pre-created exception
            ESP_LOGE(TAG, "CRITICAL: Callback threw exception, using fallback error");
            callback(get_unknown_error_exception());
        }
    }
    catch (...) {
        // Catch-all safety net
        ESP_LOGE(TAG, "Unknown exception in send()");
        callback(get_unknown_error_exception());
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
 * Always save the callback and let the callback processor handle delivery.
 * This avoids deep recursion when receive() is called from within a callback.
 * 
 * Lock ordering: queue_mutex -> callback_mutex (must be consistent everywhere)
 */
void esp32_websocket_client::receive(std::function<void(const std::string&, std::exception_ptr)> callback) {
    ESP_LOGD(TAG, "receive() called");
    
    bool has_message = false;
    {
        std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
        has_message = !m_message_queue.empty();
        
        // Always save callback - callback processor will handle delivery
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_pending_receive_callback = callback;
    }
    
    if (has_message) {
        schedule_callback_delivery();
    }
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
            ESP_LOGW(TAG, "try_deliver_message: No pending callback, queue size: %d", m_message_queue.size());
            return; // No pending callback, message stays in queue
        }
        
        if (m_message_queue.empty()) {
            ESP_LOGW(TAG, "try_deliver_message: Queue is empty but callback is pending");
            return; // No message to deliver
        }
        
        message = std::move(m_message_queue.front());  // Use move to avoid copy
        m_message_queue.pop();
        callback = m_pending_receive_callback;
        m_pending_receive_callback = nullptr;
        ESP_LOGD(TAG, "Deliver: %d bytes, queue: %zu", message.length(), m_message_queue.size());
    }
    
    // Prepare payload for callback
    auto* payload = new callback_payload{std::move(message), callback, m_callback_exec_limiter};
    
    auto task = [](void* arg)
    {
        std::unique_ptr<callback_payload> payload(static_cast<callback_payload*>(arg));
        try
        {
            payload->cb(payload->msg, nullptr);
        }
        catch (const std::exception& e)
        {
            ESP_LOGE(TAG, "try_deliver_message: Callback threw exception: %s", e.what());
        }
        catch (...)
        {
            ESP_LOGE(TAG, "try_deliver_message: Callback threw unknown exception");
        }

        if (payload->limiter)
        {
            xSemaphoreGive(payload->limiter);
        }

        vTaskDelete(nullptr);
    };

    bool scheduled = false;
    if (m_callback_exec_limiter && xSemaphoreTake(m_callback_exec_limiter, 0) == pdTRUE)
    {
        if (xTaskCreate(task, "signalr_cb_exec", CALLBACK_TASK_STACK_SIZE, payload, CALLBACK_TASK_PRIORITY, nullptr) == pdPASS)
        {
            scheduled = true;
        }
        else
        {
            // task creation failed; release permit
            xSemaphoreGive(m_callback_exec_limiter);
        }
    }

    if (!scheduled)
    {
        // CRITICAL: DO NOT execute callback inline - causes stack overflow!
        // When task creation fails, we must drop the message to prevent pthread stack overflow
        ESP_LOGE(TAG, "CRITICAL: Task creation failed, dropping message to prevent stack overflow!");
        ESP_LOGE(TAG, "         Message size: %d bytes, callback ptr: %p", payload->msg.size(), (void*)&payload->cb);
        
        // Try to notify caller about the failure (safe exception path)
        try {
            payload->cb("", get_task_creation_failed_exception());
        } catch (...) {
            // Ignore any exception during error notification
            ESP_LOGE(TAG, "Failed to notify callback about task failure");
        }
        
        delete payload; // Clean up
        
        // Consider this a critical error - might need to reduce load
        ESP_LOGW(TAG, "Consider increasing CONFIG_SIGNALR_MAX_CALLBACK_TASKS or reducing message rate");
    }
}

// WebSocket event handler
void esp32_websocket_client::websocket_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
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
            cb("", get_websocket_disconnected_exception());
        }
    }
}

void esp32_websocket_client::handle_data(const char* data, int data_len) {
    if (!data || data_len <= 0) {
        return;
    }

    // Optimized: Reserve space to reduce reallocations (xiaozhi-inspired)
    if (m_receive_buffer.capacity() < m_receive_buffer.size() + data_len) {
        m_receive_buffer.reserve(m_receive_buffer.size() + data_len + 512);
    }
    m_receive_buffer.append(data, data_len);
    
    // Check for SignalR record separator (0x1E)
    size_t separator_pos = m_receive_buffer.find('\x1e');
    while (separator_pos != std::string::npos) {
        // Extract message and update buffer in one operation to reduce allocations
        std::string message;
        message.reserve(separator_pos); // Pre-allocate to avoid reallocation
        message.assign(m_receive_buffer, 0, separator_pos);
        m_receive_buffer.erase(0, separator_pos + 1);
        
        // Reduced logging: Only log message length, not content (saves memory)
        ESP_LOGD(TAG, "RX msg: %d bytes", message.length());
        
        // Add message to queue (with overflow protection)
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            if (m_message_queue.size() < MAX_MESSAGE_QUEUE_SIZE) {
                m_message_queue.push(std::move(message));  // Use move instead of copy
                ESP_LOGD(TAG, "Queue size: %zu", m_message_queue.size());
            } else {
                ESP_LOGW(TAG, "Queue full, drop oldest");
                m_message_queue.pop();
                m_message_queue.push(std::move(message));
            }
        }
        
        // Signal callback processor task to deliver message
        schedule_callback_delivery();
        
        separator_pos = m_receive_buffer.find('\x1e');
    }
    
    // Optimized: Shrink buffer if it grows too large (xiaozhi pattern)
    if (m_receive_buffer.capacity() > 8192 && m_receive_buffer.size() < 1024) {
        m_receive_buffer.shrink_to_fit();
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
            // For dynamic error messages, we must use make_exception_ptr
            // This is rare (only on actual errors), so the overhead is acceptable
            try {
                cb("", std::make_exception_ptr(std::runtime_error(error_msg)));
            } catch (...) {
                // Fallback: use pre-created generic exception
                ESP_LOGE(TAG, "Exception during error callback, using fallback");
                try {
                    cb("", get_unknown_error_exception());
                } catch (...) {
                    ESP_LOGE(TAG, "Failed to deliver error callback");
                }
            }
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
    
    // Always monitor stack to detect issues early
    UBaseType_t high_water_mark_start = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "Callback task stack: %u bytes allocated, %u bytes free initially", 
             CALLBACK_TASK_STACK_SIZE, high_water_mark_start * sizeof(StackType_t));
    
    // Log initial heap state
    ESP_LOGI(TAG, "Heap: free=%u bytes, min_free=%u bytes",
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    
    ESP_LOGI(TAG, "Callback processor: entering main loop");
    while (client->m_callback_task_running) {
        // Wait for signal that a message is ready
        if (xSemaphoreTake(client->m_callback_semaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGD(TAG, "Callback processor: got semaphore, processing messages");
            
            // Keep processing while there are messages OR a pending callback
            // The loop continues as long as there's work to potentially do
            int message_count = 0;
            int idle_rounds = 0;
            const int MAX_IDLE_ROUNDS = 50; // 50 * 10ms = 500ms max idle
            
            // Periodic stack monitoring (every 10 messages)
            UBaseType_t last_stack_check = 0;
            
            while (client->m_callback_task_running && idle_rounds < MAX_IDLE_ROUNDS) {
                // Check stack usage every 10 messages
                if (message_count > 0 && message_count % 10 == 0) {
                    UBaseType_t stack_free = uxTaskGetStackHighWaterMark(NULL);
                    if (stack_free != last_stack_check) {
                        ESP_LOGD(TAG, "Stack monitor: %u bytes free (used: %u bytes)",
                                 stack_free * sizeof(StackType_t),
                                 CALLBACK_TASK_STACK_SIZE - stack_free * sizeof(StackType_t));
                        if (stack_free * sizeof(StackType_t) < 1024) {
                            ESP_LOGW(TAG, "WARNING: Low stack! Only %u bytes free", stack_free * sizeof(StackType_t));
                        }
                        last_stack_check = stack_free;
                    }
                }
                ESP_LOGD(TAG, "Callback processor: Round %d, calling try_deliver_message", message_count + 1);
                bool has_messages = false;
                bool has_callback = false;
                
                // Check state
                {
                    std::lock_guard<std::mutex> lock(client->m_queue_mutex);
                    has_messages = !client->m_message_queue.empty();
                }
                {
                    std::lock_guard<std::mutex> cb_lock(client->m_callback_mutex);
                    has_callback = client->m_pending_receive_callback != nullptr;
                }
                
                // If we have both message and callback, deliver
                if (has_messages && has_callback) {
                    message_count++;
                    ESP_LOGI(TAG, "Callback processor: calling try_deliver_message #%d", message_count);
                    client->try_deliver_message();
                    idle_rounds = 0; // Reset idle counter on successful delivery
                    
                    // Give SignalR time to process and call receive() again
                    vTaskDelay(pdMS_TO_TICKS(5));
                } else if (has_messages && !has_callback) {
                    // Messages waiting but no callback yet - wait for receive() to be called
                    idle_rounds++;
                    vTaskDelay(pdMS_TO_TICKS(10));
                } else {
                    // No messages - exit the loop
                    break;
                }
            }
            
            if (idle_rounds >= MAX_IDLE_ROUNDS) {
                ESP_LOGW(TAG, "Callback processor: timeout waiting for callback, %d messages queued", 
                         (int)client->m_message_queue.size());
            } else if (message_count > 0) {
                ESP_LOGI(TAG, "Callback processor: processed %d messages this round", message_count);
            }
        }
    }
    
    // Always log final stack statistics
    UBaseType_t high_water_mark_end = uxTaskGetStackHighWaterMark(NULL);
    size_t stack_used = CALLBACK_TASK_STACK_SIZE - (high_water_mark_end * sizeof(StackType_t));
    ESP_LOGI(TAG, "Callback task final statistics:");
    ESP_LOGI(TAG, "  Stack: %u bytes allocated, %u bytes used (%.1f%%), %u bytes free (min)",
             CALLBACK_TASK_STACK_SIZE, stack_used,
             (stack_used * 100.0f) / CALLBACK_TASK_STACK_SIZE,
             high_water_mark_end * sizeof(StackType_t));
    ESP_LOGI(TAG, "  Heap: free=%u bytes, min_free=%u bytes",
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    
    if (high_water_mark_end * sizeof(StackType_t) < 512) {
        ESP_LOGE(TAG, "CRITICAL: Task finished with very low stack! Risk of overflow!");
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
    ESP_LOGD(TAG, "schedule_callback_delivery called");
    if (m_callback_semaphore != nullptr) {
        BaseType_t result = xSemaphoreGive(m_callback_semaphore);
        if (result == pdTRUE) {
            ESP_LOGD(TAG, "schedule_callback_delivery: Semaphore given successfully");
        } else {
            ESP_LOGW(TAG, "schedule_callback_delivery: Failed to give semaphore (already given?)");
        }
    } else {
        ESP_LOGE(TAG, "schedule_callback_delivery: m_callback_semaphore is NULL!");
    }
}

} // namespace signalr
