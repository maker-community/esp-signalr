# Troubleshooting Guide - Auto-Reconnect

## Problem: Auto-reconnect not working

### Symptoms
- Connection drops but doesn't reconnect
- Logs show "Not connected, cannot send" but no reconnection attempts
- No "reconnect attempt X will start in Y ms" messages

### Common Causes

#### 1. Auto-Reconnect Not Enabled (Most Common)
**Problem:** You didn't call `with_automatic_reconnect()` when building the connection.

**Solution:**
```cpp
// ❌ WRONG - No auto-reconnect
auto connection = signalr::hub_connection_builder::create(url)
    .skip_negotiation(true)
    .build();

// ✅ CORRECT - With auto-reconnect
auto connection = signalr::hub_connection_builder::create(url)
    .with_automatic_reconnect()  // Add this line!
    .skip_negotiation(true)
    .build();
```

#### 2. WebSocket-Level Auto-Reconnect Interference (Fixed)
**Problem:** ESP-IDF's `esp_websocket_client` has its own auto-reconnect mechanism that interferes with SignalR's reconnection logic.

**Symptoms:**
- WebSocket shows reconnected: "WebSocket connected"
- But sending messages fails: "Cannot send: not connected"
- No SignalR handshake logs visible
- No "reconnect check" or "reconnect attempt" logs

**Cause:**
The WebSocket layer reconnects the TCP connection on its own, but SignalR's handshake state is lost, resulting in a connection that appears connected but has a broken protocol layer.

**Solution:**
This issue has been fixed in the latest version (`src/adapters/esp32_websocket_client.cpp`). Ensure you're using the latest code and recompile:
```cpp
// Library fix: Disable WebSocket-level auto-reconnect
ws_cfg.reconnect_timeout_ms = 0;  // Let SignalR layer take full control
```

See: [WebSocket Reconnect Conflict Fix Documentation](WEBSOCKET_RECONNECT_FIX.md)

#### 3. Intentional Disconnection
**Problem:** If you call `connection->stop()`, auto-reconnect won't trigger (by design).

**When it happens:**
- You explicitly called `connection->stop()`
- Your code stopped the connection during cleanup/shutdown

**Solution:** Auto-reconnect only works for unexpected disconnections (network errors, server issues). This is correct behavior.

#### 3. Max Attempts Reached
**Problem:** Connection tried to reconnect but reached the maximum attempt limit.

**Check your configuration:**
```cpp
auto connection = signalr::hub_connection_builder::create(url)
    .with_automatic_reconnect()  // Default: 4 attempts (0s, 2s, 10s, 30s)
    .build();

// Or set unlimited attempts:
auto config = signalr_client_config();
config.enable_auto_reconnect(true);
config.set_max_reconnect_attempts(-1);  // -1 = infinite
```

### Debug Steps

#### Step 1: Check if auto-reconnect is enabled
Look for this log when connection drops:
```
reconnect check: auto_reconnect_enabled=true, already_reconnecting=false, current_attempts=0, max_attempts=infinite
reconnect decision: YES - will attempt to reconnect
```

If you see `auto_reconnect_enabled=false`, you forgot to call `with_automatic_reconnect()`.

#### Step 2: Verify builder configuration
Make sure you're calling the methods in the right order:
```cpp
auto connection = signalr::hub_connection_builder::create(url)
    .with_automatic_reconnect()  // Enable reconnect first
    .skip_negotiation(true)       // Then other settings
    .build();                     // Finally build
```

#### Step 3: Check reconnection delays
Default delays are: 0s, 2s, 10s, 30s (then 30s repeatedly if max_attempts = -1)

Customize if needed:
```cpp
auto connection = signalr::hub_connection_builder::create(url)
    .with_automatic_reconnect({
        std::chrono::milliseconds(0),      // 1st attempt: immediate
        std::chrono::milliseconds(1000),   // 2nd attempt: 1 second
        std::chrono::milliseconds(5000),   // 3rd attempt: 5 seconds
        std::chrono::milliseconds(15000)   // 4th+ attempts: 15 seconds
    })
    .skip_negotiation(true)
    .build();
```

