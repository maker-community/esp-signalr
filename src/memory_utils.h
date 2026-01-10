// ESP32 SignalR Client - Memory Optimization Utilities
// Provides PSRAM-aware allocation and memory pool support for ESP32
// to reduce internal stack/heap pressure and prevent stack overflow

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <string>
#include <memory>
#include <vector>
#include <cstring>

namespace signalr {
namespace memory {

static const char* MEM_TAG = "SIGNALR_MEM";

// ============================================================================
// PSRAM Detection and Allocation
// ============================================================================

/**
 * Check if PSRAM is available on this device
 */
inline bool is_psram_available() {
#ifdef CONFIG_SPIRAM
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
#else
    return false;
#endif
}

/**
 * Get available PSRAM in bytes
 */
inline size_t get_psram_free() {
#ifdef CONFIG_SPIRAM
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#else
    return 0;
#endif
}

/**
 * Get available internal RAM in bytes
 */
inline size_t get_internal_free() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

/**
 * Log memory statistics
 */
inline void log_memory_stats(const char* context = "memory") {
    ESP_LOGI(MEM_TAG, "[%s] Internal: %u free, PSRAM: %u free",
             context,
             (unsigned)get_internal_free(),
             (unsigned)get_psram_free());
}

// ============================================================================
// PSRAM-Aware Allocators
// ============================================================================

/**
 * Allocate memory preferring PSRAM if available and size > threshold
 * Falls back to internal RAM if PSRAM not available or allocation fails
 * 
 * @param size Bytes to allocate
 * @param psram_threshold Minimum size to trigger PSRAM allocation (default 1KB)
 * @return Pointer to allocated memory, or nullptr on failure
 */
inline void* alloc_prefer_psram(size_t size, size_t psram_threshold = 1024) {
    void* ptr = nullptr;
    
#ifdef CONFIG_SPIRAM
    // Use PSRAM for larger allocations to save internal RAM
    if (size >= psram_threshold && is_psram_available()) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ptr) {
            ESP_LOGD(MEM_TAG, "Allocated %u bytes in PSRAM", (unsigned)size);
            return ptr;
        }
        // PSRAM allocation failed, fall through to internal
        ESP_LOGW(MEM_TAG, "PSRAM alloc failed for %u bytes, trying internal", (unsigned)size);
    }
#endif
    
    // Fallback to internal RAM
    ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ptr) {
        ESP_LOGD(MEM_TAG, "Allocated %u bytes in internal RAM", (unsigned)size);
    }
    return ptr;
}

/**
 * Allocate memory in PSRAM only
 * Returns nullptr if PSRAM not available
 */
