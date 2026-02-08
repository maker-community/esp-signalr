// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "hub_connection_impl.h"
#include "hub_exception.h"
#ifdef CONFIG_SIGNALR_ENABLE_TRACE_LOG_WRITER
#include "trace_log_writer.h"
#endif
#include "signalr_exception.h"
#include "json_hub_protocol.h"
#include "message_type.h"
#include "handshake_protocol.h"
#include "signalr_client_config.h"
#include "json_helpers.h"
#include "websocket_client.h"
#include "signalr_default_scheduler.h"
#include "memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

// OPTIMIZED: Reconnect task stack size - dynamically determined
namespace {
    inline uint32_t get_reconnect_stack_size() {
#ifdef CONFIG_SIGNALR_RECONNECT_STACK_SIZE
        // Use Kconfig value if specified
        return CONFIG_SIGNALR_RECONNECT_STACK_SIZE;
#else
        // Fall back to dynamic sizing based on PSRAM availability
        return signalr::memory::get_recommended_stack_size("reconnect");
#endif
    }
}

namespace signalr
{
    // unnamed namespace makes it invisble outside this translation unit
    namespace
    {
        static std::function<void(const char*, const signalr::value&)> create_hub_invocation_callback(const logger& logger,
            const std::function<void(const signalr::value&)>& set_result,
            const std::function<void(const std::exception_ptr e)>& set_exception);
    }

    std::shared_ptr<hub_connection_impl> hub_connection_impl::create(const std::string& url, std::unique_ptr<hub_protocol>&& hub_protocol,
        trace_level trace_level, const std::shared_ptr<log_writer>& log_writer, std::function<std::shared_ptr<http_client>(const signalr_client_config&)> http_client_factory,
        std::function<std::shared_ptr<websocket_client>(const signalr_client_config&)> websocket_factory, const bool skip_negotiation)
    {
        auto connection = std::shared_ptr<hub_connection_impl>(new hub_connection_impl(url, std::move(hub_protocol),
            trace_level, log_writer, http_client_factory, websocket_factory, skip_negotiation));

        connection->initialize();

        return connection;
    }

    hub_connection_impl::hub_connection_impl(const std::string& url, std::unique_ptr<hub_protocol>&& hub_protocol, trace_level trace_level,
        const std::shared_ptr<log_writer>& log_writer, std::function<std::shared_ptr<http_client>(const signalr_client_config&)> http_client_factory,
        std::function<std::shared_ptr<websocket_client>(const signalr_client_config&)> websocket_factory, const bool skip_negotiation)
        : m_connection(connection_impl::create(url, trace_level, log_writer, http_client_factory, websocket_factory, skip_negotiation))
            , m_logger(log_writer, trace_level),
        m_callback_manager("connection went out of scope before invocation result was received"),
        m_handshakeReceived(false), m_disconnected([](std::exception_ptr) noexcept {}), m_protocol(std::move(hub_protocol)),
        m_reconnecting(false), m_reconnect_attempts(0)
    {
        hub_message ping_msg(signalr::message_type::ping);
        m_cached_ping = m_protocol->write_message(&ping_msg);
    }

    void hub_connection_impl::initialize()
    {
        // weak_ptr prevents a circular dependency leading to memory leak and other problems
        std::weak_ptr<hub_connection_impl> weak_hub_connection = shared_from_this();

        m_connection->set_message_received([weak_hub_connection](std::string&& message)
        {
            auto connection = weak_hub_connection.lock();
            if (connection)
            {
                connection->process_message(std::move(message));
            }
        });

        m_connection->set_disconnected([weak_hub_connection](std::exception_ptr exception)
        {
            auto connection = weak_hub_connection.lock();
            if (connection)
            {
                connection->handle_disconnection(exception);
            }
        });
    }

    void hub_connection_impl::on(const std::string& event_name, const std::function<void(const std::vector<signalr::value>&)>& handler)
    {
        if (event_name.length() == 0)
        {
            throw std::invalid_argument("event_name cannot be empty");
        }

        auto weak_connection = std::weak_ptr<hub_connection_impl>(shared_from_this());
        auto connection = weak_connection.lock();
        if (connection && connection->get_connection_state() != connection_state::disconnected)
        {
            ESP_LOGE("HUB_CONN", "on('%s') FAILED: connection not in disconnected state (state=%d)", 
                     event_name.c_str(), (int)connection->get_connection_state());
            throw signalr_exception("can't register a handler if the connection is not in a disconnected state");
        }

        if (m_subscriptions.find(event_name) != m_subscriptions.end())
        {
            ESP_LOGE("HUB_CONN", "on('%s') FAILED: handler already registered", event_name.c_str());
            throw signalr_exception(
                "an action for this event has already been registered. event name: " + event_name);
        }

        m_subscriptions.insert({event_name, handler});
        ESP_LOGI("HUB_CONN", "on('%s') SUCCESS: handler registered, total subscriptions=%d", 
                 event_name.c_str(), (int)m_subscriptions.size());
    }

