# SignalR Auto-Reconnect Feature

## 概述

此ESP32 SignalR客户端现在支持自动重连功能，类似于JavaScript和C# SignalR客户端的实现。当连接意外断开时（例如网络问题），客户端将自动尝试重新连接到服务器。

## 功能特性

- **指数退避重连策略**：使用可配置的延迟数组进行重连尝试
- **可配置的最大重试次数**：支持有限次数重试或无限重试
- **跳过协商支持**：重连时可以跳过协商步骤（适用于WebSocket Only场景）
- **重连状态回调**：通过disconnected回调通知应用程序连接状态
- **优雅的停止机制**：调用stop()时会取消所有进行中的重连尝试

## Configuration Options

### Using `with_automatic_reconnect()` (Recommended - Similar to C# and JS)

The easiest way to enable auto-reconnect is using the builder pattern, just like the official C# and JavaScript clients:

```cpp
// Default reconnect delays: 0, 2, 10, 30 seconds
auto connection = hub_connection_builder()
    .with_url("wss://your-server.com/hub")
    .skip_negotiation()
    .with_automatic_reconnect()  // Enable with default delays
    .build();

// Or with custom delays
std::vector<std::chrono::milliseconds> custom_delays = {
    std::chrono::seconds(0),      // 1st reconnect: immediate
    std::chrono::seconds(1),      // 2nd reconnect: after 1 second
    std::chrono::seconds(5),      // 3rd reconnect: after 5 seconds
    std::chrono::seconds(15),     // 4th reconnect: after 15 seconds
    std::chrono::seconds(30)      // 5th+ reconnects: after 30 seconds
};

auto connection = hub_connection_builder()
    .with_url("wss://your-server.com/hub")
    .with_automatic_reconnect(custom_delays)
    .build();
```

### Alternative: Using `signalr_client_config` (Advanced)

For more fine-grained control, you can also configure reconnect through `signalr_client_config`:

```cpp
signalr_client_config config;
config.enable_auto_reconnect(true);
config.set_reconnect_delays(custom_delays);
config.set_max_reconnect_attempts(-1);  // -1 = infinite retries

connection.set_client_config(config);
```

## Usage Examples

### Basic Example (Skip Negotiation + Auto-Reconnect)

```cpp
#include "hub_connection_builder.h"

void setup_signalr_connection()
{
    // Create connection with auto-reconnect (matches C# and JS API)
    auto connection = hub_connection_builder()
        .with_url("wss://your-server.com/signalrhub")
        .skip_negotiation()  // Skip negotiation for WebSocket-only
        .with_automatic_reconnect()  // Enable with default delays
        .build();
    
    // Set disconnected callback
    connection.set_disconnected([](std::exception_ptr ex) {
        try {
            if (ex) {
                std::rethrow_exception(ex);
            }
        } catch (const std::exception& e) {
            ESP_LOGW("SIGNALR", "Connection lost: %s", e.what());
            ESP_LOGI("SIGNALR", "Auto-reconnect will attempt to reconnect...");
        }
    });
    
    // Register server method handlers
    connection.on("ServerMethod", [](const std::vector<signalr::value>& args) {
        ESP_LOGI("SIGNALR", "Received message from server");
        // 处理服务器消息
    });
    
    // 启动连接
    connection.start([](std::exception_ptr ex) {
        if (ex) {
            ESP_LOGE("SIGNALR", "Failed to start connection");
        } else {
            ESP_LOGI("SIGNALR", "Connection started successfully");
        }
    });
}
```

### 完整示例（包括重连事件处理）

```cpp
#include "hub_connection_builder.h"
#include "signalr_client_config.h"
#include "esp_log.h"

static const char* TAG = "SIGNALR";
static std::atomic<bool> is_connected(false);
static std::atomic<int> reconnect_count(0);

void setup_signalr_with_reconnect()
{
    // 配置重连参数
    signalr_client_config config;
    config.enable_auto_reconnect(true);
    config.set_max_reconnect_attempts(-1); // 无限重试
    
    // 自定义重连延迟：指数退避策略
    std::vector<std::chrono::milliseconds> delays;
    for (int i = 0; i < 10; i++) {
        delays.push_back(std::chrono::seconds(std::min(2 << i, 60))); // 最多60秒
    }
    config.set_reconnect_delays(delays);
    
    // 设置超时参数
    config.set_handshake_timeout(std::chrono::seconds(15));
    config.set_server_timeout(std::chrono::seconds(30));
    config.set_keepalive_interval(std::chrono::seconds(15));
    
    // 创建连接
    auto connection = hub_connection_builder()
        .with_url("wss://your-server.com/signalrhub")
        .skip_negotiation()
        .build();
    
    connection.set_client_config(config);
    
    // 设置断连回调
    connection.set_disconnected([](std::exception_ptr ex) {
        is_connected.store(false);
        reconnect_count.fetch_add(1);
        
        try {
            if (ex) {
                std::rethrow_exception(ex);
            }
            ESP_LOGW(TAG, "Connection closed (reconnect count: %d)", reconnect_count.load());
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "Connection error: %s (reconnect count: %d)", 
                     e.what(), reconnect_count.load());
        }
        
        ESP_LOGI(TAG, "Auto-reconnect is active, waiting for reconnection...");
    });
    
    // 注册服务器方法
    connection.on("Notification", [](const std::vector<signalr::value>& args) {
        if (!args.empty() && args[0].is_string()) {
            ESP_LOGI(TAG, "Notification: %s", args[0].as_string().c_str());
        }
    });
    
    connection.on("UpdateData", [](const std::vector<signalr::value>& args) {
        ESP_LOGI(TAG, "Data update received");
        // 处理数据更新
    });
    
    // 启动连接
    connection.start([](std::exception_ptr ex) {
        if (ex) {
            try {
                std::rethrow_exception(ex);
            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "Start failed: %s", e.what());
            }
            is_connected.store(false);
        } else {
            ESP_LOGI(TAG, "Connected successfully!");
            is_connected.store(true);
            reconnect_count.store(0); // 重置重连计数
        }
    });
}
```

