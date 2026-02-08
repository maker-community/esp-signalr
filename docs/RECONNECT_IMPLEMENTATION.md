# 自动重连功能实现总结

## 已完成的工作

### 1. 核心功能实现

#### 配置层 (signalr_client_config)
- ✅ 添加了自动重连开关 (`enable_auto_reconnect`)
- ✅ 添加了重连延迟配置 (`set_reconnect_delays`)
- ✅ 添加了最大重试次数配置 (`set_max_reconnect_attempts`)
- ✅ 默认值遵循指数退避策略：0秒, 2秒, 10秒, 30秒

**文件修改**：
- `include/signalr_client_config.h` - 添加公共API
- `src/signalr_client_config.cpp` - 实现配置方法

#### 连接管理层 (hub_connection_impl)
- ✅ 添加了重连状态管理（原子变量防止竞态条件）
- ✅ 实现了 `handle_disconnection()` - 处理断连事件
- ✅ 实现了 `attempt_reconnect()` - 执行重连尝试
- ✅ 实现了 `get_next_reconnect_delay()` - 计算重连延迟
- ✅ 修改了 `stop()` - 取消进行中的重连
- ✅ 修改了 `start()` - 重置重连状态

**文件修改**：
- `src/hub_connection_impl.h` - 添加私有成员和方法
- `src/hub_connection_impl.cpp` - 实现重连逻辑

### 2. 重连策略

#### 指数退避算法
```cpp
重试次数    延迟时间
1          0秒（立即）
2          2秒
3          10秒
4+         30秒
```

#### 重连触发条件
- ✅ 自动重连已启用
- ✅ 不在重连过程中
- ✅ 未达到最大重试次数（或设置为无限）
- ✅ 不是手动调用 stop() 导致的断连

### 3. 关键特性

#### 线程安全
- ✅ 使用 `std::atomic<bool>` 管理重连状态
- ✅ 使用 `std::mutex` 保护临界区
- ✅ 使用 `std::weak_ptr` 避免循环引用

#### 取消机制
- ✅ 通过 `cancellation_token_source` 取消重连
- ✅ 调用 `stop()` 会立即取消所有重连尝试
- ✅ 析构时自动清理资源

#### 跳过协商支持
- ✅ 完全兼容 `skip_negotiation()` 模式
- ✅ 重连时保持相同的协商策略

### 4. 文档

#### 完整文档 (AUTO_RECONNECT.md)
- ✅ 功能概述和特性列表
- ✅ 配置选项详细说明
- ✅ 多个使用示例
- ✅ 工作原理说明
- ✅ 最佳实践和注意事项
- ✅ 故障排查指南

#### 快速开始 (AUTO_RECONNECT_CN.md)
- ✅ 中文版快速入门
- ✅ 简化的配置示例
- ✅ 常见问题解答

#### 代码示例 (auto_reconnect_example.cpp)
- ✅ 完整的可运行示例
- ✅ 详细的注释说明
- ✅ 包含错误处理
- ✅ 演示所有主要功能

#### README 更新
- ✅ 在功能列表中添加自动重连
- ✅ 添加自动重连的快速开始示例
- ✅ 添加文档链接

### 5. 参考实现

本实现参考了以下官方SignalR客户端：

#### JavaScript客户端
- 指数退避重连策略
- 可配置的重连延迟数组
- 断连后自动尝试重连

#### C#客户端  
- NextRetryDelay 方法
- 最大重试次数控制
- 重连状态管理

## 技术细节

### 重连流程图

```
断连事件
    ↓
判断是否应重连
    ↓
是 → 计算延迟时间
    ↓
启动定时器
    ↓
等待延迟
    ↓
调用 start()
    ↓
    ├─ 成功 → 重置状态，恢复通信
    └─ 失败 → 判断是否继续
              ├─ 是 → 返回"计算延迟时间"
              └─ 否 → 放弃重连
```

### 状态机

```
初始状态：disconnected, reconnecting=false, attempts=0
              ↓
     网络断开（非手动stop）
              ↓
     reconnecting=true, attempts=0
              ↓
    ┌─────── 重连循环 ────────┐
    │                          │
    │  等待延迟 → 尝试连接     │
    │      ↓           ↓       │
    │    成功        失败      │
    │      ↓           ↓       │
    │  重置状态   检查限制    │
    │              ↓     ↓     │
    │          继续   放弃    │
    │            ↓             │
    └────────────┘             │
                               ↓
                          断连状态
```

### 内存管理

- 重连状态变量：~16字节
- 取消令牌：共享指针，多个重连共享
- 无额外的消息队列或缓存

## 使用方式

### 最简配置
```cpp
signalr_client_config config;
config.enable_auto_reconnect(true);
connection.set_client_config(config);
```

### 自定义配置
```cpp
config.enable_auto_reconnect(true);
config.set_max_reconnect_attempts(10);
config.set_reconnect_delays({0s, 2s, 5s, 10s, 30s});
```

### 监听重连事件
```cpp
connection.set_disconnected([](std::exception_ptr ex) {
    ESP_LOGW("APP", "连接断开，自动重连中...");
});
```

## 兼容性

### ✅ 完全兼容
- skip_negotiation() 模式
- 所有传输类型
- 现有的回调机制
- 现有的错误处理

### ⚠️ 行为变化
- 断连后不会立即触发用户回调（如果启用重连）
- 重连过程中 `get_connection_state()` 返回 `disconnected`

## 测试建议

### 1. 基础功能测试
```cpp
// 测试立即重连
config.set_reconnect_delays({std::chrono::seconds(0)});
// 断开网络 → 应立即重连
```

### 2. 延迟测试
```cpp
// 测试延迟重连
config.set_reconnect_delays({std::chrono::seconds(5)});
// 断开网络 → 等待5秒 → 重连
```

### 3. 重试次数测试
```cpp
// 测试最大重试
config.set_max_reconnect_attempts(3);
// 保持网络断开 → 尝试3次后放弃
```

### 4. 手动停止测试
```cpp
// 在重连过程中调用 stop()
// 应立即取消重连
connection.stop([](std::exception_ptr ex) {});
```

### 5. 并发测试
```cpp
// 在重连时调用 send() 或 invoke()
// 应返回错误，不应崩溃
```

## 已知限制

1. **消息队列**：重连时不保留未发送的消息
2. **状态同步**：重连后需要应用层重新同步状态
3. **服务器订阅**：重连后服务器端订阅可能丢失

## 未来改进建议

1. ✨ 添加重连回调（reconnecting/reconnected事件）
2. ✨ 可选的消息队列缓存
3. ✨ 自动重新订阅服务器方法
4. ✨ 连接质量统计

## 版本信息

- **实现日期**：2026-01-09
- **基于版本**：ESP32 SignalR Client v1.x
- **参考标准**：SignalR Protocol, ASP.NET Core SignalR

## 贡献者

实现基于以下项目和标准：
- Microsoft SignalR-Client-Cpp
- SignalR JavaScript Client
- SignalR .NET Client
- ASP.NET Core SignalR 文档