    void hub_connection_impl::start(std::function<void(std::exception_ptr)> callback) noexcept
    {
        if (m_connection->get_connection_state() != connection_state::disconnected)
        {
            callback(std::make_exception_ptr(signalr_exception(
                "the connection can only be started if it is in the disconnected state")));
            return;
        }

        // Reset reconnect state when manually starting
        if (!m_reconnecting.load())
        {
            m_reconnect_attempts.store(0);
        }

        m_connection->set_client_config(m_signalr_client_config);
        m_handshakeTask = std::make_shared<completion_event>();
        m_disconnect_cts = std::make_shared<cancellation_token_source>();
        m_handshakeReceived = false;
        std::weak_ptr<hub_connection_impl> weak_connection = shared_from_this();
        m_connection->start([weak_connection, callback](std::exception_ptr start_exception)
            {
                auto connection = weak_connection.lock();
                if (!connection)
                {
                    // The connection has been destructed
                    callback(std::make_exception_ptr(signalr_exception("the hub connection has been deconstructed")));
                    return;
                }

                if (start_exception)
                {
                    assert(connection->get_connection_state() == connection_state::disconnected);
                    // connection didn't start, don't call stop
                    callback(start_exception);
                    return;
                }

                std::shared_ptr<std::mutex> handshake_request_lock = std::make_shared<std::mutex>();
                // Track whether handshake callback has already run; explicitly init to false
                std::shared_ptr<bool> handshake_request_done = std::make_shared<bool>(false);

                auto handle_handshake = [weak_connection, handshake_request_done, handshake_request_lock, callback](std::exception_ptr exception, bool fromSend)
                {
                    ESP_LOGI("HUB_CONN", ">>> handle_handshake ENTERED, fromSend=%d <<<", fromSend);
                    assert(fromSend ? *handshake_request_done : true);

                    auto connection = weak_connection.lock();
                    if (!connection)
                    {
                        ESP_LOGE("HUB_CONN", "handle_handshake: connection destructed!");
                        // The connection has been destructed
                        callback(std::make_exception_ptr(signalr_exception("the hub connection has been deconstructed")));
                        return;
                    }

                    {
                        ESP_LOGI("HUB_CONN", "handle_handshake: Acquiring handshake_request_lock...");
                        std::lock_guard<std::mutex> lock(*handshake_request_lock);
                        ESP_LOGI("HUB_CONN", "handle_handshake: Got lock, checking handshake_request_done...");
                        // connection.send will be waiting on the handshake task which has been set by the caller already
                        if (!fromSend && *handshake_request_done == true)
                        {
                            ESP_LOGI("HUB_CONN", "handle_handshake: Already done, returning");
                            return;
                        }
                        *handshake_request_done = true;
                        ESP_LOGI("HUB_CONN", "handle_handshake: Set handshake_request_done=true");
                    }

                    ESP_LOGI("HUB_CONN", "handle_handshake: Lock released, proceeding to try block...");
                    try
                    {
                        if (exception == nullptr)
                        {
                            ESP_LOGI("HUB_CONN", "handle_handshake: No exception, waiting for handshake completion...");
                            // CRITICAL FIX: Don't block on m_handshakeTask->get() if we're on
                            // the callback processor task! This would deadlock because the
                            // handshake response needs to be processed by the same task.
                            //
                            // Solution: Poll with yield instead of blocking wait
                            const int MAX_WAIT_MS = 30000; // 30 seconds timeout
                            const int POLL_INTERVAL_MS = 10;
                            int waited_ms = 0;
                            
                            ESP_LOGI("HUB_CONN", "handle_handshake: Starting poll loop for handshake completion...");
                            while (!connection->m_handshakeTask->is_set() && waited_ms < MAX_WAIT_MS)
                            {
                                vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
                                waited_ms += POLL_INTERVAL_MS;
                                if (waited_ms % 1000 == 0) {
                                    ESP_LOGI("HUB_CONN", "handle_handshake: Still waiting... %d ms elapsed", waited_ms);
                                }
                            }
                            
                            if (!connection->m_handshakeTask->is_set())
                            {
                                ESP_LOGE("HUB_CONN", "handle_handshake: TIMEOUT after %d ms!", waited_ms);
                                exception = std::make_exception_ptr(signalr_exception("handshake timeout"));
                            }
                            else
                            {
                                ESP_LOGI("HUB_CONN", "handle_handshake: Handshake completed! Calling get() (should not block)...");
                                // Now it's safe to call get() since we know it won't block
                                connection->m_handshakeTask->get();
                                ESP_LOGI("HUB_CONN", "handle_handshake: get() returned, calling callback(nullptr)...");
                                callback(nullptr);
                                ESP_LOGI("HUB_CONN", "handle_handshake: callback returned!");
                            }
                        }
                    }
                    catch (...)
                    {
                        ESP_LOGE("HUB_CONN", "handle_handshake: Exception caught in try block!");
                        exception = std::current_exception();
                    }

                    if (exception != nullptr)
                    {
                        connection->m_logger.log(trace_level::warning, "handshake failed, stopping connection");
                        connection->m_connection->stop([callback, exception](std::exception_ptr)
                            {
                                callback(exception);
                            }, exception);
                    }
                    else
                    {
                        connection->m_logger.log(trace_level::info, "handshake succeeded, starting keepalive");
                        connection->start_keepalive();
                    }
                };

                auto handshake_request = handshake::write_handshake(connection->m_protocol);
                auto handshake_task = connection->m_handshakeTask;
                auto handshake_timeout = connection->m_signalr_client_config.get_handshake_timeout();

                connection->m_disconnect_cts->register_callback([handle_handshake, handshake_request_lock, handshake_request_done]()
                    {
                        {
                            std::lock_guard<std::mutex> lock(*handshake_request_lock);
                            // no op after connection.send returned, m_handshakeTask should be set before m_disconnect_cts is canceled
                            if (*handshake_request_done == true)
                            {
                                return;
                            }
                        }

                        // if the request isn't completed then no one is waiting on the handshake task
                        // so we need to run the callback here instead of relying on connection.send completing
                        // handshake_request_done is set in handle_handshake, don't set it here
                        handle_handshake(nullptr, false);
                    });

                timer(connection->m_signalr_client_config.get_scheduler(),
                    [handle_handshake, handshake_task, handshake_timeout, handshake_request_lock](std::chrono::milliseconds duration)
                    {
                        {
                            std::lock_guard<std::mutex> lock(*handshake_request_lock);

                            // if the task is set then connection.send is either already waiting on the handshake or has completed,
                            // or stop has been called and will be handling the callback
                            if (handshake_task->is_set())
                            {
                                return true;
                            }

                            if (duration < handshake_timeout)
                            {
                                return false;
                            }
                        }

                        auto exception = std::make_exception_ptr(signalr_exception("timed out waiting for the server to respond to the handshake message."));
                        // unblocks connection.send if it's waiting on the task
                        handshake_task->set(exception);

                        handle_handshake(exception, false);
                        return true;
                    });

                connection->m_connection->send(handshake_request, connection->m_protocol->transfer_format(),
                    [handle_handshake, handshake_request_done, handshake_request_lock](std::exception_ptr exception)
                {
                    {
                        std::lock_guard<std::mutex> lock(*handshake_request_lock);
                        if (*handshake_request_done == true)
                        {
                            // callback ran from timer or cancellation token, nothing to do here
                            return;
                        }

                        // indicates that the handshake timer doesn't need to call the callback, it just needs to set the timeout exception
                        // handle_handshake will be waiting on the handshake completion (error or success) to call the callback
                        *handshake_request_done = true;
                    }

                    handle_handshake(exception, true);
                });
            });
    }

