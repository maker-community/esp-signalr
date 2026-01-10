// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "signalr_client_config.h"
#include "signalr_default_scheduler.h"
#include <stdexcept>

namespace signalr
{
#ifdef USE_CPPRESTSDK
    void signalr_client_config::set_proxy(const web::web_proxy &proxy)
    {
        m_http_client_config.set_proxy(proxy);
#if false
        m_websocket_client_config.set_proxy(proxy);
#endif
    }

    void signalr_client_config::set_credentials(const web::credentials &credentials)
    {
        m_http_client_config.set_credentials(credentials);
#if false
        m_websocket_client_config.set_credentials(credentials);
#endif
    }

    web::http::client::http_client_config signalr_client_config::get_http_client_config() const
    {
        return m_http_client_config;
    }

    void signalr_client_config::set_http_client_config(const web::http::client::http_client_config& http_client_config)
    {
        m_http_client_config = http_client_config;
    }

#if false
    web::websockets::client::websocket_client_config signalr_client_config::get_websocket_client_config() const noexcept
    {
        return m_websocket_client_config;
    }

    void signalr_client_config::set_websocket_client_config(const web::websockets::client::websocket_client_config& websocket_client_config)
    {
        m_websocket_client_config = websocket_client_config;
    }
#endif
#endif

    signalr_client_config::signalr_client_config()
        // NOTE: Initialization order MUST match declaration order in header file!
        : m_scheduler(nullptr)  // LAZY INIT: don't create scheduler until needed!
        , m_handshake_timeout(std::chrono::seconds(15))
        , m_server_timeout(std::chrono::seconds(30))
        , m_keepalive_interval(std::chrono::seconds(15))
        , m_auto_reconnect_enabled(false)
        , m_max_reconnect_attempts(-1) // -1 means infinite retries
    {
        // IMPORTANT: Do NOT create scheduler here!
        // Each signalr_default_scheduler creates 1 scheduler task + 2 worker tasks,
        // consuming ~12KB+ of internal SRAM. With lazy initialization, we avoid
        // creating multiple schedulers when config objects are copied/replaced.
        
        // Default reconnect delays following exponential backoff (similar to JS/C# clients)
        // 0, 2, 10, 30 seconds
        m_reconnect_delays = {
            std::chrono::seconds(0),
            std::chrono::seconds(2),
            std::chrono::seconds(10),
            std::chrono::seconds(30)
        };
    }

    const std::map<std::string, std::string>& signalr_client_config::get_http_headers() const noexcept
    {
        return m_http_headers;
    }

    std::map<std::string, std::string>& signalr_client_config::get_http_headers() noexcept
    {
        return m_http_headers;
    }

    void signalr_client_config::set_http_headers(const std::map<std::string, std::string>& http_headers)
    {
        m_http_headers = http_headers;
    }

    void signalr_client_config::set_scheduler(std::shared_ptr<scheduler> scheduler)
    {
        if (!scheduler)
        {
            return;
        }

        m_scheduler = std::move(scheduler);
    }

    // NOTE: This is non-const because of lazy initialization
    // The scheduler is created on first access to avoid memory waste
    std::shared_ptr<scheduler> signalr_client_config::get_scheduler()
    {
        // Lazy initialization: create scheduler only when first accessed
        if (!m_scheduler)
        {
            m_scheduler = std::make_shared<signalr_default_scheduler>();
        }
        return m_scheduler;
    }

    void signalr_client_config::set_handshake_timeout(std::chrono::milliseconds timeout)
    {
        if (timeout <= std::chrono::seconds(0))
        {
            throw std::runtime_error("timeout must be greater than 0.");
        }

        m_handshake_timeout = timeout;
    }

    std::chrono::milliseconds signalr_client_config::get_handshake_timeout() const noexcept
    {
        return m_handshake_timeout;
    }

    void signalr_client_config::set_server_timeout(std::chrono::milliseconds timeout)
    {
        if (timeout <= std::chrono::seconds(0))
        {
            throw std::runtime_error("timeout must be greater than 0.");
        }

        m_server_timeout = timeout;
    }

    std::chrono::milliseconds signalr_client_config::get_server_timeout() const noexcept
    {
        return m_server_timeout;
    }

    void signalr_client_config::set_keepalive_interval(std::chrono::milliseconds interval)
    {
        if (interval <= std::chrono::seconds(0))
        {
            throw std::runtime_error("interval must be greater than 0.");
        }

        m_keepalive_interval = interval;
    }

    std::chrono::milliseconds signalr_client_config::get_keepalive_interval() const noexcept
    {
        return m_keepalive_interval;
    }

    void signalr_client_config::set_reconnect_delays(const std::vector<std::chrono::milliseconds>& delays)
    {
        m_reconnect_delays = delays;
    }

    const std::vector<std::chrono::milliseconds>& signalr_client_config::get_reconnect_delays() const noexcept
    {
        return m_reconnect_delays;
    }

    void signalr_client_config::set_max_reconnect_attempts(int max_attempts)
    {
        m_max_reconnect_attempts = max_attempts;
    }

    int signalr_client_config::get_max_reconnect_attempts() const noexcept
    {
        return m_max_reconnect_attempts;
    }

    void signalr_client_config::enable_auto_reconnect(bool enable)
    {
        m_auto_reconnect_enabled = enable;
    }

    bool signalr_client_config::is_auto_reconnect_enabled() const noexcept
    {
        return m_auto_reconnect_enabled;
    }
}
