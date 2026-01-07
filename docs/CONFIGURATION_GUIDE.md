# ESP32 SignalR - 快速配置指南

## 🚀 默认配置（已优化）

适合大多数 ESP32 项目，包括 xiaozhi-esp32：

```cpp
工作线程池: 2 个
工作线程栈: 3KB
调度器栈: 6KB
回调任务栈: 6KB
总内存占用: ~22KB (70% 减少)
```

## 📋 常见场景配置

### 场景 1: 内存紧张（最小配置）
```kconfig
CONFIG_SIGNALR_WORKER_POOL_SIZE=1
CONFIG_SIGNALR_WORKER_STACK_SIZE=2048
CONFIG_SIGNALR_SCHEDULER_STACK_SIZE=4096
CONFIG_SIGNALR_CALLBACK_STACK_SIZE=4096
# 内存占用: ~12KB
```

### 场景 2: 高并发（需要更多内存）
```kconfig
CONFIG_SIGNALR_WORKER_POOL_SIZE=4
CONFIG_SIGNALR_WORKER_STACK_SIZE=4096
CONFIG_SIGNALR_SCHEDULER_STACK_SIZE=8192
CONFIG_SIGNALR_CALLBACK_STACK_SIZE=8192
# 内存占用: ~32KB
```

### 场景 3: xiaozhi-esp32 集成（推荐）
```kconfig
CONFIG_SIGNALR_WORKER_POOL_SIZE=2
CONFIG_SIGNALR_WORKER_STACK_SIZE=3072
CONFIG_SIGNALR_CALLBACK_STACK_SIZE=6144
CONFIG_SIGNALR_SKIP_NEGOTIATION=n
CONFIG_SIGNALR_ENABLE_DETAILED_LOGS=n
# 内存占用: ~22KB (默认配置)
```

## 🛠️ 配置方法

### 方法 1: 使用 menuconfig
```bash
idf.py menuconfig
# 导航到: Component config -> ESP32 SignalR Client Configuration
```

### 方法 2: 直接修改 sdkconfig
```kconfig
# 添加到项目的 sdkconfig.defaults
CONFIG_SIGNALR_WORKER_POOL_SIZE=2
CONFIG_SIGNALR_CALLBACK_STACK_SIZE=6144
```

## 🔍 监控内存使用

### 检查堆内存
```cpp
#include "esp_system.h"

ESP_LOGI(TAG, "Free heap: %d", esp_get_free_heap_size());
ESP_LOGI(TAG, "Min free heap: %d", esp_get_minimum_free_heap_size());
```

### 检查栈使用
```cpp
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
ESP_LOGI(TAG, "Stack watermark: %d bytes remaining", watermark * sizeof(StackType_t));
```

## ⚠️ 故障排查

### 问题: 栈溢出 (Stack overflow)
**解决方案**:
```kconfig
# 增加回调任务栈大小
CONFIG_SIGNALR_CALLBACK_STACK_SIZE=8192
```

### 问题: 回调处理缓慢
**解决方案**:
```kconfig
# 增加工作线程数
CONFIG_SIGNALR_WORKER_POOL_SIZE=3
```

### 问题: 内存不足
**解决方案**:
1. 减少工作线程数
2. 减少栈大小
3. 启用 `CONFIG_SIGNALR_SKIP_NEGOTIATION`

## 📊 内存对比

| 配置 | 内存占用 | 适用场景 |
|------|---------|----------|
| **最小** | ~12KB | 简单应用，内存极度紧张 |
| **默认** | ~22KB | 大多数应用（推荐） |
| **高性能** | ~32KB | 高并发，内存充足 |
| **原始未优化** | ~64KB | ❌ 不推荐 |

## 🎯 xiaozhi-esp32 特别说明

使用默认配置即可，总内存占用预估:
```
SignalR: 22KB
MQTT: 10KB
WebSocket: 10KB
语音处理: 20KB
其他: 20KB
-------------
总计: 82KB (占 ESP32 可用 SRAM 约 25%)
```

安全！✅

## 📚 更多信息

- 详细优化报告: [OPTIMIZATION_REPORT.md](OPTIMIZATION_REPORT.md)
- 快速开始: [docs/QUICKSTART.md](docs/QUICKSTART.md)
- 集成指南: [docs/INTEGRATION_GUIDE.md](docs/INTEGRATION_GUIDE.md)