    void hub_connection_impl::stop(std::function<void(std::exception_ptr)> callback, bool is_dtor) noexcept
    {
        // Cancel any ongoing reconnection attempts
        {
            std::lock_guard<std::mutex> lock(m_reconnect_lock);
            if (m_reconnecting.load())
            {
                m_logger.log(trace_level::info, "stopping connection and cancelling reconnection attempts");
                m_reconnecting.store(false);
                m_reconnect_attempts.store(0);
                
                if (m_reconnect_cts)
                {
                    try
                    {
                        m_reconnect_cts->cancel();
                    }
                    catch (...)
                    {
                        // Ignore cancellation errors
                    }
                }
            }
        }

        if (get_connection_state() == connection_state::disconnected)
        {
            // don't log if already disconnected and stop called from dtor, it's just noise
            if (!is_dtor)
            {
                m_logger.log(trace_level::debug, "stop ignored because the connection is already disconnected.");
            }
            callback(nullptr);
            return;
        }
        else
        {
            {
                std::lock_guard<std::mutex> lock(m_stop_callback_lock);
                m_stop_callbacks.push_back(callback);

                if (m_stop_callbacks.size() > 1)
                {
                    m_logger.log(trace_level::info, "Stop is already in progress, waiting for it to finish.");
                    // we already registered the callback
                    // so we can just return now as the in-progress stop will trigger the callback when it completes
                    return;
                }
            }
            std::weak_ptr<hub_connection_impl> weak_connection = shared_from_this();
            m_connection->stop([weak_connection](std::exception_ptr exception)
                {
                    auto connection = weak_connection.lock();
                    if (!connection)
                    {
                        return;
                    }

                    assert(connection->get_connection_state() == connection_state::disconnected);

                    std::vector<std::function<void(std::exception_ptr)>> callbacks;

                    {
                        std::lock_guard<std::mutex> lock(connection->m_stop_callback_lock);
                        // copy the callbacks out and clear the list inside the lock
                        // then run the callbacks outside of the lock
                        callbacks = connection->m_stop_callbacks;
                        connection->m_stop_callbacks.clear();
                    }

                    for (auto& callback : callbacks)
                    {
                        callback(exception);
                    }
                }, nullptr);
        }
    }

