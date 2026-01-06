# Quick Start Guide - ESP32 SignalR Client

## Prerequisites

- ESP-IDF >= 5.0.0
- WiFi-enabled ESP32 board
- ASP.NET Core SignalR server (see [Test Server Setup](https://github.com/maker-community/esp-signalr-example/blob/main/TEST_SERVER.md))

## Installation

### Option 1: Using ESP Component Registry (Recommended)

Add to your `main/idf_component.yml`:

```yaml
dependencies:
  verdure/esp-signalr: "^1.0.0"
```

### Option 2: Manual Installation

```bash
cd your-project/managed_components
git clone https://github.com/maker-community/esp-signalr.git verdure__esp-signalr
```

## Configuration

Enable C++ exceptions in `sdkconfig`:

```
CONFIG_COMPILER_CXX_EXCEPTIONS=y
CONFIG_COMPILER_CXX_EXCEPTIONS_EMG_POOL_SIZE=512
```

Or via menuconfig:
```bash
idf.py menuconfig
# Navigate to: Component config → Compiler options → Enable C++ exceptions
```

## Basic Usage

### 1. Include Headers

```cpp
#include "hub_connection_builder.h"
#include "esp32_websocket_client.h"
#include "esp32_http_client.h"
```

### 2. Create Connection

```cpp
auto connection = signalr::hub_connection_builder::create("https://example.com/hub")
    .with_websocket_factory([](const signalr::signalr_client_config& config) {
        return std::make_shared<signalr::esp32_websocket_client>(config);
    })
    .with_http_client_factory([](const signalr::signalr_client_config& config) {
        return std::make_shared<signalr::esp32_http_client>(config);
    })
    .build();
```

### 3. Register Message Handlers

```cpp
connection.on("ReceiveMessage", [](const std::vector<signalr::value>& args) {
    if (args.size() >= 2) {
        ESP_LOGI("SignalR", "%s: %s", 
                 args[0].as_string().c_str(),
                 args[1].as_string().c_str());
    }
});
```

### 4. Start Connection

```cpp
connection.start([](std::exception_ptr exception) {
    if (exception) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& e) {
            ESP_LOGE("SignalR", "Connection failed: %s", e.what());
        }
    } else {
        ESP_LOGI("SignalR", "Connected successfully!");
    }
});
```

### 5. Send Messages

```cpp
std::vector<signalr::value> args;
args.push_back(signalr::value("user"));
args.push_back(signalr::value("Hello from ESP32!"));

connection.invoke("SendMessage", args, [](const signalr::value& result, 
                                           std::exception_ptr exception) {
    if (exception) {
        ESP_LOGE("SignalR", "Invoke failed");
    } else {
        ESP_LOGI("SignalR", "Message sent");
    }
});
```

### 6. Cleanup

```cpp
connection.stop([](std::exception_ptr exception) {
    ESP_LOGI("SignalR", "Disconnected");
});
```

## Complete Example

For a full working example with WiFi setup, see the separate repository:
**[esp-signalr-example](https://github.com/maker-community/esp-signalr-example)**

## Memory Considerations

- Typical RAM usage: 20-30KB
- Flash usage: 50-150KB
- For large messages, enable PSRAM: `CONFIG_SPIRAM_SUPPORT=y`

## Common Issues

### Compilation Error: "C++ exceptions not enabled"
**Solution**: Enable in sdkconfig:
```
CONFIG_COMPILER_CXX_EXCEPTIONS=y
```

### Runtime Error: "Connection failed"
**Solution**: 
1. Verify WiFi is connected
2. Check server URL is correct
3. Ensure server has CORS configured for ESP32 origin
4. Check server logs for errors

### Memory Allocation Failed
**Solution**:
1. Reduce JSON buffer sizes
2. Enable PSRAM
3. Increase task stack size
4. Monitor heap with `esp_get_free_heap_size()`

## Advanced Configuration

### Custom Timeouts

```cpp
signalr::signalr_client_config config;
config.handshake_timeout = std::chrono::milliseconds(10000);
config.disconnect_timeout = std::chrono::milliseconds(5000);

auto connection = signalr::hub_connection_builder::create(url)
    .with_config(config)
    // ... other configuration
    .build();
```

### Connection State Monitoring

```cpp
connection.set_disconnected([](std::exception_ptr exception) {
    ESP_LOGW("SignalR", "Connection lost");
    // Implement reconnection logic here
});
```

## Next Steps

- Read the [Integration Guide](INTEGRATION_GUIDE.md) for detailed implementation
- Setup a [test server](https://github.com/maker-community/esp-signalr-example/blob/main/TEST_SERVER.md) for development
- Explore the [complete example code](https://github.com/maker-community/esp-signalr-example)

## Support

For issues and questions:
- GitHub Issues: https://github.com/maker-community/esp-signalr/issues
- Documentation: https://github.com/maker-community/esp-signalr

## License

MIT License - See LICENSE file
