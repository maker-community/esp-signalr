// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

// ESP32 adaptation: FreeRTOS-based implementation

#include "stdafx.h"
#include "signalr_default_scheduler.h"
#include "esp_log.h"
#include <assert.h>

static const char* TAG = "signalr_scheduler";

namespace signalr
{
    // Worker thread implementation
    void thread::task_function(void* param)
    {
        auto* internals = static_cast<struct internals*>(param);
        
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
        
        // Create the worker task
        BaseType_t result = xTaskCreate(
            task_function,
            "signalr_worker",
            4096,  // Stack size
            m_internals.get(),
            5,  // Priority
            &m_task_handle
        );
        
        if (result != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create worker task");
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
        for (int i = 0; i < 100 && m_task_handle != nullptr; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
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
        
        std::vector<thread> threads(5);  // 5 worker threads
        
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
        
        BaseType_t result = xTaskCreate(
            scheduler_task_function,
            "signalr_sched",
            8192,  // Larger stack for scheduler
            m_internals.get(),
            5,  // Priority
            &m_scheduler_task_handle
        );
        
        if (result != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create scheduler task");
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
        for (int i = 0; i < 100 && m_scheduler_task_handle != nullptr; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
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
