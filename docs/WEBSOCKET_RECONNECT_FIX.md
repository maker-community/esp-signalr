# 🔧 Critical Fix: WebSocket vs SignalR Reconnection Conflict

## Problem Discovered

### The Issue
After implementing SignalR-level auto-reconnect with `with_automatic_reconnect()`, we discovered that **reconnection wasn't triggering** even though the feature was properly configured.

### Root Cause
**ESP-IDF's `esp_websocket_client` has its own built-in auto-reconnect mechanism** that was interfering with SignalR's reconnection logic:

```cpp
// OLD CODE (PROBLEMATIC)
ws_cfg.reconnect_timeout_ms = INITIAL_RETRY_DELAY_MS;  // 1000ms
```

When a network error occurred:
1. WebSocket layer would detect the disconnection
2. WebSocket layer would **automatically try to reconnect** (within 1 second)
3. SignalR's `handle_disconnection()` would **never be called**
4. SignalR's auto-reconnect logic would **never execute**

This created a conflict where:
- ❌ WebSocket layer reconnects the TCP/TLS connection
- ❌ But SignalR handshake state is lost
- ❌ Connection appears "connected" but SignalR protocol is broken
- ❌ Messages fail with "Not connected" errors

### Evidence from User Logs

```
E (41793) transport_base: tcp_read error, errno=Connection reset by peer
I (41873) ESP32_WS_CLIENT: WebSocket disconnected
I (41873) websocket_client: Reconnect after 1000 ms     ← WebSocket layer reconnecting
I (60713) ESP32_WS_CLIENT: WebSocket connected          ← But no SignalR handshake!
W (59593) ESP32_WS_CLIENT: Cannot send: not connected   ← SignalR layer still broken
```

Notice:
- ✅ WebSocket reconnected at 60713ms
- ❌ But **NO** "reconnect check" logs from `handle_disconnection()`
- ❌ **NO** "reconnect attempt" logs from SignalR layer
- ❌ Connection appears connected but can't send messages

## The Fix

### Code Change
**Disable WebSocket-level auto-reconnect** to let SignalR handle reconnection completely:

```cpp
// NEW CODE (CORRECT)
ws_cfg.reconnect_timeout_ms = 0;  // 0 = disable auto-reconnect
```

Location: `src/adapters/esp32_websocket_client.cpp`, line ~233

### Why This Works

**Before (Broken):**
```
Network Error
    ↓
[WebSocket Layer] Auto-reconnects TCP connection
    ↓
[SignalR Layer] Never notified, handshake state lost
    ↓
Result: "Zombie connection" - appears connected but broken
```

**After (Fixed):**
```
Network Error
    ↓
[WebSocket Layer] Reports disconnection, stops
    ↓
[SignalR Layer] handle_disconnection() called
    ↓
[SignalR Layer] Checks if auto_reconnect_enabled
    ↓
[SignalR Layer] Calls connection->start() with proper handshake
    ↓
Result: Full reconnection with handshake negotiation
```

## Verification

After this fix, you should see these logs when disconnection occurs:

```
E (41793) transport_base: tcp_read error, errno=Connection reset by peer
I (41873) ESP32_WS_CLIENT: WebSocket disconnected
I (41874) HUB_CONN: handle_disconnection: connection lost, analyzing reconnection options...
I (41875) HUB_CONN: reconnect check: auto_reconnect_enabled=true, already_reconnecting=false, current_attempts=0, max_attempts=infinite
I (41876) HUB_CONN: reconnect decision: YES - will attempt to reconnect
I (41877) HUB_CONN: starting reconnection process...
I (41878) HUB_CONN: reconnect attempt 1 will start in 0 ms
I (41879) HUB_CONN: starting reconnect attempt 1
I (41880) ESP32_WS_CLIENT: Starting WebSocket connection to wss://...
I (42500) ESP32_WS_CLIENT: WebSocket connected
I (42501) HUB_CONN: handle_handshake: Handshake completed!
I (42502) HUB_CONN: reconnect attempt 1 succeeded
```

