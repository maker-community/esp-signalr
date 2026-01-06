# esp-signalr

ESP32 SignalR Client Library - A complete implementation of Microsoft SignalR client for ESP32 platform.

## Overview

This library provides a full-featured SignalR client implementation for ESP32 devices, adapted from Microsoft's official [SignalR-Client-Cpp](https://github.com/aspnet/SignalR-Client-Cpp). It enables ESP32 devices to communicate with ASP.NET Core SignalR servers for real-time bidirectional communication.

## Key Features

- ✅ Complete SignalR protocol support (Hub connections, negotiation, handshake)
- ✅ WebSocket transport using ESP-IDF native `esp_websocket_client`
- ✅ HTTP client using ESP-IDF native `esp_http_client`
- ✅ JSON serialization using `cJSON` (ESP32 native)
- ✅ FreeRTOS task scheduling (multi-threaded)
- ✅ Optimized for ESP32 memory constraints
- ✅ Support for multiple ESP32 variants (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6)

## Quick Start

See the comprehensive library in `managed_components/verdure__esp-signalr/`:

- **[README.md](managed_components/verdure__esp-signalr/README.md)** - Main library documentation
- **[QUICKSTART.md](managed_components/verdure__esp-signalr/QUICKSTART.md)** - Quick start guide
- **[INTEGRATION_GUIDE.md](managed_components/verdure__esp-signalr/INTEGRATION_GUIDE.md)** - Detailed integration guide

## 📦 Complete Example

For a complete working example project, see:
**[esp-signalr-example](https://github.com/maker-community/esp-signalr-example)**

The example includes:
- Complete ESP-IDF project setup
- WiFi connection handling
- SignalR hub connection
- Message sending/receiving
- Test server setup guide

## Installation

### ESP Component Registry (Recommended)

Add to your `main/idf_component.yml`:

```yaml
dependencies:
  verdure/esp-signalr: "^1.0.0"
```

### Manual Installation

```bash
cd your-project/managed_components
git clone https://github.com/maker-community/esp-signalr.git verdure__esp-signalr
```

## Basic Usage

```cpp
#include "signalrclient/hub_connection_builder.h"
#include "signalrclient/esp32_websocket_client.h"
#include "signalrclient/esp32_http_client.h"

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

## Requirements

- **ESP-IDF**: >= 5.0.0
- **C++ Exceptions**: Must be enabled (`CONFIG_COMPILER_CXX_EXCEPTIONS=y`)
- **Supported Boards**: ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6

## Memory Usage

- **RAM**: ~20-30KB
- **Flash**: ~50-150KB
- **Recommended**: Enable PSRAM for large messages

## Project Structure

```
esp-signalr/
├── managed_components/verdure__esp-signalr/  # Main library
│   ├── src/
│   │   ├── adapters/              # ESP32 platform adapters
│   │   │   ├── esp32_websocket_client.cpp
│   │   │   └── esp32_http_client.cpp
│   │   ├── signalrclient/         # SignalR core protocol
│   │   └── json_adapter.cpp       # cJSON adapter
│   ├── include/                   # Public headers
│   ├── third_party_code/          # URI utilities
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   ├── README.md
│   ├── QUICKSTART.md
│   ├── INTEGRATION_GUIDE.md
│   └── extract_core_files.sh
└── README.md (this file)
```

For a complete example application, see the separate repository:
[esp-signalr-example](https://github.com/maker-community/esp-signalr-example)

## Architecture

The library uses an adapter pattern to integrate SignalR protocol with ESP32 native APIs:

```
┌──────────────────────────────────────────────────────┐
│  SignalR Core Protocol (unchanged)                   │
│  - Hub Connection, Negotiation, Handshake            │
│  - JSON Protocol, Message Routing                    │
└──────────────────────────────────────────────────────┘
                          ↓ Abstract interfaces
┌──────────────────────────────────────────────────────┐
│  Platform Adapters (ESP32 implementation)            │
│  - esp32_websocket_client  → esp_websocket_client    │
│  - esp32_http_client       → esp_http_client         │
│  - JSON Adapter            → cJSON                    │
│  - Scheduler Adapter       → FreeRTOS                 │
└──────────────────────────────────────────────────────┘
```

## Development

### Building the Example

See the complete example project at:
[esp-signalr-example](https://github.com/maker-community/esp-signalr-example)

### Running Tests

Set up an ASP.NET Core SignalR test server following the instructions in the example repository:
[TEST_SERVER.md](https://github.com/maker-community/esp-signalr-example/blob/main/TEST_SERVER.md)

## Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Submit a pull request

## Credits

- Based on [Microsoft SignalR-Client-Cpp](https://github.com/aspnet/SignalR-Client-Cpp)
- Adapted for ESP32 by the maker community

## License

MIT License - See LICENSE file for details

## Support

- **Issues**: https://github.com/maker-community/esp-signalr/issues
- **Documentation**: https://github.com/maker-community/esp-signalr
- **Example Code**: https://github.com/maker-community/esp-signalr-example

## Roadmap

- [ ] Add HTTPS/TLS support
- [ ] Implement streaming support
- [ ] Add MessagePack protocol support
- [ ] Optimize memory usage further
- [ ] Add comprehensive unit tests
- [ ] Support for ESP8266

---

**Made with ❤️ by the ESP32 community**