# ESP32 SignalR Client Library

A complete ESP32/ESP-IDF implementation of Microsoft SignalR client, adapted from [SignalR-Client-Cpp](https://github.com/aspnet/SignalR-Client-Cpp).

## Features

- ✅ Full SignalR protocol support (Hub connections, negotiation, handshake)
- ✅ WebSocket transport using ESP-IDF native `esp_websocket_client`
- ✅ HTTP client using ESP-IDF native `esp_http_client`
- ✅ JSON serialization using `cJSON`
- ✅ FreeRTOS task scheduling
- ✅ Optimized for ESP32 memory constraints

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│  SignalR Core Protocol (unchanged)                       │
│  - Hub Connection, Negotiation, Handshake                │
│  - JSON Protocol, Message Routing                        │
└──────────────────────────────────────────────────────────┘
                          ↓ Abstract interfaces
┌──────────────────────────────────────────────────────────┐
│  Platform Adapters (ESP32 implementation)                │
│  - esp32_websocket_client  → esp_websocket_client        │
│  - esp32_http_client       → esp_http_client             │
│  - JSON Adapter            → cJSON                        │
│  - Scheduler Adapter       → FreeRTOS                     │
└──────────────────────────────────────────────────────────┘
```

## Requirements

- ESP-IDF >= 5.0.0
- ESP32, ESP32-S2, ESP32-S3, ESP32-C3, or ESP32-C6
- C++ exceptions enabled: `CONFIG_COMPILER_CXX_EXCEPTIONS=y`

## Installation

### Using ESP Component Registry

```yaml
dependencies:
  verdure/esp-signalr: "^1.0.0"
```

### Manual Installation

```bash
cd your-project/managed_components
git clone https://github.com/maker-community/esp-signalr.git verdure__esp-signalr
```

## Quick Start

```cpp
#include "signalrclient/hub_connection_builder.h"
#include "esp32_websocket_client.h"
#include "esp32_http_client.h"

// Create connection
auto connection = signalr::hub_connection_builder::create("https://your-server.com/hub")
    .with_websocket_factory([](const signalr::signalr_client_config& config) {
        return std::make_shared<signalr::esp32_websocket_client>(config);
    })
    .with_http_client_factory([](const signalr::signalr_client_config& config) {
        return std::make_shared<signalr::esp32_http_client>(config);
    })
    .build();

// Register message handler
connection.on("ReceiveMessage", [](const std::vector<signalr::value>& args) {
    ESP_LOGI("SignalR", "Received: %s", args[0].as_string().c_str());
});

// Start connection
connection.start([](std::exception_ptr ex) {
    if (!ex) {
        ESP_LOGI("SignalR", "Connected!");
    }
});

// Send message
std::vector<signalr::value> args;
args.push_back(signalr::value("Hello from ESP32!"));
connection.invoke("SendMessage", args);
```

## Memory Usage

- RAM: ~20-30KB
- Flash: ~50-150KB
- Recommended: Enable PSRAM for large messages

## Configuration

Add to your `sdkconfig`:

```
CONFIG_COMPILER_CXX_EXCEPTIONS=y
CONFIG_COMPILER_CXX_EXCEPTIONS_EMG_POOL_SIZE=512
```

## Examples

For a complete working example, see the separate repository:
**[esp-signalr-example](https://github.com/maker-community/esp-signalr-example)**

The example includes:
- WiFi connection setup
- SignalR hub connection
- Message sending and receiving
- Error handling

## Documentation

- [Quick Start Guide](QUICKSTART.md)
- [Integration Guide](INTEGRATION_GUIDE.md)
- [Complete Example Project](https://github.com/maker-community/esp-signalr-example)
- [ASP.NET Core Test Server Setup](https://github.com/maker-community/esp-signalr-example/blob/main/TEST_SERVER.md)

## License

MIT License - See LICENSE file

## Credits

Based on [Microsoft SignalR-Client-Cpp](https://github.com/aspnet/SignalR-Client-Cpp)