    void hub_connection_impl::process_message(std::string&& response)
    {
        ESP_LOGI("HUB_CONN", ">>> process_message CALLED, message length: %d <<<", response.length());
        ESP_LOGI("HUB_CONN", "process_message: message content: %s", response.c_str());
        
        try
        {
            if (!m_handshakeReceived)
            {
                ESP_LOGI("HUB_CONN", "process_message: Handshake NOT received yet, parsing handshake...");
                // Our WebSocket adapter strips the 0x1E record separator when queuing messages.
                // The upstream handshake parser expects the separator to delineate the handshake frame.
                // If it is missing, append it temporarily so parse_handshake succeeds.
                std::string handshake_frame = response;
                if (handshake_frame.find(record_separator) == std::string::npos) {
                    handshake_frame.push_back(record_separator);
                }

                signalr::value handshake;
                std::tie(response, handshake) = handshake::parse_handshake(handshake_frame);
                ESP_LOGI("HUB_CONN", "process_message: Handshake parsed");

                auto& obj = handshake.as_map();
                auto found = obj.find("error");
                if (found != obj.end())
                {
                    auto& error = found->second.as_string();
                    ESP_LOGE("HUB_CONN", "process_message: Handshake error: %s", error.c_str());
                    if (m_logger.is_enabled(trace_level::error))
                    {
                        m_logger.log(trace_level::error, std::string("handshake error: ")
                            .append(error));
                    }
                    m_handshakeTask->set(std::make_exception_ptr(signalr_exception(std::string("Received an error during handshake: ").append(error))));
                    return;
                }
                else
                {
                    found = obj.find("type");
                    if (found != obj.end())
                    {
                        ESP_LOGE("HUB_CONN", "process_message: Unexpected message during handshake");
                        m_handshakeTask->set(std::make_exception_ptr(signalr_exception(std::string("Received unexpected message while waiting for the handshake response."))));
                        return;
                    }

                    ESP_LOGI("HUB_CONN", "process_message: Handshake successful! Setting m_handshakeTask...");
                    m_handshakeReceived = true;
                    m_handshakeTask->set();
                    ESP_LOGI("HUB_CONN", "process_message: m_handshakeTask->set() called!");

                    if (response.size() == 0)
                    {
                        ESP_LOGI("HUB_CONN", "process_message: No additional data after handshake, returning");
                        return;
                    }
                }
            }

            ESP_LOGI("HUB_CONN", "process_message: Resetting server timeout...");
            reset_server_timeout();
            
            // Ensure message has record separator for proper parsing
            // WebSocket adapter may strip the 0x1E record separator
            if (response.find(record_separator) == std::string::npos) {
                ESP_LOGW("HUB_CONN", "process_message: Adding missing record_separator to message");
                response.push_back(record_separator);
            }
            
            auto messages = m_protocol->parse_messages(response);
            ESP_LOGI("HUB_CONN", "process_message: Parsed %d message(s)", (int)messages.size());

            for (const auto& val : messages)
            {
                // Protocol received an unknown message type and gave us a null object, close the connection like we do in other client implementations
                if (val == nullptr)
                {
                    throw std::runtime_error("null message received");
                }

                switch (val->message_type)
                {
                case message_type::invocation:
                {
                    auto invocation = static_cast<invocation_message*>(val.get());
                    ESP_LOGI("HUB_CONN", "Looking for handler: target='%s', subscriptions count=%d", 
                             invocation->target.c_str(), (int)m_subscriptions.size());
                    auto event = m_subscriptions.find(invocation->target);
                    if (event != m_subscriptions.end())
                    {
                        ESP_LOGI("HUB_CONN", "Handler FOUND for '%s', calling...", invocation->target.c_str());
                        const auto& args = invocation->arguments;
                        event->second(args);
                        ESP_LOGI("HUB_CONN", "Handler '%s' call completed", invocation->target.c_str());
                    }
                    else
                    {
                        ESP_LOGW("HUB_CONN", "Handler NOT FOUND for '%s'", invocation->target.c_str());
                        m_logger.log(trace_level::info, "handler not found");
                    }
                    break;
                }
                case message_type::stream_invocation:
                    // Sent to server only, should not be received by client
                    throw std::runtime_error("Received unexpected message type 'StreamInvocation'");
                case message_type::stream_item:
                    // TODO
                    break;
                case message_type::completion:
                {
                    auto completion = static_cast<completion_message*>(val.get());
                    invoke_callback(completion);
                    break;
                }
                case message_type::cancel_invocation:
                    // Sent to server only, should not be received by client
                    throw std::runtime_error("Received unexpected message type 'CancelInvocation'.");
                case message_type::ping:
                    if (m_logger.is_enabled(trace_level::debug))
                    {
                        m_logger.log(trace_level::debug, "ping message received.");
                    }
                    break;
                case message_type::close:
                    // TODO
                    break;
                default:
                    throw std::runtime_error("unknown message type '" + std::to_string(static_cast<int>(val->message_type)) + "' received");
                    break;
                }
            }
        }
        catch (const std::exception &e)
        {
            if (m_logger.is_enabled(trace_level::error))
            {
                m_logger.log(trace_level::error, std::string("error occurred when parsing response: ")
                    .append(e.what())
                    .append(". response: ")
                    .append(response));
            }

            // TODO: Consider passing "reason" exception to stop
            m_connection->stop([](std::exception_ptr) {}, std::current_exception());
        }
    }

