#include "esp32_websocket_client.h"
#include "signalr_client_config.h"
#include "memory_utils.h"
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
    // OPTIMIZED: Reduced from 6144 to 5120 (5KB) - sufficient for JSON parsing
    // Stack monitoring showed typical usage is ~3-4KB
    constexpr size_t CALLBACK_TASK_STACK_SIZE = 5120;
#endif
    constexpr UBaseType_t CALLBACK_TASK_PRIORITY = 5;
    // OPTIMIZED: Increased from 10s to 15s - reconnection often takes longer
    // especially when server is restarting or network is recovering
#ifdef CONFIG_SIGNALR_CONNECTION_TIMEOUT_MS
    constexpr uint32_t CONNECTION_TIMEOUT_MS = CONFIG_SIGNALR_CONNECTION_TIMEOUT_MS;
#else
    constexpr uint32_t CONNECTION_TIMEOUT_MS = 15000;
#endif
#ifdef CONFIG_SIGNALR_MAX_QUEUE_SIZE
    constexpr size_t MAX_MESSAGE_QUEUE_SIZE = CONFIG_SIGNALR_MAX_QUEUE_SIZE;
#else
    // Reduced from 50 to 20 - prevents excessive memory usage
    constexpr size_t MAX_MESSAGE_QUEUE_SIZE = 20;
#endif

#ifdef CONFIG_SIGNALR_MAX_CALLBACK_TASKS
    constexpr UBaseType_t MAX_CALLBACK_EXEC_TASKS = CONFIG_SIGNALR_MAX_CALLBACK_TASKS;
#else
    // OPTIMIZED: Reduced from 3 to 2 - saves ~5KB stack per task
    // Most scenarios only need 1-2 concurrent callbacks
    constexpr UBaseType_t MAX_CALLBACK_EXEC_TASKS = 2;
