// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

// ESP32 adaptation: FreeRTOS-based header

#pragma once

#include "scheduler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <vector>
#include <chrono>
#include <memory>

namespace signalr
{
    struct thread
    {
    public:
        thread();
        thread(const thread&) = delete;
        thread& operator=(const thread&) = delete;

        void add(signalr_base_cb);
        void start();
        bool is_free() const;
        void shutdown();
        ~thread();
    private:
        struct internals
        {
            signalr_base_cb m_callback;
            SemaphoreHandle_t m_callback_mutex;
            SemaphoreHandle_t m_callback_sem;
            bool m_closed;
            bool m_busy;
        };

        std::shared_ptr<internals> m_internals;
        TaskHandle_t m_task_handle;
        
        static void task_function(void* param);
    };

    struct signalr_default_scheduler : scheduler
    {
        signalr_default_scheduler() : m_internals(std::make_shared<internals>())
        {
            run();
        }
        signalr_default_scheduler(const signalr_default_scheduler&) = delete;
        signalr_default_scheduler& operator=(const signalr_default_scheduler&) = delete;

        void schedule(const signalr_base_cb& cb, std::chrono::milliseconds delay = std::chrono::milliseconds::zero());
        ~signalr_default_scheduler();

    private:
        void run();

        struct internals
        {
            std::vector<std::pair<signalr_base_cb, std::chrono::time_point<std::chrono::steady_clock, std::chrono::nanoseconds>>> m_callbacks;
            SemaphoreHandle_t m_callback_mutex;
            SemaphoreHandle_t m_callback_sem;
            bool m_closed;
        };

        std::shared_ptr<internals> m_internals;
        TaskHandle_t m_scheduler_task_handle;

        void close();
        
        static void scheduler_task_function(void* param);
    };

    void timer_internal(const std::shared_ptr<scheduler>& scheduler, std::function<bool(std::chrono::milliseconds)> func, std::chrono::milliseconds duration);
    void timer(const std::shared_ptr<scheduler>& scheduler, std::function<bool(std::chrono::milliseconds)> func);
}
