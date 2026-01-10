// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

// ESP32 adaptation: FreeRTOS-based implementation with PSRAM optimization

#include "signalr_default_scheduler.h"
#include "memory_utils.h"
#include "esp_log.h"
#include <assert.h>

static const char* TAG = "signalr_scheduler";

// Configuration constants - OPTIMIZED based on PSRAM availability
namespace {
#ifdef CONFIG_SIGNALR_WORKER_STACK_SIZE
    constexpr uint32_t WORKER_TASK_STACK_SIZE = CONFIG_SIGNALR_WORKER_STACK_SIZE;
#else
    // OPTIMIZED: Reduced from 6144 to 4096 (4KB) when no PSRAM
    // With PSRAM available, memory_utils will recommend larger stacks
    constexpr uint32_t WORKER_TASK_STACK_SIZE = 4096;
#endif

#ifdef CONFIG_SIGNALR_SCHEDULER_STACK_SIZE
    constexpr uint32_t SCHEDULER_TASK_STACK_SIZE = CONFIG_SIGNALR_SCHEDULER_STACK_SIZE;
#else
    // OPTIMIZED: Reduced from 6144 to 4096 (4KB) - scheduler is lightweight
    constexpr uint32_t SCHEDULER_TASK_STACK_SIZE = 4096;
#endif

#ifdef CONFIG_SIGNALR_WORKER_POOL_SIZE
    constexpr size_t WORKER_THREAD_POOL_SIZE = CONFIG_SIGNALR_WORKER_POOL_SIZE;
#else
    constexpr size_t WORKER_THREAD_POOL_SIZE = 2;        // Default: 2 workers
#endif

    constexpr UBaseType_t TASK_PRIORITY = 5;             // Priority for all SignalR tasks
    constexpr uint32_t SHUTDOWN_RETRY_COUNT = 100;       // Max retries when shutting down
    constexpr uint32_t SHUTDOWN_RETRY_DELAY_MS = 10;     // Delay between shutdown retries
    
    // Get actual stack size based on PSRAM availability
    inline uint32_t get_actual_worker_stack_size() {
        return signalr::memory::get_recommended_stack_size("worker");
    }
    
    inline uint32_t get_actual_scheduler_stack_size() {
        return signalr::memory::get_recommended_stack_size("scheduler");
    }
}

namespace signalr
{
    // Worker thread implementation
    void thread::task_function(void* param)
    {
        auto* internals = static_cast<struct internals*>(param);
        
        // Always monitor stack - critical for debugging stack overflow issues
        UBaseType_t high_water_mark_start = uxTaskGetStackHighWaterMark(NULL);
        uint32_t actual_stack = get_actual_worker_stack_size();
        ESP_LOGI(TAG, "Worker task started - stack: %u bytes allocated, %u bytes free initially", 
                 actual_stack, high_water_mark_start * sizeof(StackType_t));
        
        while (true)
        {
            // Wait for work to be assigned
            xSemaphoreTake(internals->m_callback_sem, portMAX_DELAY);
            
            signalr_base_cb cb;
            {
                // Lock to get the callback
                xSemaphoreTake(internals->m_callback_mutex, portMAX_DELAY);
                
                if (internals->m_closed && internals->m_callback == nullptr)
                {
                    xSemaphoreGive(internals->m_callback_mutex);
                    
                    // Always log final stack statistics
                    UBaseType_t high_water_mark_end = uxTaskGetStackHighWaterMark(NULL);
                    uint32_t actual_stack = get_actual_worker_stack_size();
                    size_t stack_used = actual_stack - (high_water_mark_end * sizeof(StackType_t));
                    ESP_LOGI(TAG, "Worker task exiting - stack: %u bytes used (%.1f%%), %u bytes free (min)",
                             stack_used, (stack_used * 100.0f) / actual_stack,
                             high_water_mark_end * sizeof(StackType_t));
                    
                    if (high_water_mark_end * sizeof(StackType_t) < 512) {
                        ESP_LOGW(TAG, "WARNING: Worker task had very low stack! Risk of overflow!");
                    }
                    
                    vTaskDelete(NULL);
                    return;
                }
                
                cb = internals->m_callback;
                internals->m_callback = nullptr;
                
                xSemaphoreGive(internals->m_callback_mutex);
            }
            
            // Execute the callback
            if (cb)
            {
                try
                {
                    cb();
                }
                catch (...)
                {
                    ESP_LOGE(TAG, "Exception in worker thread callback");
                }
            }
            
            // Mark as not busy
            internals->m_busy = false;
        }
    }

