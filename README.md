# ESP32 SignalR Client Library

A complete ESP32/ESP-IDF implementation of Microsoft SignalR client, adapted from [SignalR-Client-Cpp](https://github.com/aspnet/SignalR-Client-Cpp).

## ⚡ Optimized for ESP32

- 🎯 **Memory Efficient**: ~22KB RAM (70% reduction from original port)
- 🔧 **Configurable**: Kconfig options for fine-tuning
- 🚀 **Lightweight**: Only 2 worker threads by default
- 📦 **Modular**: Optional features can be disabled
- 🛡️ **Stable**: Queue overflow protection prevents memory leaks
- 📊 **Debuggable**: Optional stack monitoring for optimization

## Features

- ✅ Full SignalR protocol support (Hub connections, negotiation, handshake)
- ✅ WebSocket transport using ESP-IDF native `esp_websocket_client`
- ✅ HTTP client using ESP-IDF native `esp_http_client`
- ✅ JSON serialization using `cJSON`
- ✅ FreeRTOS task scheduling
- ✅ Configurable memory usage via Kconfig

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
#include "hub_connection_builder.h"
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
## Configuration

### Basic Configuration

Add to your `sdkconfig`:

```
CONFIG_COMPILER_CXX_EXCEPTIONS=y
CONFIG_COMPILER_CXX_EXCEPTIONS_EMG_POOL_SIZE=512
```

### Advanced Configuration

Use `idf.py menuconfig` to customize:

```bash
idf.py menuconfig
# Navigate to: Component config -> ESP32 SignalR Client Configuration
```

Available options:
- Worker pool size (1-5, default: 2)
- Stack sizes for tasks (with usage recommendations)
- Message buffer size
- Message queue size (overflow protection)
- Enable/disable negotiation
- Enable/disable trace logging
- Enable/disable stack monitoring (development)

See [Configuration Guide](docs/CONFIGURATION_GUIDE.md) for details.

## Examples

For a complete working example, see the separate repository:
**[esp-signalr-example](https://github.com/maker-community/esp-signalr-example)**

The example includes:
- WiFi connection setup
- SignalR hub connection
- Message sending and receiving
- Error handling

## Documentation

### Getting Started
- 📖 [Quick Start Guide](docs/QUICKSTART.md) - Get running in 5 minutes
- 📖 [Integration Guide](docs/INTEGRATION_GUIDE.md) - Detailed integration steps

### Configuration & Optimization
- ⚙️ [Configuration Guide](docs/CONFIGURATION_GUIDE.md) - Memory optimization tips
- 📊 [Optimization Report](docs/OPTIMIZATION_REPORT.md) - Round 1: Basic optimizations
- 🔬 [Advanced Optimization](docs/ADVANCED_OPTIMIZATION.md) - Round 2: Conditional compilation
- ✅ [Final Optimizations](docs/FINAL_OPTIMIZATIONS.md) - Round 3: Stability & debugging

### Examples & Testing
- 💻 [Complete Example Project](https://github.com/maker-community/esp-signalr-example)
- 🧪 [ASP.NET Core Test Server Setup](https://github.com/maker-community/esp-signalr-example/blob/main/TEST_SERVER.md)

## Memory Usage

| Configuration | RAM Usage | Use Case |
|--------------|-----------|----------|
| **Minimal** | ~12KB | Memory constrained |
| **Default** | ~22KB | Recommended for most projects |
| **High Performance** | ~32KB | High concurrency needs |

See [Optimization Report](docs/OPTIMIZATION_REPORT.md) for detailed analysis.

## License

MIT License - See LICENSE file

## Credits

Based on [Microsoft SignalR-Client-Cpp](https://github.com/aspnet/SignalR-Client-Cpp)