inline void* alloc_psram_only(size_t size) {
#ifdef CONFIG_SPIRAM
    if (is_psram_available()) {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
#endif
    return nullptr;
}

/**
 * Free memory allocated with alloc_prefer_psram or alloc_psram_only
 */
inline void free_memory(void* ptr) {
    if (ptr) {
        heap_caps_free(ptr);
    }
}

/**
 * Reallocate memory, preserving PSRAM preference
 */
inline void* realloc_prefer_psram(void* ptr, size_t new_size, size_t psram_threshold = 1024) {
#ifdef CONFIG_SPIRAM
    if (new_size >= psram_threshold && is_psram_available()) {
        void* new_ptr = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (new_ptr) {
            return new_ptr;
        }
        // Fall through to internal RAM
    }
#endif
    return heap_caps_realloc(ptr, new_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// ============================================================================
// PSRAM-Backed String Buffer
// ============================================================================

/**
 * A string buffer that uses PSRAM for storage when available
 * Designed for large message buffers that would otherwise consume internal RAM
 */
class psram_string {
public:
    psram_string() : m_data(nullptr), m_size(0), m_capacity(0) {}
    
    explicit psram_string(size_t initial_capacity) : m_data(nullptr), m_size(0), m_capacity(0) {
        reserve(initial_capacity);
    }
    
    ~psram_string() {
        if (m_data) {
            free_memory(m_data);
        }
    }
    
    // Move-only to avoid accidental copies of large buffers
    psram_string(const psram_string&) = delete;
    psram_string& operator=(const psram_string&) = delete;
    
    psram_string(psram_string&& other) noexcept 
        : m_data(other.m_data), m_size(other.m_size), m_capacity(other.m_capacity) {
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_capacity = 0;
    }
    
    psram_string& operator=(psram_string&& other) noexcept {
        if (this != &other) {
            if (m_data) free_memory(m_data);
            m_data = other.m_data;
            m_size = other.m_size;
            m_capacity = other.m_capacity;
            other.m_data = nullptr;
            other.m_size = 0;
            other.m_capacity = 0;
        }
        return *this;
    }
    
    void reserve(size_t new_capacity) {
        if (new_capacity <= m_capacity) return;
        
        // Add some extra space to reduce reallocations
        new_capacity = std::max(new_capacity, m_capacity * 2);
        new_capacity = std::max(new_capacity, (size_t)256);
        
        char* new_data = static_cast<char*>(alloc_prefer_psram(new_capacity, 512));
        if (!new_data) {
            ESP_LOGE(MEM_TAG, "psram_string: allocation failed for %u bytes", (unsigned)new_capacity);
            return;
        }
        
        if (m_data && m_size > 0) {
            memcpy(new_data, m_data, m_size);
            free_memory(m_data);
        }
        
        m_data = new_data;
        m_capacity = new_capacity;
    }
    
    void append(const char* data, size_t len) {
        if (!data || len == 0) return;
        
        size_t new_size = m_size + len;
        if (new_size + 1 > m_capacity) {
            reserve(new_size + 1);
        }
        
        if (m_data) {
            memcpy(m_data + m_size, data, len);
            m_size = new_size;
            m_data[m_size] = '\0';
        }
    }
    
    void append(const std::string& str) {
        append(str.data(), str.size());
    }
    
    void erase(size_t start, size_t count) {
        if (start >= m_size) return;
        
        if (start + count >= m_size) {
            m_size = start;
        } else {
            size_t remaining = m_size - start - count;
            memmove(m_data + start, m_data + start + count, remaining);
            m_size -= count;
        }
        
        if (m_data) m_data[m_size] = '\0';
    }
    
    void clear() {
        m_size = 0;
        if (m_data) m_data[0] = '\0';
    }
    
    void shrink_to_fit() {
        if (m_capacity > m_size * 2 && m_capacity > 1024) {
            size_t new_capacity = std::max(m_size + 1, (size_t)256);
            char* new_data = static_cast<char*>(alloc_prefer_psram(new_capacity, 512));
            if (new_data) {
                if (m_data && m_size > 0) {
                    memcpy(new_data, m_data, m_size);
                    free_memory(m_data);
                }
                m_data = new_data;
                m_capacity = new_capacity;
                m_data[m_size] = '\0';
            }
        }
    }
    
    size_t find(char c, size_t start = 0) const {
        if (!m_data) return std::string::npos;
        for (size_t i = start; i < m_size; ++i) {
            if (m_data[i] == c) return i;
        }
        return std::string::npos;
    }
    
    std::string substr(size_t start, size_t len) const {
        if (!m_data || start >= m_size) return "";
        len = std::min(len, m_size - start);
        return std::string(m_data + start, len);
    }
    
    // Convert to std::string (copies data to internal RAM)
    std::string to_string() const {
        if (!m_data || m_size == 0) return "";
        return std::string(m_data, m_size);
    }
    
    const char* c_str() const { return m_data ? m_data : ""; }
    const char* data() const { return m_data; }
    size_t size() const { return m_size; }
    size_t capacity() const { return m_capacity; }
    bool empty() const { return m_size == 0; }
    
private:
    char* m_data;
    size_t m_size;
    size_t m_capacity;
};

// ============================================================================
// Task Stack Size Recommendations
// ============================================================================

/**
 * Get recommended task stack size based on PSRAM availability
 * When PSRAM is available, we can use larger stacks (allocated in PSRAM)
 * When only internal RAM, we need minimal stacks
 * 
 * CRITICAL: The reconnect task runs the entire connection flow synchronously:
 *   - WebSocket client creation
 *   - SSL/TLS handshake (if wss://)
 *   - SignalR handshake with JSON parsing
 *   - Exception handling with C++ unwinding
 *   - Multiple lambda captures with shared_ptr
 * This requires at least 10-12KB stack even without PSRAM!
 */
inline size_t get_recommended_stack_size(const char* task_type) {
    bool has_psram = is_psram_available();
    
    if (strcmp(task_type, "callback") == 0) {
        // Callback task: JSON parsing, user handlers
        return has_psram ? 8192 : 5120;  // 8KB with PSRAM, 5KB without
    }
    else if (strcmp(task_type, "worker") == 0) {
        // Worker task: general processing
        return has_psram ? 6144 : 4096;  // 6KB with PSRAM, 4KB without
    }
    else if (strcmp(task_type, "scheduler") == 0) {
        // Scheduler task: lightweight
        return has_psram ? 6144 : 4096;  // 6KB with PSRAM, 4KB without
    }
    else if (strcmp(task_type, "reconnect") == 0) {
        // Reconnect task: CRITICAL - runs entire connection flow synchronously!
        // The stack trace during reconnect is very deep:
        // reconnect_task -> start() -> start_negotiate() -> start_transport() 
        // -> websocket start -> handshake -> handle_handshake -> callbacks
        // Plus C++ exception handling overhead for each level
        // 
        // MUST be at least 12KB even without PSRAM to prevent stack overflow!
        return has_psram ? 16384 : 12288;  // 16KB with PSRAM, 12KB without
    }
    else if (strcmp(task_type, "websocket") == 0) {
        // WebSocket library task
        return has_psram ? 8192 : 6144;  // 8KB with PSRAM, 6KB without
    }
    
    // Default - be conservative
    return has_psram ? 8192 : 6144;
}

// ============================================================================
// Stack-safe callback wrapper
// ============================================================================

/**
 * Wraps a callback to execute with stack monitoring
 * Logs warning if stack usage is dangerously high
 */
template<typename Func>
void execute_with_stack_check(Func&& func, const char* context = "callback") {
    UBaseType_t stack_before = uxTaskGetStackHighWaterMark(NULL);
    
    func();
    
    UBaseType_t stack_after = uxTaskGetStackHighWaterMark(NULL);
    size_t stack_used = (stack_before - stack_after) * sizeof(StackType_t);
    
    if (stack_after * sizeof(StackType_t) < 512) {
        ESP_LOGW(MEM_TAG, "[%s] CRITICAL: Only %u bytes stack remaining!",
                 context, (unsigned)(stack_after * sizeof(StackType_t)));
    } else if (stack_used > 1024) {
        ESP_LOGD(MEM_TAG, "[%s] Stack delta: %u bytes", context, (unsigned)stack_used);
    }
}

// ============================================================================
// Memory pool for small allocations (reduces fragmentation)
// ============================================================================

/**
 * Simple fixed-size memory pool for frequently allocated objects
 * Reduces heap fragmentation from repeated small allocations
 */
template<size_t BLOCK_SIZE, size_t BLOCK_COUNT>
class memory_pool {
public:
    memory_pool() {
        m_mutex = xSemaphoreCreateMutex();
        
        // Try to allocate pool in PSRAM
        m_pool = static_cast<uint8_t*>(alloc_prefer_psram(BLOCK_SIZE * BLOCK_COUNT, 2048));
        if (m_pool) {
            memset(m_used, 0, sizeof(m_used));
            ESP_LOGI(MEM_TAG, "Memory pool created: %u blocks x %u bytes",
                     (unsigned)BLOCK_COUNT, (unsigned)BLOCK_SIZE);
        } else {
            ESP_LOGE(MEM_TAG, "Failed to create memory pool");
        }
    }
    
    ~memory_pool() {
        if (m_mutex) vSemaphoreDelete(m_mutex);
        if (m_pool) free_memory(m_pool);
    }
    
    void* allocate() {
        if (!m_pool || !m_mutex) return nullptr;
        
        xSemaphoreTake(m_mutex, portMAX_DELAY);
        
        for (size_t i = 0; i < BLOCK_COUNT; ++i) {
            if (!m_used[i]) {
                m_used[i] = true;
                xSemaphoreGive(m_mutex);
                return m_pool + (i * BLOCK_SIZE);
            }
        }
        
        xSemaphoreGive(m_mutex);
        ESP_LOGW(MEM_TAG, "Memory pool exhausted");
        return nullptr;
    }
    
    void deallocate(void* ptr) {
        if (!m_pool || !ptr) return;
        
        uint8_t* p = static_cast<uint8_t*>(ptr);
        if (p < m_pool || p >= m_pool + (BLOCK_SIZE * BLOCK_COUNT)) {
            ESP_LOGE(MEM_TAG, "Invalid pointer passed to pool deallocate");
            return;
        }
        
        size_t index = (p - m_pool) / BLOCK_SIZE;
        
        xSemaphoreTake(m_mutex, portMAX_DELAY);
        m_used[index] = false;
        xSemaphoreGive(m_mutex);
    }
    
    size_t available() const {
        size_t count = 0;
        for (size_t i = 0; i < BLOCK_COUNT; ++i) {
            if (!m_used[i]) ++count;
        }
        return count;
    }
    
private:
    uint8_t* m_pool = nullptr;
    bool m_used[BLOCK_COUNT] = {false};
    SemaphoreHandle_t m_mutex = nullptr;
};

} // namespace memory
} // namespace signalr