    thread::thread()
        : m_internals(std::make_shared<internals>())
        , m_task_handle(nullptr)
    {
        m_internals->m_callback = nullptr;
        m_internals->m_callback_mutex = xSemaphoreCreateMutex();
        m_internals->m_callback_sem = xSemaphoreCreateBinary();
        m_internals->m_closed = false;
        m_internals->m_busy = false;
        
        if (m_internals->m_callback_mutex == nullptr || m_internals->m_callback_sem == nullptr)
        {
            ESP_LOGE(TAG, "Failed to create synchronization primitives");
            return;
        }
        
        // Create the worker task with PSRAM-optimized stack size
        uint32_t actual_stack = get_actual_worker_stack_size();
        BaseType_t result = xTaskCreate(
            task_function,
            "signalr_worker",
            actual_stack,
            m_internals.get(),
            TASK_PRIORITY,
            &m_task_handle
        );
        
        if (result != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create worker task (stack=%u)", actual_stack);
        }
        else
        {
            ESP_LOGD(TAG, "Created worker task with %u byte stack", actual_stack);
        }
    }

    void thread::add(signalr_base_cb cb)
    {
        xSemaphoreTake(m_internals->m_callback_mutex, portMAX_DELAY);
        
        assert(m_internals->m_closed == false);
        assert(m_internals->m_busy == false);
        
        m_internals->m_callback = cb;
        m_internals->m_busy = true;
        
        xSemaphoreGive(m_internals->m_callback_mutex);
    }

    void thread::start()
    {
        xSemaphoreGive(m_internals->m_callback_sem);
    }