#### Step 4: Monitor logs
Expected log sequence on disconnect:
```
I (220935) WebSocket: Did not get TCP close within expected delay
I (222625) WebSocket: WebSocket error occurred
I (222626) HUB_CONN: handle_disconnection: connection lost, analyzing reconnection options...
I (222627) HUB_CONN: reconnect check: auto_reconnect_enabled=true, already_reconnecting=false, current_attempts=0, max_attempts=infinite
I (222628) HUB_CONN: reconnect decision: YES - will attempt to reconnect
I (222629) HUB_CONN: starting reconnection process...
I (222630) HUB_CONN: reconnect attempt 1 will start in 0 ms
I (222631) HUB_CONN: starting reconnect attempt 1
```

If you don't see "reconnect check" and "reconnect decision" logs, the code isn't reaching `handle_disconnection()`.

### Complete Working Example

```cpp
#include "hub_connection_builder.h"
#include "esp_log.h"

static const char* TAG = "SignalR";

void setup_signalr_connection()
{
    std::string url = "wss://your-server.com/hubs/chat";
    
    // Create connection with auto-reconnect
    auto connection = signalr::hub_connection_builder::create(url)
        .with_automatic_reconnect({
            std::chrono::milliseconds(0),      // Immediate retry
            std::chrono::milliseconds(2000),   // 2 seconds
            std::chrono::milliseconds(10000),  // 10 seconds
            std::chrono::milliseconds(30000)   // 30 seconds (repeats)
        })
        .skip_negotiation(true)
        .build();
    
    // Optional: Set up disconnected callback to monitor connection status
    connection->set_disconnected([](std::exception_ptr exception) {
        if (exception) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::exception& e) {
                ESP_LOGW(TAG, "Connection lost: %s", e.what());
            }
        } else {
            ESP_LOGI(TAG, "Connection closed normally");
        }
    });
    
    // Start the connection
    connection->start([connection](std::exception_ptr exception) {
        if (exception) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "Failed to start connection: %s", e.what());
            }
        } else {
            ESP_LOGI(TAG, "Connected successfully! ID: %s", 
                     connection->get_connection_id().c_str());
        }
    });
}
```

### Advanced Configuration

#### Using signalr_client_config for more control
```cpp
// Create custom config
signalr_client_config config;
config.enable_auto_reconnect(true);
config.set_max_reconnect_attempts(-1);  // Infinite retries
config.set_reconnect_delays({
    std::chrono::milliseconds(0),
    std::chrono::milliseconds(2000),
    std::chrono::milliseconds(10000),
    std::chrono::milliseconds(30000)
});

// Apply to connection
auto connection = signalr::hub_connection_builder::create(url)
    .with_automatic_reconnect()  // This must be called to enable the feature
    .skip_negotiation(true)
    .build();

// Note: The builder's with_automatic_reconnect() takes precedence
// So if you use both methods, use only one approach:

// Approach 1: Builder only (recommended)
auto connection = signalr::hub_connection_builder::create(url)
    .with_automatic_reconnect(delays)
    .build();

// Approach 2: Config only
auto config = signalr_client_config();
config.enable_auto_reconnect(true);
// Then apply via set_client_config() after building
```

### Still Not Working?

1. **Check FreeRTOS stack size:** Reconnection logic runs async callbacks. Ensure adequate stack:
   ```cpp
   // In sdkconfig or menuconfig:
   CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192  // Increase if needed
   ```

2. **Verify memory:** Reconnection creates timers and tasks. Check free heap:
   ```cpp
   ESP_LOGI(TAG, "Free heap: %d", esp_get_free_heap_size());
   ```

3. **Enable verbose logging:** Set trace level to see all internal operations:
   ```cpp
   connection->set_trace_level(signalr::trace_level::debug);
   ```

4. **Check your scheduler:** The default scheduler uses FreeRTOS timers. If you provided a custom scheduler, ensure it handles the timer callbacks correctly.

## Still have issues?

Please provide:
1. Complete logs from connection start through disconnection
2. Your connection builder code
3. ESP-IDF version and ESP32 chip type
4. Free heap before/after connection attempts
