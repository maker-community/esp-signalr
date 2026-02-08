#include "esp32_http_client.h"
#include "signalr_client_config.h"
#include "esp_log.h"
#include "cancellation_token_source.h"
#include <atomic>
#include <cstring>
#include <stdexcept>

static const char* TAG = "ESP32_HTTP_CLIENT";

namespace signalr {

esp32_http_client::esp32_http_client(const signalr_client_config& config) {
    // Configuration can be used for future enhancements
}

esp32_http_client::~esp32_http_client() {
}

void esp32_http_client::send(const std::string& url, http_request& request,
                            std::function<void(const http_response&, std::exception_ptr)> callback,
                            cancellation_token token) {
    ESP_LOGI(TAG, "HTTP %s request to: %s", 
            request.method == http_method::GET ? "GET" : "POST", url.c_str());
    
    try {
        if (token.is_canceled()) {
            throw canceled_exception();
        }

        http_response response = perform_request(url, request.method, request.content, 
                                                request.headers, request.timeout, token);
        callback(response, nullptr);
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "HTTP request failed: %s", e.what());
        callback(http_response(), std::make_exception_ptr(e));
    }
}

http_response esp32_http_client::perform_request(const std::string& url,
                                                 http_method method,
                                                 const std::string& content,
                                                 const std::map<std::string, std::string>& headers,
                                                 std::chrono::seconds timeout,
                                                 cancellation_token token) {
    http_response response;
    m_response_buffer.clear();

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = (method == http_method::GET) ? HTTP_METHOD_GET : HTTP_METHOD_POST;
    config.timeout_ms = static_cast<int>(timeout.count() * 1000);
    config.event_handler = http_event_handler;
    config.user_data = &m_response_buffer;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        throw std::runtime_error("Failed to initialize HTTP client");
    }

    auto cleanup_flag = std::make_shared<std::atomic<bool>>(false);

    token.register_callback([client, cleanup_flag]()
    {
        if (!cleanup_flag->load(std::memory_order_acquire))
        {
            esp_http_client_close(client);
        }
    });

    if (token.is_canceled())
    {
        esp_http_client_cleanup(client);
        throw canceled_exception();
    }

    // Set headers
    for (const auto& header : headers) {
        esp_http_client_set_header(client, header.first.c_str(), header.second.c_str());
    }

    // Set body for POST requests
    if (method == http_method::POST && !content.empty()) {
        esp_http_client_set_post_field(client, content.c_str(), content.length());
    }

    // Perform request
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        response.status_code = esp_http_client_get_status_code(client);
        response.content = m_response_buffer;
        
        ESP_LOGI(TAG, "HTTP Status: %d, Response length: %d", 
                response.status_code, m_response_buffer.length());
    } else {
        // Set cleanup flag BEFORE cleanup to prevent cancellation callback from accessing freed client
        cleanup_flag->store(true, std::memory_order_release);
        esp_http_client_cleanup(client);
        throw std::runtime_error("HTTP request failed: " + std::string(esp_err_to_name(err)));
    }

    cleanup_flag->store(true, std::memory_order_release);
    esp_http_client_cleanup(client);
    if (token.is_canceled())
    {
        throw canceled_exception();
    }
    return response;
}

esp_err_t esp32_http_client::http_event_handler(esp_http_client_event_t* evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0 && evt->user_data) {
                auto* buffer = static_cast<std::string*>(evt->user_data);
                buffer->append(static_cast<char*>(evt->data), evt->data_len);
                ESP_LOGD(TAG, "Received %d bytes", evt->data_len);
            }
            break;

        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP error event");
            break;

        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP connected");
            break;

        case HTTP_EVENT_HEADERS_SENT:
            ESP_LOGD(TAG, "HTTP headers sent");
            break;

        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "Header: %s: %s", evt->header_key, evt->header_value);
            break;

        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP request finished");
            break;

        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP disconnected");
            break;

        default:
            break;
    }
    return ESP_OK;
}

} // namespace signalr
