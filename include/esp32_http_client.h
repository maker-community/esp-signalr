#pragma once

#include "http_client.h"
#include "cancellation_token.h"
#include "esp_http_client.h"
#include <string>
#include <map>
#include <functional>
#include <memory>

namespace signalr {

// Forward declaration
struct signalr_client_config;

/**
 * ESP32 HTTP client adapter
 * Wraps ESP-IDF esp_http_client to provide SignalR-compatible interface
 * Implements the http_client abstract interface
 */
class esp32_http_client : public http_client {
public:
    explicit esp32_http_client(const signalr_client_config& config);
    virtual ~esp32_http_client();

    // Implement http_client interface
    void send(const std::string& url, http_request& request,
             std::function<void(const http_response&, std::exception_ptr)> callback, 
             cancellation_token token) override;

private:
    static esp_err_t http_event_handler(esp_http_client_event_t* evt);
    
    http_response perform_request(const std::string& url,
                                  http_method method,
                                  const std::string& content,
                                  const std::map<std::string, std::string>& headers,
                                  std::chrono::seconds timeout);

    std::string m_response_buffer;
};

} // namespace signalr