    bool hub_connection_impl::invoke_callback(completion_message* completion)
    {
        const char* error = nullptr;
        if (!completion->error.empty())
        {
            error = completion->error.data();
        }

        // TODO: consider transferring ownership of 'result' so that if we run user callbacks on a different thread we don't need to
        // worry about object lifetime
        if (!m_callback_manager.invoke_callback(completion->invocation_id, error, completion->result, true))
        {
            if (m_logger.is_enabled(trace_level::info))
            {
                m_logger.log(trace_level::info, std::string("no callback found for id: ").append(completion->invocation_id));
            }
        }

        return true;
    }

    void hub_connection_impl::invoke(const std::string& method_name, const std::vector<signalr::value>& arguments, std::function<void(const signalr::value&, std::exception_ptr)> callback) noexcept
    {
        const auto& callback_id = m_callback_manager.register_callback(
            create_hub_invocation_callback(m_logger, [callback](const signalr::value& result) { callback(result, nullptr); },
                [callback](const std::exception_ptr e) { callback(signalr::value(), e); }));

        invoke_hub_method(method_name, arguments, callback_id, nullptr,
            [callback](const std::exception_ptr e){ callback(signalr::value(), e); });
    }

    void hub_connection_impl::send(const std::string& method_name, const std::vector<signalr::value>& arguments, std::function<void(std::exception_ptr)> callback) noexcept
    {
        invoke_hub_method(method_name, arguments, "",
            [callback]() { callback(nullptr); },
            [callback](const std::exception_ptr e){ callback(e); });
    }

    void hub_connection_impl::invoke_hub_method(const std::string& method_name, const std::vector<signalr::value>& arguments,
        const std::string& callback_id, std::function<void()> set_completion, std::function<void(const std::exception_ptr)> set_exception) noexcept
    {
        m_logger.log(trace_level::info, std::string("invoke_hub_method: method=").append(method_name).append(", args_count=").append(std::to_string(arguments.size())));
        try
        {
            invocation_message invocation(callback_id, method_name, arguments);
            m_logger.log(trace_level::info, "invoke_hub_method: calling write_message...");
            auto message = m_protocol->write_message(&invocation);
            m_logger.log(trace_level::info, std::string("invoke_hub_method: message serialized, length=").append(std::to_string(message.length())));

            // weak_ptr prevents a circular dependency leading to memory leak and other problems
            auto weak_hub_connection = std::weak_ptr<hub_connection_impl>(shared_from_this());

            m_logger.log(trace_level::info, "invoke_hub_method: calling m_connection->send()...");
            m_connection->send(message, m_protocol->transfer_format(), [set_completion, set_exception, weak_hub_connection, callback_id](std::exception_ptr exception)
                {
                    if (exception)
                    {
                        auto hub_connection = weak_hub_connection.lock();
                        if (hub_connection)
                        {
                            hub_connection->m_callback_manager.remove_callback(callback_id);
                        }
                        set_exception(exception);
                    }
                    else
                    {
                        if (callback_id.empty())
                        {
                            // complete nonBlocking call
                            set_completion();
                        }
                    }
                });

            reset_send_ping();
        }
        catch (const std::exception& e)
        {
            m_logger.log(trace_level::error, std::string("invoke_hub_method: EXCEPTION CAUGHT: ").append(e.what()));
            m_callback_manager.remove_callback(callback_id);
            if (m_logger.is_enabled(trace_level::warning))
            {
                m_logger.log(trace_level::warning, std::string("failed to send invocation: ").append(e.what()));
            }
            set_exception(std::current_exception());
        }
    }

    connection_state hub_connection_impl::get_connection_state() const noexcept
    {
        return m_connection->get_connection_state();
    }

    std::string hub_connection_impl::get_connection_id() const
    {
        return m_connection->get_connection_id();
    }

    void hub_connection_impl::set_client_config(const signalr_client_config& config)
    {
        m_signalr_client_config = config;
        m_connection->set_client_config(config);
    }

    void hub_connection_impl::set_disconnected(const std::function<void(std::exception_ptr)>& disconnected)
    {
        m_disconnected = disconnected;
    }

    void hub_connection_impl::reset_send_ping()
    {
        auto timeMs = (std::chrono::steady_clock::now() + m_signalr_client_config.get_keepalive_interval()).time_since_epoch();
        m_nextActivationSendPing.store(std::chrono::duration_cast<std::chrono::milliseconds>(timeMs).count());
    }

    void hub_connection_impl::reset_server_timeout()
    {
        auto timeMs = (std::chrono::steady_clock::now() + m_signalr_client_config.get_server_timeout()).time_since_epoch();
        m_nextActivationServerTimeout.store(std::chrono::duration_cast<std::chrono::milliseconds>(timeMs).count());
    }