#endif
    
    // Connection retry parameters
    constexpr uint32_t INITIAL_RETRY_DELAY_MS = 1000;
    constexpr uint32_t MAX_RETRY_DELAY_MS = 30000;
    constexpr float RETRY_BACKOFF_MULTIPLIER = 2.0f;
    
    // PSRAM usage threshold - buffers larger than this go to PSRAM if available
    constexpr size_t PSRAM_THRESHOLD = 1024;
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
    // CRITICAL: Disable WebSocket-level auto-reconnect so SignalR layer can handle reconnection
    // If WebSocket auto-reconnects, SignalR's handle_disconnection() won't be called
    ws_cfg.disable_auto_reconnect = true;  // Completely disable auto-reconnect
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
        // Use a timeout for close to prevent hanging indefinitely if the connection is already broken
        // The transport layer might be stuck waiting for a Close frame that will never come
        ESP_LOGI(TAG, "Closing WebSocket client...");
        esp_websocket_client_close(m_client, pdMS_TO_TICKS(1000)); 
        
        ESP_LOGI(TAG, "Stopping WebSocket client...");
        esp_websocket_client_stop(m_client);
        
        ESP_LOGI(TAG, "Destroying WebSocket client...");
        esp_websocket_client_destroy(m_client);
        m_client = nullptr;
    }
    m_is_connected = false;
    xEventGroupClearBits(m_event_group, CONNECTED_BIT);
    ESP_LOGI(TAG, "WebSocket client cleanup complete");
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
 * OPTIMIZED: Reduced task creation overhead by using a simpler callback structure.
 * The callback_payload now uses move semantics to avoid string copies.
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
        callback = std::move(m_pending_receive_callback);  // OPTIMIZED: Move callback too
        m_pending_receive_callback = nullptr;
        ESP_LOGD(TAG, "Deliver: %d bytes, queue: %zu", message.length(), m_message_queue.size());
    }
    
    // OPTIMIZED: Heap-allocate payload with move semantics
    auto* payload = new callback_payload{std::move(message), std::move(callback), m_callback_exec_limiter};
    
    // OPTIMIZED: Simplified task function with reduced stack requirements
    auto task = [](void* arg)
    {
        std::unique_ptr<callback_payload> payload(static_cast<callback_payload*>(arg));
        
        // Execute callback with minimal stack overhead
        try
        {
            payload->cb(payload->msg, nullptr);
        }
        catch (const std::exception& e)
        {
            ESP_LOGE(TAG, "Callback exception: %s", e.what());
        }
        catch (...)
        {
            ESP_LOGE(TAG, "Callback unknown exception");
        }

        if (payload->limiter)
        {
            xSemaphoreGive(payload->limiter);
        }

        vTaskDelete(nullptr);
    };

    bool scheduled = false;
    // Wait up to 500ms for a task slot instead of immediately failing
    // This prevents message loss when callback tasks are still completing
    if (m_callback_exec_limiter && xSemaphoreTake(m_callback_exec_limiter, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        // OPTIMIZED: Use dynamic stack size based on PSRAM availability
        uint32_t task_stack = signalr::memory::get_recommended_stack_size("callback");
        
        // First try regular task creation
        if (xTaskCreate(task, "signalr_cb_exec", task_stack, payload, CALLBACK_TASK_PRIORITY, nullptr) == pdPASS)
        {
            scheduled = true;
            ESP_LOGD(TAG, "Scheduled callback task (stack=%u)", task_stack);
        }
        else
        {
            // Task creation failed due to low memory
            // Execute callback inline on current task (callback_processor_task has sufficient stack)
            // This is safe because callback_processor_task was specifically designed with extra stack
            ESP_LOGW(TAG, "Task creation failed, executing callback inline (stack=%u)", task_stack);
            
            try
            {
                payload->cb(payload->msg, nullptr);
                scheduled = true;  // Mark as handled
                ESP_LOGD(TAG, "Inline callback execution completed");
            }
            catch (const std::exception& e)
            {
                ESP_LOGE(TAG, "Inline callback exception: %s", e.what());
                scheduled = true;  // Still mark as handled to prevent infinite retry
            }
            catch (...)
            {
                ESP_LOGE(TAG, "Inline callback unknown exception");
                scheduled = true;
            }
            
            // Release the semaphore since we executed inline
            xSemaphoreGive(m_callback_exec_limiter);
            delete payload;
            payload = nullptr;
        }
    }

    if (!scheduled && payload)
    {
        // When task creation fails, put the message back in the queue for retry
        // This is safer than dropping the message or causing stack overflow with inline execution
        ESP_LOGW(TAG, "Task creation failed, re-queuing message for retry");
        ESP_LOGW(TAG, "         Message size: %zu bytes", payload->msg.size());
        
        {
            std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
            std::lock_guard<std::mutex> cb_lock(m_callback_mutex);
            
            // Put message back at front of queue (it was originally at front)
            std::queue<std::string> temp_queue;
            temp_queue.push(std::move(payload->msg));
            while (!m_message_queue.empty()) {
                temp_queue.push(std::move(m_message_queue.front()));
                m_message_queue.pop();
            }
            m_message_queue = std::move(temp_queue);
            
            // Restore the callback so it can be retried
            if (!m_pending_receive_callback) {
                m_pending_receive_callback = std::move(payload->cb);
            }
        }
        
        delete payload; // Clean up payload
        
        // Log memory status to help diagnose the issue
        signalr::memory::log_memory_stats("task_creation_retry");
        
        // Wait a bit before next attempt to allow memory to be freed
        vTaskDelay(pdMS_TO_TICKS(100));
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

    // OPTIMIZED: Use PSRAM for receive buffer if available
    // This reduces internal RAM pressure significantly
    if (m_receive_buffer.capacity() < m_receive_buffer.size() + data_len) {
        size_t new_capacity = m_receive_buffer.size() + data_len + 512;
        // Try to use PSRAM for larger buffers
        if (new_capacity >= PSRAM_THRESHOLD && signalr::memory::is_psram_available()) {
            // Reserve will handle the reallocation
            m_receive_buffer.reserve(new_capacity);
            ESP_LOGD(TAG, "Receive buffer expanded to %u bytes (PSRAM preferred)", 
                     (unsigned)m_receive_buffer.capacity());
        } else {
            m_receive_buffer.reserve(new_capacity);
        }
    }
    m_receive_buffer.append(data, data_len);
    
    // Check for SignalR record separator (0x1E)
    size_t separator_pos = m_receive_buffer.find('\x1e');
    while (separator_pos != std::string::npos) {
        // OPTIMIZED: Use move semantics to avoid copy
        std::string message;
        message.reserve(separator_pos);
        message.assign(m_receive_buffer, 0, separator_pos);
        m_receive_buffer.erase(0, separator_pos + 1);
        
        // Reduced logging: Only log message length, not content (saves memory)
        ESP_LOGD(TAG, "RX msg: %d bytes", message.length());
        
        // Add message to queue (with overflow protection)
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            if (m_message_queue.size() < MAX_MESSAGE_QUEUE_SIZE) {
                m_message_queue.push(std::move(message));
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
    
    // OPTIMIZED: More aggressive shrinking to free PSRAM/RAM
    if (m_receive_buffer.capacity() > 4096 && m_receive_buffer.size() < 512) {
        m_receive_buffer.shrink_to_fit();
        ESP_LOGD(TAG, "Shrunk receive buffer to save memory");
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
    ESP_LOGI(TAG, "Callback task stack: ~%u bytes free initially", 
             high_water_mark_start * sizeof(StackType_t));
    
    // Log initial heap state
    signalr::memory::log_memory_stats("callback_task_start");
    
    ESP_LOGI(TAG, "Callback processor: entering main loop");
    while (client->m_callback_task_running) {
        // Wait for signal that a message is ready
        if (xSemaphoreTake(client->m_callback_semaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGD(TAG, "Callback processor: got semaphore, processing messages");
            
            // Keep processing while there are messages OR a pending callback
            // The loop continues as long as there's work to potentially do
            int message_count = 0;
            int idle_rounds = 0;
            const int MAX_IDLE_ROUNDS = 200; // 200 * 10ms = 2000ms max idle (increased from 500ms)
            
            while (client->m_callback_task_running && idle_rounds < MAX_IDLE_ROUNDS) {
                // OPTIMIZED: Reduced stack monitoring frequency to every 20 messages
                if (message_count > 0 && message_count % 20 == 0) {
                    UBaseType_t stack_free = uxTaskGetStackHighWaterMark(NULL);
                    ESP_LOGD(TAG, "Stack: %u bytes free", stack_free * sizeof(StackType_t));
                    if (stack_free * sizeof(StackType_t) < 512) {
                        ESP_LOGW(TAG, "WARNING: Low stack!");
                    }
                }
                
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
                    ESP_LOGD(TAG, "Processing message #%d", message_count);
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
                ESP_LOGW(TAG, "Callback timeout, %d messages queued", 
                         (int)client->m_message_queue.size());
            } else if (message_count > 0) {
                ESP_LOGI(TAG, "Processed %d messages", message_count);
            }
        }
    }
    
    // Always log final stack statistics
    UBaseType_t high_water_mark_end = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "Callback task final: %u bytes stack free (min)",
             high_water_mark_end * sizeof(StackType_t));
    signalr::memory::log_memory_stats("callback_task_end");
    
    if (high_water_mark_end * sizeof(StackType_t) < 512) {
        ESP_LOGE(TAG, "CRITICAL: Task finished with very low stack!");
    }
    
    ESP_LOGI(TAG, "Callback processor task exiting");
    vTaskDelete(NULL);
}

void esp32_websocket_client::start_callback_processor() {
    if (m_callback_task != nullptr) {
        return; // Already running
    }
    
    m_callback_task_running = true;
    
    // Use larger stack for callback processor since it may execute callbacks inline
    // when task creation fails due to low memory
    uint32_t stack_size = signalr::memory::get_recommended_stack_size("callback");
    // Add extra 2KB for inline callback execution safety margin
    stack_size += 2048;
    
    BaseType_t result = xTaskCreate(
        callback_processor_task,
        "signalr_cb",
        stack_size,
        this,
        CALLBACK_TASK_PRIORITY,
        &m_callback_task
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create callback processor task (stack=%u)", stack_size);
        m_callback_task = nullptr;
        m_callback_task_running = false;
    } else {
        ESP_LOGI(TAG, "Callback processor created (stack=%u)", stack_size);
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
