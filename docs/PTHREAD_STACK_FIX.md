# pthread栈溢出紧急修复指南

## ⚠️ CRITICAL: pthread任务栈太小！

### 崩溃信息
```
Guru Meditation Error: Core 0 panic'ed (Stack protection fault).
Detected in task "pthread" at 0x403801da
Stack bounds: 0x3fc97cb0 - 0x3fc982b0  (仅 2.6KB!)
```

**根本原因**：ESP-IDF默认pthread栈只有**3KB**，C++异常需要4-5KB！

## 🔥 立即修复

### 方案1：增加pthread默认栈大小（推荐）

在项目根目录运行：
```bash
idf.py menuconfig
```

导航到：
```
Component config → 
  Pthread → 
    Default task stack size (PTHREAD_TASK_STACK_SIZE_DEFAULT)
```

**将默认值从 3072 改为 8192** (8KB)

或者直接在 `sdkconfig` 中添加/修改：
```ini
CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=8192
```

### 方案2：禁用C++异常（极限优化）

如果内存极度紧张，可以禁用异常：
```bash
idf.py menuconfig
```

导航到：
```
Compiler options → 
  Enable C++ exceptions (COMPILER_CXX_EXCEPTIONS)
  [✗] 取消勾选
```

或在 `sdkconfig` 中：
```ini
# CONFIG_COMPILER_CXX_EXCEPTIONS is not set
```

⚠️ **警告**：禁用异常会破坏SignalR SDK，需要大量代码重构！

## 🎯 推荐配置

### 最佳配置（稳定性优先）
```ini
# Pthread配置
CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=8192

# SignalR配置
CONFIG_SIGNALR_WORKER_STACK_SIZE=6144
CONFIG_SIGNALR_CALLBACK_STACK_SIZE=6144
CONFIG_SIGNALR_MAX_CALLBACK_TASKS=3
```

### 平衡配置（内存有限）
```ini
# Pthread配置
CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=6144

# SignalR配置  
CONFIG_SIGNALR_WORKER_STACK_SIZE=6144
CONFIG_SIGNALR_CALLBACK_STACK_SIZE=4096
CONFIG_SIGNALR_MAX_CALLBACK_TASKS=2
```

### 极限配置（不推荐）
```ini
# Pthread配置
CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=4096

# SignalR配置
CONFIG_SIGNALR_WORKER_STACK_SIZE=4096
CONFIG_SIGNALR_CALLBACK_STACK_SIZE=3072
CONFIG_SIGNALR_MAX_CALLBACK_TASKS=1
```

## 📊 内存成本对比

| 配置 | Pthread | Worker | 回调 | 总计 | 稳定性 |
|------|---------|--------|------|------|--------|
| **推荐** | 8KB | 6KB×2 | 6KB×3 | **38KB** | ⭐⭐⭐⭐⭐ |
| 平衡 | 6KB | 6KB×2 | 4KB×2 | 26KB | ⭐⭐⭐⭐ |
| 极限 | 4KB | 4KB×2 | 3KB×1 | 15KB | ⭐⭐ |
| 当前（崩溃） | **3KB** | 6KB×2 | 6KB×3 | 33KB | ❌ |

## 🔍 诊断

### 检查当前配置
```bash
grep PTHREAD_TASK_STACK sdkconfig
grep SIGNALR_ sdkconfig
```

### 查看栈使用
在日志中搜索：
```
Worker task exiting - stack: XXX bytes used, YYY bytes free
Callback task final statistics: XXX bytes used, YYY bytes free
```

如果"bytes free"低于512，**必须增加栈！**

## 🚀 验证修复

重新编译并测试：
```bash
idf.py build flash monitor
```

观察日志：
1. ✅ 不再出现"Stack protection fault"
2. ✅ 栈使用统计显示有足够余量
3. ✅ 网络断开重连不会崩溃

## 💡 为什么pthread会执行SignalR代码？

ESP-IDF的某些组件（如WebSocket客户端的事件处理）**可能在pthread上下文中执行**：

```
ESP WebSocket库内部事件 
  ↓
调用用户回调（在pthread中）
  ↓
SignalR代码执行
  ↓
C++异常展开
  ↓
pthread栈溢出！💥
```

## ✅ 已完成的优化

1. ✅ **预创建9个静态异常对象**
   - 启动时创建一次
   - 运行时零开销

2. ✅ **Worker栈增加到6KB**

3. ✅ **回调任务栈增加到6KB**

4. ✅ **WebSocket任务栈增加到8KB**

## ❌ 仍需手动配置

- ❌ **pthread栈** - 需要在`menuconfig`或`sdkconfig`中设置

## 📝 总结

### 立即行动
```bash
# 1. 配置pthread栈
idf.py menuconfig
# 设置 Pthread → Default task stack size = 8192

# 2. 重新编译
idf.py build

# 3. 烧录测试
idf.py flash monitor
```

### 期望结果
- ✅ pthread栈从3KB → 8KB
- ✅ 完全消除栈溢出
- ✅ 稳定运行不崩溃
- ✅ 内存总开销：+5KB

---

**更新日期**: 2026-01-09  
**严重程度**: CRITICAL  
**状态**: ⚠️ 需要手动配置pthread栈