    void hub_connection_impl::start_keepalive()
    {
        m_logger.log(trace_level::info, "starting keep alive timer.");

        auto send_ping = [](std::shared_ptr<hub_connection_impl> connection)
        {
            if (!connection)
            {
                return;
            }

            if (connection->get_connection_state() != connection_state::connected)
            {
                return;
            }

            try
            {
                std::weak_ptr<hub_connection_impl> weak_connection = connection;
                connection->m_connection->send(
                    connection->m_cached_ping,
                    connection->m_protocol->transfer_format(), [weak_connection](std::exception_ptr exception)
                    {
                        auto connection = weak_connection.lock();
                        if (connection)
                        {
                            if (exception)
                            {
                                if (connection->m_logger.is_enabled(trace_level::warning))
                                {
                                    connection->m_logger.log(trace_level::warning, "failed to send ping!");
                                }
                            }
                            else
                            {
                                connection->reset_send_ping();
                            }
                        }
                    });
            }
            catch (const std::exception& e)
            {
                if (connection->m_logger.is_enabled(trace_level::warning))
                {
                    connection->m_logger.log(trace_level::warning, std::string("failed to send ping: ").append(e.what()));
                }
            }
        };

        send_ping(shared_from_this());
        reset_server_timeout();

        std::weak_ptr<hub_connection_impl> weak_connection = shared_from_this();
        timer(m_signalr_client_config.get_scheduler(),
            [send_ping, weak_connection](std::chrono::milliseconds)
            {
                auto connection = weak_connection.lock();

                if (!connection)
                {
                    return true;
                }

                if (connection->get_connection_state() != connection_state::connected)
                {
                    return true;
                }

                auto timeNowmSeconds =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

                if (timeNowmSeconds > connection->m_nextActivationServerTimeout.load())
                {
                    if (connection->get_connection_state() == connection_state::connected)
                    {
                        auto error_msg = std::string("server timeout (")
                            .append(std::to_string(connection->m_signalr_client_config.get_server_timeout().count()))
                            .append(" ms) elapsed without receiving a message from the server.");
                        if (connection->m_logger.is_enabled(trace_level::warning))
                        {
                            connection->m_logger.log(trace_level::warning, error_msg);
                        }

                        connection->m_connection->stop([](std::exception_ptr)
                            {
                            }, std::make_exception_ptr(signalr_exception(error_msg)));
                    }
                }

                if (timeNowmSeconds > connection->m_nextActivationSendPing.load())
                {
                    connection->m_logger.log(trace_level::info, "sending ping to server.");
                    send_ping(connection);
                }

                return false;
            });
    }

    // unnamed namespace makes it invisble outside this translation unit
    namespace
    {
        static std::function<void(const char* error, const signalr::value&)> create_hub_invocation_callback(const logger& logger,
            const std::function<void(const signalr::value&)>& set_result,
            const std::function<void(const std::exception_ptr)>& set_exception)
        {
            return [logger, set_result, set_exception](const char* error, const signalr::value& message)
            {
                if (error != nullptr)
                {
                    set_exception(
                        std::make_exception_ptr(
                            hub_exception(error)));
                }
                else
                {
                    set_result(message);
                }
            };
        }
    }

