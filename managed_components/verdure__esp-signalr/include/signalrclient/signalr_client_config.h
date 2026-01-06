#pragma once

#include <string>
#include <chrono>

namespace signalr {

/**
 * SignalR client configuration
 * This is a placeholder that will be replaced by the actual SignalR core implementation
 */
struct signalr_client_config {
    // Connection timeouts
    std::chrono::milliseconds handshake_timeout{5000};
    std::chrono::milliseconds disconnect_timeout{5000};
    
    // Optional proxy settings
    std::string proxy_url;
    
    // Optional headers
    // std::map<std::string, std::string> headers;
    
    signalr_client_config() = default;
};

} // namespace signalr
