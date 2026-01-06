#include "signalrclient/esp32_http_client.h"
#include "signalrclient/signalr_client_config.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "ESP32_HTTP_CLIENT";

// Configuration constants
namespace {
    constexpr uint32_t HTTP_TIMEOUT_MS = 10000;          // HTTP request timeout
    constexpr size_t HTTP_BUFFER_SIZE = 2048;            // HTTP receive buffer size
    constexpr size_t HTTP_BUFFER_SIZE_TX = 2048;         // HTTP transmit buffer size
}

namespace signalr {

esp32_http_client::esp32_http_client(const signalr_client_config& config) {
    // Configuration can be used for future enhancements
}

esp32_http_client::~esp32_http_client() {
}

void esp32_http_client::get(const std::string& url, response_callback callback) {
    ESP_LOGI(TAG, "GET request to: %s", url.c_str());
    
    try {
        http_response response = perform_request(url, HTTP_METHOD_GET);
        if (callback) {
            callback(response);
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "GET request failed: %s", e.what());
        if (m_error_callback) {
            m_error_callback(e.what());
        }
    }
}

void esp32_http_client::post(const std::string& url, const std::string& body,
                            const std::map<std::string, std::string>& headers,
                            response_callback callback) {
    ESP_LOGI(TAG, "POST request to: %s", url.c_str());
    
    try {
        http_response response = perform_request(url, HTTP_METHOD_POST, body, headers);
        if (callback) {
            callback(response);
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "POST request failed: %s", e.what());
        if (m_error_callback) {
            m_error_callback(e.what());
        }
    }
}

void esp32_http_client::set_error_callback(error_callback callback) {
    m_error_callback = callback;
}

http_response esp32_http_client::perform_request(const std::string& url,
                                                 esp_http_client_method_t method,
                                                 const std::string& body,
                                                 const std::map<std::string, std::string>& headers) {
    http_response response;
    m_response_buffer.clear();

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = method;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.event_handler = http_event_handler;
    config.user_data = &m_response_buffer;
    config.buffer_size = HTTP_BUFFER_SIZE;
    config.buffer_size_tx = HTTP_BUFFER_SIZE_TX;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        throw std::runtime_error("Failed to initialize HTTP client");
    }

    // Set headers
    for (const auto& header : headers) {
        esp_http_client_set_header(client, header.first.c_str(), header.second.c_str());
    }

    // Set body for POST requests
    if (method == HTTP_METHOD_POST && !body.empty()) {
        esp_http_client_set_post_field(client, body.c_str(), body.length());
    }

    // Perform request
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        response.status_code = esp_http_client_get_status_code(client);
        response.body = m_response_buffer;
        
        ESP_LOGI(TAG, "HTTP Status: %d, Response length: %d", 
                response.status_code, m_response_buffer.length());
    } else {
        esp_http_client_cleanup(client);
        throw std::runtime_error("HTTP request failed: " + std::string(esp_err_to_name(err)));
    }

    esp_http_client_cleanup(client);
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