    void hub_connection_impl::handle_disconnection(std::exception_ptr exception)
    {
        // Log disconnection handling - this is a normal flow when connection drops
        ESP_LOGI("HUB_CONN", ">>> handle_disconnection CALLED <<<");
        m_logger.log(trace_level::info, "handle_disconnection: connection lost, analyzing reconnection options...");
        
        // start may be waiting on the handshake response so we complete it here, this no-ops if already set
        m_handshakeTask->set(std::make_exception_ptr(signalr_exception("connection closed while handshake was in progress.")));
        
        try
        {
            m_disconnect_cts->cancel();
        }
        catch (const std::exception& ex)
        {
            if (m_logger.is_enabled(trace_level::warning))
            {
                m_logger.log(trace_level::warning, std::string("disconnect event threw an exception during connection closure: ")
                    .append(ex.what()));
            }
        }

        m_callback_manager.clear("connection was stopped before invocation result was received");

        // Check if we should attempt to reconnect
        bool should_reconnect = false;
        {
            std::lock_guard<std::mutex> lock(m_reconnect_lock);
            
            bool auto_reconnect_enabled = m_signalr_client_config.is_auto_reconnect_enabled();
            bool already_reconnecting = m_reconnecting.load();
            int current_attempts = m_reconnect_attempts.load();
            int max_attempts = m_signalr_client_config.get_max_reconnect_attempts();
            
            // Use ESP_LOGI for visibility in debugging
            ESP_LOGI("HUB_CONN", "reconnect check: auto_reconnect=%s, reconnecting=%s, attempts=%d, max=%d",
                auto_reconnect_enabled ? "TRUE" : "FALSE",
                already_reconnecting ? "TRUE" : "FALSE",
                current_attempts,
                max_attempts);
            
            // Log reconnection decision factors
            m_logger.log(trace_level::info, 
                std::string("reconnect check: auto_reconnect_enabled=").append(auto_reconnect_enabled ? "true" : "false")
                .append(", already_reconnecting=").append(already_reconnecting ? "true" : "false")
                .append(", current_attempts=").append(std::to_string(current_attempts))
                .append(", max_attempts=").append(max_attempts == -1 ? "infinite" : std::to_string(max_attempts)));
            
            // Only reconnect if:
            // 1. The disconnection was due to an error (exception is not null)
            // 2. Auto-reconnect is enabled
            // 3. We're not already in a reconnecting state being cancelled
            // 4. We haven't exceeded max attempts (or max is -1 for infinite)
            if (exception != nullptr &&
                auto_reconnect_enabled &&
                !already_reconnecting &&
                (max_attempts == -1 || current_attempts < max_attempts))
            {
                should_reconnect = true;
                m_reconnecting.store(true);
                ESP_LOGI("HUB_CONN", "reconnect decision: YES - will attempt to reconnect");
                m_logger.log(trace_level::info, "reconnect decision: YES - will attempt to reconnect");
            }
            else
            {
                ESP_LOGW("HUB_CONN", "reconnect decision: NO (exception=%s, auto_reconnect=%s)", 
                    exception ? "YES" : "NO", auto_reconnect_enabled ? "TRUE" : "FALSE");
                m_logger.log(trace_level::info, "reconnect decision: NO - will not reconnect (exception=" + std::string(exception ? "yes" : "no") + ")");
            }
        }

        if (should_reconnect)
        {
            ESP_LOGI("HUB_CONN", "starting reconnection process...");
            m_logger.log(trace_level::info, "starting reconnection process...");
            
            // Call the disconnected callback to notify user
            m_disconnected(exception);
            
            // Start reconnection attempt
            attempt_reconnect();
        }
        else
        {
            // Reset reconnect state if we're not going to reconnect
            m_reconnecting.store(false);
            m_reconnect_attempts.store(0);
            
            m_logger.log(trace_level::info, "no reconnection will be attempted, calling disconnected callback");
            
            // Call the disconnected callback
            m_disconnected(exception);
        }
    }

    // Task function for reconnection - runs in its own task with dedicated stack
    // This is a friend function declared in hub_connection_impl.h
    void reconnect_task_function(void* param)
    {
        // Take ownership of the parameters
        auto* params = static_cast<reconnect_task_params*>(param);
        std::weak_ptr<hub_connection_impl> weak_connection = params->weak_connection;
        int attempt = params->attempt;
        auto reconnect_cts = params->reconnect_cts;
        delete params;  // Free the parameter struct

        // Stack monitoring - CRITICAL for debugging stack overflow
        UBaseType_t stack_start = uxTaskGetStackHighWaterMark(NULL);
        uint32_t stack_allocated = get_reconnect_stack_size();
        ESP_LOGI("HUB_CONN", "reconnect_task: started for attempt %d (stack: %u allocated, %u free)", 
                 attempt, stack_allocated, stack_start * sizeof(StackType_t));

        // Check if reconnection was cancelled
        if (reconnect_cts->is_canceled())
        {
            ESP_LOGW("HUB_CONN", "reconnect_task: cancelled before start");
            vTaskDelete(NULL);
            return;
        }

        auto connection = weak_connection.lock();
        if (!connection)
        {
            ESP_LOGW("HUB_CONN", "reconnect_task: connection destroyed");
            vTaskDelete(NULL);
            return;
        }

        // Verify connection state before attempting to start
        auto current_state = connection->get_connection_state();
        ESP_LOGI("HUB_CONN", "reconnect_task: attempt %d, current state=%d", attempt, (int)current_state);

        if (current_state != connection_state::disconnected)
        {
            ESP_LOGW("HUB_CONN", "reconnect_task: not in disconnected state (%d), aborting", (int)current_state);
            vTaskDelete(NULL);
            return;
        }

        // Use a semaphore to wait for start() to complete
        SemaphoreHandle_t done_sem = xSemaphoreCreateBinary();
        std::exception_ptr start_exception_result = nullptr;

        connection->m_logger.log(trace_level::info,
            std::string("starting reconnect attempt ").append(std::to_string(attempt)));

        // Call start() - this is safe now because we're in our own task with enough stack
        connection->start([done_sem, &start_exception_result](std::exception_ptr start_exception)
        {
            start_exception_result = start_exception;
            xSemaphoreGive(done_sem);
        });

        // Wait for start() to complete (with 60 second timeout)
        if (xSemaphoreTake(done_sem, pdMS_TO_TICKS(60000)) != pdTRUE)
        {
            ESP_LOGE("HUB_CONN", "reconnect_task: timeout waiting for start()");
            vSemaphoreDelete(done_sem);
            vTaskDelete(NULL);
            return;
        }
        vSemaphoreDelete(done_sem);

        // Re-acquire connection (it may have been destroyed during start)
        connection = weak_connection.lock();
        if (!connection)
        {
            ESP_LOGW("HUB_CONN", "reconnect_task: connection destroyed after start");
            vTaskDelete(NULL);
            return;
        }

        if (start_exception_result)
        {
            // Reconnect failed
            try {
                std::rethrow_exception(start_exception_result);
            } catch (const std::exception& e) {
                ESP_LOGE("HUB_CONN", "reconnect attempt %d failed: %s", attempt, e.what());
            }

            connection->m_logger.log(trace_level::warning,
                std::string("reconnect attempt ").append(std::to_string(attempt))
                .append(" failed"));

            // Check if we should try again
            bool should_retry = false;
            {
                std::lock_guard<std::mutex> lock(connection->m_reconnect_lock);

                int max_attempts = connection->m_signalr_client_config.get_max_reconnect_attempts();
                if (max_attempts == -1 || attempt < max_attempts)
                {
                    should_retry = true;
                }
            }

            if (should_retry)
            {
                ESP_LOGI("HUB_CONN", "reconnect: will retry (attempt %d)", attempt + 1);
                // Try again - this will create a new task
                connection->attempt_reconnect();
            }
            else
            {
                // Give up
                ESP_LOGE("HUB_CONN", "reconnect: giving up after %d attempts", attempt);
                connection->m_logger.log(trace_level::error,
                    "reconnect failed: maximum retry attempts reached");
                connection->m_reconnecting.store(false);
                connection->m_reconnect_attempts.store(0);
            }
        }
        else
        {
            // Reconnect succeeded!
            ESP_LOGI("HUB_CONN", "╔══════════════════════════════════════════════════════╗");
            ESP_LOGI("HUB_CONN", "║  ✓ RECONNECT SUCCESSFUL (attempt %d)                ║", attempt);
            ESP_LOGI("HUB_CONN", "╚══════════════════════════════════════════════════════╝");

            connection->m_logger.log(trace_level::info,
                std::string("reconnect attempt ").append(std::to_string(attempt))
                .append(" succeeded"));

            // Reset reconnect state
            connection->m_reconnecting.store(false);
            connection->m_reconnect_attempts.store(0);
        }

        ESP_LOGI("HUB_CONN", "reconnect_task: exiting");
        vTaskDelete(NULL);
    }