Key differences:
- ✅ `handle_disconnection()` is called
- ✅ Reconnect logic executes
- ✅ Full SignalR handshake completes
- ✅ Connection is fully functional after reconnect

## Impact on Configuration

This fix **does NOT require any changes to your application code**. If you already have:

```cpp
auto connection = signalr::hub_connection_builder::create(url)
    .with_automatic_reconnect()
    .build();
```

It will now work correctly! Just recompile the library with the fix.

## Testing

To verify the fix works:

1. **Trigger a disconnection:**
   - Unplug network cable
   - Turn off WiFi router
   - Stop the server
   - Use `iptables` to block traffic

2. **Expected behavior:**
   - Connection drops
   - SignalR logs show reconnection attempts with delays
   - After network restores, connection fully recovers
   - Messages can be sent successfully after reconnect

3. **Check for these logs:**
   ```
   I (xxxxx) HUB_CONN: reconnect check: auto_reconnect_enabled=true
   I (xxxxx) HUB_CONN: reconnect decision: YES - will attempt to reconnect
   I (xxxxx) HUB_CONN: reconnect attempt N will start in X ms
   I (xxxxx) HUB_CONN: reconnect attempt N succeeded
   ```

## Performance Notes

### Before Fix (Problematic)
- WebSocket reconnects in 1 second
- But connection is broken (no handshake)
- User must manually stop and restart

### After Fix (Correct)
- First retry: **immediate** (0ms delay)
- Second retry: 2 seconds delay
- Third retry: 10 seconds delay
- Fourth+ retry: 30 seconds delay

The slight increase in initial retry delay is **intentional and correct** because we're doing a proper SignalR handshake, not just reconnecting TCP.

## Related Files

This fix touches:
- `src/adapters/esp32_websocket_client.cpp` - Disabled WebSocket auto-reconnect
- `src/hub_connection_impl.cpp` - SignalR reconnection logic (already correct)
- `src/hub_connection_builder.cpp` - Builder API (already correct)

## Additional Notes

### Why Not Use Both?
You might wonder: "Why not let WebSocket reconnect fast, then SignalR can handshake?"

**Problem:** When WebSocket reconnects on its own, the connection state becomes:
- WebSocket layer: `CONNECTED`
- SignalR layer: `DISCONNECTED` (handshake never completed)

This "split-brain" state is very hard to recover from because:
- SignalR thinks it's disconnected but `start()` rejects (WebSocket already connected)
- WebSocket thinks it's connected but SignalR won't send (no handshake)
- Manual intervention required to break the deadlock

By disabling WebSocket auto-reconnect, we ensure **one source of truth**: SignalR layer controls the entire lifecycle.

## Backward Compatibility

If you **don't** use `with_automatic_reconnect()`:
- ✅ Connection behaves same as before
- ✅ Manual `start()` still works
- ✅ No automatic reconnection (expected)
- ✅ You must call `start()` manually after disconnect

If you **do** use `with_automatic_reconnect()`:
- ✅ **NOW WORKS CORRECTLY** (was broken before)
- ✅ Automatic reconnection with exponential backoff
- ✅ Proper SignalR handshake on every reconnect
- ✅ Fully functional connection after reconnect

## Summary

| Aspect | Before Fix | After Fix |
|--------|------------|-----------|
| WebSocket auto-reconnect | ✅ Enabled | ❌ Disabled |
| SignalR auto-reconnect | ❌ Never triggered | ✅ Triggers correctly |
| Handshake after reconnect | ❌ Missing | ✅ Complete |
| Connection state | 💔 Broken | ✅ Healthy |
| User code changes needed | N/A | ✅ None |

---

**Date:** 2026-01-09  
**Impact:** Critical fix for auto-reconnect feature  
**Status:** Resolved
