# 故障排除指南 - 自动重连

## 问题：自动重连不工作

### 症状
- 连接断开但不会重新连接
- 日志显示 "Not connected, cannot send" 但没有重连尝试
- 没有 "reconnect attempt X will start in Y ms" 消息

### 常见原因

#### 1. 未启用自动重连（最常见）
**问题：** 构建连接时没有调用 `with_automatic_reconnect()`。

**解决方案：**
```cpp
// ❌ 错误 - 没有自动重连
auto connection = signalr::hub_connection_builder::create(url)
    .skip_negotiation(true)
    .build();

// ✅ 正确 - 启用自动重连
auto connection = signalr::hub_connection_builder::create(url)
    .with_automatic_reconnect()  // 添加这一行！
    .skip_negotiation(true)
    .build();
```

#### 2. WebSocket 层的自动重连干扰（已修复）
**问题：** ESP-IDF 的 `esp_websocket_client` 有自己的自动重连机制，会干扰 SignalR 的重连逻辑。

**症状：**
- WebSocket 显示已重连："WebSocket connected"
- 但发送消息失败："Cannot send: not connected"
- 没有看到 SignalR 的握手日志
- 没有 "reconnect check" 或 "reconnect attempt" 日志

**原因：**
WebSocket 层自己重连了 TCP 连接，但 SignalR 的握手状态丢失了，导致连接看起来已连接，但实际协议层已损坏。

**解决方案：**
此问题已在最新版本中修复（`src/adapters/esp32_websocket_client.cpp`）。确保使用最新代码并重新编译：
```cpp
// 库已修复：禁用 WebSocket 层自动重连
ws_cfg.reconnect_timeout_ms = 0;  // 让 SignalR 层完全接管
```

详见：[WebSocket 重连冲突修复文档](WEBSOCKET_RECONNECT_FIX.md)

#### 3. 主动断开连接
**问题：** 如果你调用了 `connection->stop()`，自动重连不会触发（这是设计行为）。

**发生情况：**
- 你显式调用了 `connection->stop()`
- 你的代码在清理/关闭期间停止了连接

**解决方案：** 自动重连仅适用于意外断开（网络错误、服务器问题）。这是正确的行为。

#### 3. 达到最大重试次数
**问题：** 连接尝试重连但达到了最大尝试次数限制。

**检查你的配置：**
```cpp
auto connection = signalr::hub_connection_builder::create(url)
    .with_automatic_reconnect()  // 默认：4 次尝试 (0秒, 2秒, 10秒, 30秒)
    .build();

// 或设置无限次尝试：
auto config = signalr_client_config();
config.enable_auto_reconnect(true);
config.set_max_reconnect_attempts(-1);  // -1 = 无限次
```

### 调试步骤

#### 步骤 1: 检查是否启用了自动重连
连接断开时查找此日志：
```
reconnect check: auto_reconnect_enabled=true, already_reconnecting=false, current_attempts=0, max_attempts=infinite
reconnect decision: YES - will attempt to reconnect
```

如果看到 `auto_reconnect_enabled=false`，说明你忘记调用 `with_automatic_reconnect()`。

#### 步骤 2: 验证构建器配置
确保按正确顺序调用方法：
```cpp
auto connection = signalr::hub_connection_builder::create(url)
    .with_automatic_reconnect()  // 先启用重连
    .skip_negotiation(true)       // 然后其他设置
    .build();                     // 最后构建
```

#### 步骤 3: 检查重连延迟
默认延迟为：0秒, 2秒, 10秒, 30秒（如果 max_attempts = -1，之后重复 30秒）

如需自定义：
```cpp
auto connection = signalr::hub_connection_builder::create(url)
    .with_automatic_reconnect({
        std::chrono::milliseconds(0),      // 第1次尝试：立即
        std::chrono::milliseconds(1000),   // 第2次尝试：1秒
        std::chrono::milliseconds(5000),   // 第3次尝试：5秒
        std::chrono::milliseconds(15000)   // 第4次及以后：15秒
    })
    .skip_negotiation(true)
    .build();
```

#### 步骤 4: 监控日志
断开连接时的预期日志序列：
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

如果看不到 "reconnect check" 和 "reconnect decision" 日志，说明代码没有到达 `handle_disconnection()`。

### 完整的工作示例

```cpp
#include "hub_connection_builder.h"
#include "esp_log.h"

static const char* TAG = "SignalR";

void setup_signalr_connection()
{
    std::string url = "wss://your-server.com/hubs/chat";
    
    // 创建带自动重连的连接
    auto connection = signalr::hub_connection_builder::create(url)
        .with_automatic_reconnect({
            std::chrono::milliseconds(0),      // 立即重试
            std::chrono::milliseconds(2000),   // 2秒
            std::chrono::milliseconds(10000),  // 10秒
            std::chrono::milliseconds(30000)   // 30秒（重复）
        })
        .skip_negotiation(true)
        .build();
    
    // 可选：设置断开连接回调以监控连接状态
    connection->set_disconnected([](std::exception_ptr exception) {
        if (exception) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::exception& e) {
                ESP_LOGW(TAG, "连接丢失: %s", e.what());
            }
        } else {
            ESP_LOGI(TAG, "连接正常关闭");
        }
    });
    
    // 启动连接
    connection->start([connection](std::exception_ptr exception) {
        if (exception) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "启动连接失败: %s", e.what());
            }
        } else {
            ESP_LOGI(TAG, "连接成功！ID: %s", 
                     connection->get_connection_id().c_str());
        }
    });
}
```

### 高级配置

#### 使用 signalr_client_config 进行更多控制
```cpp
// 创建自定义配置
signalr_client_config config;
config.enable_auto_reconnect(true);
config.set_max_reconnect_attempts(-1);  // 无限重试
config.set_reconnect_delays({
    std::chrono::milliseconds(0),
    std::chrono::milliseconds(2000),
    std::chrono::milliseconds(10000),
    std::chrono::milliseconds(30000)
});

// 应用到连接
auto connection = signalr::hub_connection_builder::create(url)
    .with_automatic_reconnect()  // 必须调用此方法以启用功能
    .skip_negotiation(true)
    .build();

// 注意：构建器的 with_automatic_reconnect() 优先级更高
// 所以如果同时使用两种方法，只使用一种方式：

// 方式 1：仅构建器（推荐）
auto connection = signalr::hub_connection_builder::create(url)
    .with_automatic_reconnect(delays)
    .build();

// 方式 2：仅配置
auto config = signalr_client_config();
config.enable_auto_reconnect(true);
// 然后在构建后通过 set_client_config() 应用
```

### 仍然不工作？

1. **检查 FreeRTOS 堆栈大小：** 重连逻辑运行异步回调。确保足够的堆栈：
   ```cpp
   // 在 sdkconfig 或 menuconfig 中：
   CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192  // 如需增加
   ```

2. **验证内存：** 重连会创建定时器和任务。检查空闲堆：
   ```cpp
   ESP_LOGI(TAG, "空闲堆: %d", esp_get_free_heap_size());
   ```

3. **启用详细日志：** 设置跟踪级别以查看所有内部操作：
   ```cpp
   connection->set_trace_level(signalr::trace_level::debug);
   ```

4. **检查你的调度器：** 默认调度器使用 FreeRTOS 定时器。如果你提供了自定义调度器，请确保它正确处理定时器回调。

## 仍有问题？

请提供：
1. 从连接开始到断开的完整日志
2. 你的连接构建器代码
3. ESP-IDF 版本和 ESP32 芯片类型
4. 连接尝试前后的空闲堆大小