    void hub_connection_impl::attempt_reconnect()
    {
        auto delay = get_next_reconnect_delay();
        int attempt = m_reconnect_attempts.load() + 1;
        m_reconnect_attempts.store(attempt);

        ESP_LOGI("HUB_CONN", "attempt_reconnect: attempt=%d, delay=%lld ms", attempt, (long long)delay.count());

        m_logger.log(trace_level::info, 
            std::string("reconnect attempt ").append(std::to_string(attempt))
            .append(" will start in ").append(std::to_string(delay.count()))
            .append(" ms"));

        // Create a new cancellation token for this reconnect attempt
        m_reconnect_cts = std::make_shared<cancellation_token_source>();
        auto reconnect_cts = m_reconnect_cts;
        std::weak_ptr<hub_connection_impl> weak_connection = shared_from_this();

        // Wait for the delay before starting reconnection
        if (delay.count() > 0)
        {
            vTaskDelay(pdMS_TO_TICKS(delay.count()));
        }

        // Check if cancelled during delay
        if (reconnect_cts->is_canceled())
        {
            ESP_LOGW("HUB_CONN", "attempt_reconnect: cancelled during delay");
            return;
        }

        // Create parameters for the reconnect task
        auto* params = new reconnect_task_params{weak_connection, attempt, reconnect_cts};

        // OPTIMIZED: Use dynamic stack size based on PSRAM availability
        // CRITICAL: Must be at least 12KB for the deep call chain in start()
        uint32_t reconnect_stack = get_reconnect_stack_size();
        
        ESP_LOGI("HUB_CONN", "Creating reconnect task with %u byte stack (PSRAM: %s)", 
                 reconnect_stack, 
                 signalr::memory::is_psram_available() ? "yes" : "no");
        
        // Create a dedicated task for reconnection with sufficient stack
        BaseType_t result = xTaskCreate(
            reconnect_task_function,
            "signalr_reconn",
            reconnect_stack,
            params,
            5,     // Same priority as other SignalR tasks
            NULL
        );

        if (result != pdPASS)
        {
            ESP_LOGE("HUB_CONN", "Failed to create reconnect task (stack=%u)!", reconnect_stack);
            delete params;
            m_reconnecting.store(false);
            m_reconnect_attempts.store(0);
        }
        else
        {
            ESP_LOGD("HUB_CONN", "Created reconnect task with %u byte stack", reconnect_stack);
        }
    }

    std::chrono::milliseconds hub_connection_impl::get_next_reconnect_delay()
    {
        const auto& delays = m_signalr_client_config.get_reconnect_delays();
        int attempt = m_reconnect_attempts.load();
        
        if (delays.empty())
        {
            // Default to 0 if no delays configured
            return std::chrono::milliseconds(0);
        }
        
        // Use the delay for the current attempt, or the last delay if we've exceeded the array size
        if (attempt < delays.size())
        {
            return delays[attempt];
        }
        else
        {
            return delays.back();
        }
    }
}
