#pragma once

#include "esp_http_client.h"
#include <string>
#include <map>
#include <functional>
#include <memory>

namespace signalr {

// Forward declaration
struct signalr_client_config;

/**
 * HTTP response structure
 */
struct http_response {
    int status_code;
    std::string body;
    std::map<std::string, std::string> headers;
};

/**
 * ESP32 HTTP client adapter
 * Wraps ESP-IDF esp_http_client to provide SignalR-compatible interface
 */
class esp32_http_client {
public:
    using response_callback = std::function<void(const http_response&)>;
    using error_callback = std::function<void(const std::string&)>;

    explicit esp32_http_client(const signalr_client_config& config);
    ~esp32_http_client();

    // HTTP methods
    void get(const std::string& url, response_callback callback);
    void post(const std::string& url, const std::string& body, 
             const std::map<std::string, std::string>& headers,
             response_callback callback);

    // Error handling
    void set_error_callback(error_callback callback);

private:
    static esp_err_t http_event_handler(esp_http_client_event_t* evt);
    
    http_response perform_request(const std::string& url,
                                  esp_http_client_method_t method,
                                  const std::string& body = "",
                                  const std::map<std::string, std::string>& headers = {});

    error_callback m_error_callback;
    std::string m_response_buffer;
};

} // namespace signalr