## 工作原理

### 重连流程

1. **检测断连**：当底层WebSocket连接断开时，触发`set_disconnected`回调
2. **判断是否重连**：检查是否启用自动重连、是否达到最大重试次数
3. **延迟重连**：根据当前重试次数，使用对应的延迟时间
4. **尝试重连**：调用`start()`方法尝试重新建立连接
5. **重连成功/失败**：
   - 成功：重置重连计数器，恢复正常通信
   - 失败：如果未达到最大重试次数，继续下一次重连尝试

### 指数退避策略

默认的重连延迟遵循指数退避原则：
- 第1次：0秒（立即重连）
- 第2次：2秒后
- 第3次：10秒后
- 第4次及以后：30秒后

这种策略可以：
- 快速恢复短暂的网络中断
- 避免在长时间网络故障时过度消耗资源
- 给服务器恢复时间

## 最佳实践

### 1. 跳过协商的场景

如果您的服务器仅支持WebSocket传输，建议跳过协商：

```cpp
auto connection = hub_connection_builder()
    .with_url("wss://your-server.com/signalrhub")
    .skip_negotiation()  // 跳过协商，直接使用WebSocket
    .build();
```

**优点**：
- 减少连接建立时间
- 避免HTTP协商请求失败导致的连接问题
- 简化重连流程

### 2. pthread栈大小配置

如文档中提到的，ESP32上的pthread异常展开需要足够的栈空间。建议配置：

```cpp
// 在menuconfig中设置或代码中设置
#define PTHREAD_STACK_SIZE (16 * 1024)  // 16KB
```

### 3. 监控重连状态

使用`set_disconnected`回调来监控连接状态：

```cpp
connection.set_disconnected([](std::exception_ptr ex) {
    // 记录断连原因
    // 更新UI状态
    // 通知其他系统组件
});
```

### 4. 合理设置重试参数

根据您的应用场景调整重连参数：

- **实时应用**（如IoT控制）：短延迟、多次重试
  ```cpp
  config.set_max_reconnect_attempts(20);
  config.set_reconnect_delays({0s, 1s, 2s, 5s, 10s});
  ```

- **监控应用**（如数据采集）：中等延迟、无限重试
  ```cpp
  config.set_max_reconnect_attempts(-1);  // 无限重试
  config.set_reconnect_delays({0s, 5s, 15s, 30s, 60s});
  ```

- **批处理应用**：长延迟、有限重试
  ```cpp
  config.set_max_reconnect_attempts(5);
  config.set_reconnect_delays({10s, 30s, 60s, 120s});
  ```

## 注意事项

### 1. 资源管理

- 重连过程中不会保留待发送的消息队列
- 应用层需要自行处理消息的重发逻辑
- 重连期间的`invoke()`调用会失败

### 2. 状态同步

- 重连成功后，服务器端的订阅状态可能已丢失
- 建议在连接成功回调中重新订阅服务器方法
- 考虑实现应用层的状态恢复机制

### 3. 手动停止

调用`stop()`会立即取消所有重连尝试：

```cpp
connection.stop([](std::exception_ptr ex) {
    ESP_LOGI(TAG, "Connection stopped");
});
```

### 4. 线程安全

- 所有公共API都是线程安全的
- 回调函数在内部线程中执行，注意同步访问共享资源

## 故障排查

### 问题1：重连不起作用

**检查项**：
- 确认已调用`config.enable_auto_reconnect(true)`
- 确认配置已应用：`connection.set_client_config(config)`
- 检查是否达到最大重试次数

### 问题2：重连太频繁

**解决方案**：
```cpp
// 增加延迟间隔
std::vector<std::chrono::milliseconds> delays = {
    std::chrono::seconds(5),
    std::chrono::seconds(10),
    std::chrono::seconds(30),
    std::chrono::minutes(1)
};
config.set_reconnect_delays(delays);
```

### 问题3：设备重启

如果在重连时设备重启，可能是栈溢出。参考 [PTHREAD_STACK_FIX.md](PTHREAD_STACK_FIX.md) 增加pthread栈大小。

## 参考资料

- [SignalR JavaScript Client Auto-Reconnect](https://docs.microsoft.com/en-us/aspnet/core/signalr/javascript-client#reconnect-clients)
- [SignalR .NET Client Auto-Reconnect](https://docs.microsoft.com/en-us/aspnet/core/signalr/dotnet-client#handle-lost-connection)
- [PTHREAD_STACK_FIX.md](PTHREAD_STACK_FIX.md) - ESP32 pthread栈大小配置

## 版本历史

- **v1.0.0** - 初始实现
  - 基础自动重连功能
  - 指数退避策略
  - 跳过协商支持
  - 可配置的重试参数