    void thread::shutdown()
    {
        xSemaphoreTake(m_internals->m_callback_mutex, portMAX_DELAY);
        m_internals->m_closed = true;
        xSemaphoreGive(m_internals->m_callback_mutex);
        
        // Signal the task to wake up and exit
        xSemaphoreGive(m_internals->m_callback_sem);
        
        // Wait for task to complete (with timeout)
        for (uint32_t i = 0; i < SHUTDOWN_RETRY_COUNT && m_task_handle != nullptr; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(SHUTDOWN_RETRY_DELAY_MS));
        }
    }

    bool thread::is_free() const
    {
        return !m_internals->m_busy;
    }

    thread::~thread()
    {
        shutdown();
        
        if (m_internals->m_callback_mutex != nullptr)
        {
            vSemaphoreDelete(m_internals->m_callback_mutex);
        }
        if (m_internals->m_callback_sem != nullptr)
        {
            vSemaphoreDelete(m_internals->m_callback_sem);
        }
    }

    // Scheduler task implementation
    void signalr_default_scheduler::scheduler_task_function(void* param)
    {
        auto* internals = static_cast<struct signalr_default_scheduler::internals*>(param);
        
#ifdef CONFIG_SIGNALR_ENABLE_STACK_MONITORING
        UBaseType_t high_water_mark_start = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(TAG, "Scheduler task started - initial stack high water mark: %u bytes", 
                 high_water_mark_start * sizeof(StackType_t));
#endif
        
        std::vector<thread> threads(WORKER_THREAD_POOL_SIZE);  // Worker threads
        
        size_t prev_callback_count = 0;
        
        while (true)
        {
            // Wait for callbacks with timeout
            xSemaphoreTake(internals->m_callback_sem, pdMS_TO_TICKS(15));
            
            xSemaphoreTake(internals->m_callback_mutex, portMAX_DELAY);
            
            if (internals->m_closed && internals->m_callbacks.empty())
            {
                xSemaphoreGive(internals->m_callback_mutex);
                vTaskDelete(NULL);
                return;
            }
            
            // Find callbacks ready to run
            auto curr_time = std::chrono::steady_clock::now();
            auto it = internals->m_callbacks.begin();
            
            while (it != internals->m_callbacks.end())
            {
                bool found = false;
                
                if (it->second <= curr_time)
                {
                    // Find a free worker thread
                    for (auto& worker : threads)
                    {
                        if (worker.is_free())
                        {
                            worker.add(it->first);
                            it->first = nullptr;
                            worker.start();
                            found = true;
                            break;
                        }
                    }
                    
                    if (!found)
                    {
                        // No free workers, break and try again later
                        break;
                    }
                }
                
                if (found)
                {
                    it = internals->m_callbacks.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            
            prev_callback_count = internals->m_callbacks.size();
            
            xSemaphoreGive(internals->m_callback_mutex);
        }
        
#ifdef CONFIG_SIGNALR_ENABLE_STACK_MONITORING
        UBaseType_t high_water_mark_end = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(TAG, "Scheduler task exiting - final stack high water mark: %u bytes", 
                 high_water_mark_end * sizeof(StackType_t));
        ESP_LOGI(TAG, "Scheduler task stack used: %u bytes out of %u",
                 SCHEDULER_TASK_STACK_SIZE - (high_water_mark_end * sizeof(StackType_t)), 
                 SCHEDULER_TASK_STACK_SIZE);
#endif
    }

    void signalr_default_scheduler::schedule(const signalr_base_cb& cb, std::chrono::milliseconds delay)
    {
        xSemaphoreTake(m_internals->m_callback_mutex, portMAX_DELAY);
        
        assert(m_internals->m_closed == false);
        
        m_internals->m_callbacks.push_back(
            std::make_pair(cb, std::chrono::steady_clock::now() + delay)
        );
        
        xSemaphoreGive(m_internals->m_callback_mutex);
        
        // Notify scheduler if no delay
        if (delay == std::chrono::milliseconds::zero())
        {
            xSemaphoreGive(m_internals->m_callback_sem);
        }
    }

    void signalr_default_scheduler::run()
    {
        m_internals->m_callback_mutex = xSemaphoreCreateMutex();
        m_internals->m_callback_sem = xSemaphoreCreateBinary();
        m_internals->m_closed = false;
        
        if (m_internals->m_callback_mutex == nullptr || m_internals->m_callback_sem == nullptr)
        {
            ESP_LOGE(TAG, "Failed to create scheduler synchronization primitives");
            return;
        }
        
        // Create scheduler task with PSRAM-optimized stack size
        uint32_t actual_stack = get_actual_scheduler_stack_size();
        BaseType_t result = xTaskCreate(
            scheduler_task_function,
            "signalr_sched",
            actual_stack,
            m_internals.get(),
            TASK_PRIORITY,
            &m_scheduler_task_handle
        );
        
        if (result != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create scheduler task (stack=%u)", actual_stack);
        }
        else
        {
            // Log memory status on scheduler start
            signalr::memory::log_memory_stats("scheduler_init");
            ESP_LOGI(TAG, "Created scheduler task with %u byte stack", actual_stack);
        }
    }

    void signalr_default_scheduler::close()
    {
        if (m_internals->m_callback_mutex != nullptr)
        {
            xSemaphoreTake(m_internals->m_callback_mutex, portMAX_DELAY);
            m_internals->m_closed = true;
            xSemaphoreGive(m_internals->m_callback_mutex);
        }
        
        if (m_internals->m_callback_sem != nullptr)
        {
            xSemaphoreGive(m_internals->m_callback_sem);
        }
    }

    signalr_default_scheduler::~signalr_default_scheduler()
    {
        close();
        
        // Wait for scheduler task to complete (with timeout)
        for (uint32_t i = 0; i < SHUTDOWN_RETRY_COUNT && m_scheduler_task_handle != nullptr; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(SHUTDOWN_RETRY_DELAY_MS));
        }
        
        if (m_internals->m_callback_mutex != nullptr)
        {
            vSemaphoreDelete(m_internals->m_callback_mutex);
        }
        if (m_internals->m_callback_sem != nullptr)
        {
            vSemaphoreDelete(m_internals->m_callback_sem);
        }
    }

    // Timer functions remain the same
    void timer(const std::shared_ptr<scheduler>& scheduler, std::function<bool(std::chrono::milliseconds)> func)
    {
        timer_internal(scheduler, func, std::chrono::milliseconds::zero());
    }

    void timer_internal(const std::shared_ptr<scheduler>& scheduler, std::function<bool(std::chrono::milliseconds)> func, std::chrono::milliseconds duration)
    {
        constexpr auto tick = std::chrono::seconds(1);
        duration += tick;
        scheduler->schedule([func, scheduler, duration]()
            {
                if (!func(duration))
                {
                    timer_internal(scheduler, func, duration);
                }
            }, tick);
    }
}
