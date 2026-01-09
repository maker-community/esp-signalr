# SignalR 自动重连功能 - 快速开始

## 简介

ESP32 SignalR 客户端现已支持自动重连功能！当网络断开后，客户端会自动尝试重新连接到服务器。

## 快速使用

### 1. 基础配置（最简单）

```cpp
#include "hub_connection_builder.h"

// 创建连接并启用自动重连（使用默认延迟：0, 2, 10, 30秒）
auto connection = hub_connection_builder()
    .with_url("wss://your-server.com/hub")
    .skip_negotiation()  // 跳过协商，直接使用WebSocket
    .with_automatic_reconnect()  // 启用自动重连
    .build();

// 启动连接
connection.start([](std::exception_ptr ex) {
    if (!ex) {
        ESP_LOGI("APP", "连接成功！");
    }
});
```

就这么简单！现在当网络断开时，客户端会自动重连。

### 2. 自定义重连参数

```cpp
// 自定义重连延迟（第1次立即，第2次2秒后，第3次10秒后...）
std::vector<std::chrono::milliseconds> delays = {
    std::chrono::seconds(0),
    std::chrono::seconds(2),
    std::chrono::seconds(10),
    std::chrono::seconds(30)
};

auto connection = hub_connection_builder()
    .with_url("wss://your-server.com/hub")
    .skip_negotiation()
    .with_automatic_reconnect(delays)  // 使用自定义延迟
    .build();
```

### 3. 监听断连事件

```cpp
connection.set_disconnected([](std::exception_ptr ex) {
    try {
        if (ex) std::rethrow_exception(ex);
    } catch (const std::exception& e) {
        ESP_LOGW("APP", "连接断开: %s", e.what());
        ESP_LOGI("APP", "自动重连已启动...");
    }
});
```

## 完整示例

```cpp
void setup_signalr() {
    // 创建连接并启用自动重连
    auto connection = hub_connection_builder()
        .with_url("wss://your-server.com/hub")
        .skip_negotiation()
        .with_automatic_reconnect()  // 使用默认重连策略
        .build();
    
    // 设置断连回调
    connection.set_disconnected([](std::exception_ptr ex) {
        ESP_LOGW("APP", "连接断开，自动重连中...");
    });
    
    // 注册服务器方法
    connection.on("ServerMethod", [](const std::vector<signalr::value>& args) {
        ESP_LOGI("APP", "收到服务器消息");
    });
    
    // 启动
    connection.start([](std::exception_ptr ex) {
        if (ex) {
            ESP_LOGE("APP", "启动失败");
        } else {
            ESP_LOGI("APP", "连接成功！");
        }
    });
}
```

## 工作原理

1. **网络断开** → 触发断连回调
2. **自动重连** → 根据延迟配置等待后重试
3. **重连成功** → 恢复正常通信
4. **重连失败** → 继续尝试（直到达到最大次数或手动停止）

## 默认配置

- **重连延迟**：0秒, 2秒, 10秒, 30秒
- **最大重试次数**：无限（-1）
- **启用状态**：默认关闭（需手动启用）

## 注意事项

### ✅ 推荐做法

1. **跳过协商**：如果服务器只支持WebSocket，使用 `skip_negotiation()`
2. **监听断连**：设置 `set_disconnected` 回调以了解连接状态
3. **增大栈**：参考 `docs/PTHREAD_STACK_FIX.md` 配置足够的pthread栈

### ❌ 注意

1. 重连时不会保留未发送的消息
2. 重连成功后可能需要重新同步状态
3. 调用 `stop()` 会取消所有重连尝试

## 常见问题

**Q: 如何知道正在重连？**  
A: 通过 `set_disconnected` 回调可以知道断连，日志会显示重连进度

**Q: 重连太频繁怎么办？**  
A: 增加延迟时间：
```cpp
config.set_reconnect_delays({
    std::chrono::seconds(5),
    std::chrono::seconds(15),
    std::chrono::seconds(30),
    std::chrono::minutes(1)
});
```

**Q: 如何停止重连？**  
A: 调用 `connection.stop()` 会立即停止所有重连尝试

**Q: 设备在重连时重启？**  
A: 可能是栈溢出，增大pthread栈大小（参考 `docs/PTHREAD_STACK_FIX.md`）

## 更多信息

- 详细文档：[AUTO_RECONNECT.md](AUTO_RECONNECT.md)
- 完整示例：[examples/auto_reconnect_example.cpp](examples/auto_reconnect_example.cpp)
- pthread栈配置：[PTHREAD_STACK_FIX.md](PTHREAD_STACK_FIX.md)

## 参考

本实现参考了 SignalR 官方客户端的重连机制：
- [JavaScript Client](https://docs.microsoft.com/en-us/aspnet/core/signalr/javascript-client#reconnect-clients)
- [.NET Client](https://docs.microsoft.com/en-us/aspnet/core/signalr/dotnet-client#handle-lost-connection)
