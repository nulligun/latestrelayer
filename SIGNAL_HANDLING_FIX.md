# Signal Handling Fix for Multiplexer Container

## Problem

The multiplexer container was taking an excessively long time to stop when receiving SIGTERM signals (from `docker stop` or `docker compose stop`), often requiring Docker to force-kill it with SIGKILL (exit code 137) after 10 seconds.

## Root Causes

### Issue 1: UDP Receiver Threads Blocked in recvfrom() (CRITICAL)

The **primary issue** was in [`UDPReceiver::closeSocket()`](src/UDPReceiver.cpp:115-120). When stopping, the code only called `close(socket_fd_)`, which **does not reliably unblock** a thread waiting in `recvfrom()` on Linux. The receiver thread would remain blocked for up to 10 seconds until Docker force-killed the container.

**Symptom**: Container logs showed:
```
[Live] Socket closed, unblocking receive loop
[Container waits ~10 seconds]
exit code 137  (SIGKILL)
```

**Root Cause**: The `recvfrom()` system call blocks indefinitely, and simply closing the socket file descriptor doesn't guarantee the blocked thread will wake up immediately.

### Issue 2: Initialization Phase Not Checking Shutdown Flags

In [`Multiplexer::analyzeStreams()`](src/Multiplexer.cpp:172-203), the fallback stream wait loop ran indefinitely without checking the `running_` flag:

```cpp
// OLD CODE - BLOCKED INDEFINITELY
while (!fallback_initialized) {
    // No running_ check here!
    while (fallback_packets_analyzed < 100 && 
           fallback_queue_->pop(packet, std::chrono::milliseconds(100))) {
        // ... analyze packets
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));  // Long blocking sleep
}
```

### Issue 3: Long Sleep Operations (Minor)

Sleep operations used large intervals (1 second) without checking shutdown status, delaying responsiveness.

## Solution

### Primary Fix: `src/UDPReceiver.cpp` - Use shutdown() to Unblock recvfrom()

**The critical fix** was to call `shutdown(socket_fd_, SHUT_RDWR)` before `close()`:

```cpp
void UDPReceiver::closeSocket() {
    if (socket_fd_ >= 0) {
        // Shutdown the socket first to interrupt any blocking recvfrom() calls
        // This is critical - close() alone does NOT reliably unblock recvfrom()
        shutdown(socket_fd_, SHUT_RDWR);
        close(socket_fd_);
        socket_fd_ = -1;
    }
}
```

The `shutdown()` call **immediately interrupts** any thread blocked in `recvfrom()`, causing it to return with `EBADF` or `ECONNRESET`, allowing the thread to exit cleanly.

We also added proper error handling in the receive loop to detect shutdown:

```cpp
if (bytes_received < 0) {
    // EBADF or ECONNRESET means socket was shutdown - expected during stop
    if (errno == EBADF || errno == ECONNRESET || errno == ENOTCONN) {
        if (should_stop_.load()) {
            std::cout << "[" << name_ << "] Socket shutdown detected, exiting cleanly" << std::endl;
        }
        break;
    }
    // ... handle other errors
}
```

### Secondary Fix: `src/Multiplexer.cpp` - Make Initialization Interruptible

1. **Set `running_` flag early in initialization**
   - Moved `running_ = true` from `run()` to `initialize()` so shutdown can be detected during init phase

2. **Added shutdown checks to fallback wait loop**
   ```cpp
   // NEW CODE - RESPONSIVE TO SHUTDOWN
   while (!fallback_initialized && running_.load()) {
       // Check running_ flag in inner loop too
       while (fallback_packets_analyzed < 100 && running_.load() && 
              fallback_queue_->pop(packet, std::chrono::milliseconds(100))) {
           // ... analyze packets
       }
       
       // Check for shutdown signal
       if (!running_.load()) {
           std::cout << "[Multiplexer] Shutdown requested during initialization" << std::endl;
           return false;
       }
       
       // Sleep in 100ms increments with shutdown checks
       for (int i = 0; i < 10 && running_.load(); i++) {
           std::this_thread::sleep_for(std::chrono::milliseconds(100));
       }
   }
   ```

3. **Added shutdown checks to `analyzeLiveStreamDynamically()`**
   - Now checks `running_.load()` in the analysis loop
   - Returns false immediately if shutdown is detected

4. **Replaced long sleeps with interruptible patterns**
   - Changed 1-second sleeps to loops of 100ms sleeps with shutdown checks
   - Allows shutdown response within 100ms instead of waiting for full sleep duration

## Results

### Before Fix
- **UDP receiver thread join**: ~10 seconds (blocked in `recvfrom()`)
- **Container exit**: Code 137 (force-killed by Docker SIGKILL)
- **Initialization shutdown**: Indefinite (hung until force-kill)
- **Total shutdown time**: 10+ seconds

### After Fix
- **UDP receiver thread join**: **0ms** (instant!)
- **Container exit**: Code 0 (clean shutdown)
- **Initialization shutdown**: < 500ms
- **Runtime shutdown**: **~135ms total**
- **Total graceful shutdown**: Well within 2s `stop_grace_period`

**Actual measurements from logs:**
```
[Live] Thread joined (0ms)
[Fallback] Thread joined (0ms)
[RTMPOutput] Shutdown complete (50ms)
[Multiplexer] Shutdown complete (135ms)
```

## Signal Flow

```
Docker SIGTERM → entrypoint.sh (exec) → ts-multiplexer process
                                           ↓
                                    signalHandler()
                                           ↓
                                  Sets running_ = false
                                           ↓
                        ┌──────────────────┴──────────────────┐
                        ↓                                     ↓
            In analyzeStreams()?                    In processLoop()?
                        ↓                                     ↓
        Checks running_ every 100ms              Checks running_ every 1ms
                        ↓                                     ↓
            Exits initialization loop                Exits process loop
                        ↓                                     ↓
                        └──────────────────┬──────────────────┘
                                           ↓
                                  UDPReceiver.stop()
                                           ↓
                                shutdown(socket) then close()
                                           ↓
                            recvfrom() unblocks with error
                                           ↓
                                Thread exits immediately
                                           ↓
                                Clean shutdown in < 200ms
```

## Testing

Run the provided test script to verify the fix:

```bash
./test-shutdown.sh
```

This script tests:
1. Shutdown during initialization (before fallback stream ready)
2. Shutdown during normal runtime operation

Both should complete in ≤ 3 seconds (actual: 1-2s).

## Components Fixed and Verified

1. **UDPReceiver**: Now uses `shutdown()` before `close()` to immediately unblock `recvfrom()` - ✓ **FIXED** (was the critical issue)
2. **RTMPOutput**: Uses 1s timeout for FFmpeg SIGTERM, then SIGKILL - ✓ Good (50ms actual)
3. **Multiplexer Main Loop**: Uses 1ms queue timeouts - ✓ Good
4. **Initialization**: Now checks shutdown every 100ms - ✓ **FIXED**

## Docker Compose Configuration

The current `stop_grace_period: 2s` is appropriate and does not need adjustment. The multiplexer now shuts down well within this window (~135ms actual).

## Key Takeaway

**The lesson**: On Linux, `close()` on a socket does **not** reliably wake up threads blocked in `recvfrom()`, `recv()`, or `accept()`. Always use `shutdown(fd, SHUT_RDWR)` before `close(fd)` when you need to interrupt blocking socket operations from another thread.

## Related Files Modified

- [`src/UDPReceiver.cpp`](src/UDPReceiver.cpp) - **Critical fix**: Added `shutdown()` before `close()` and proper error handling
- [`src/Multiplexer.cpp`](src/Multiplexer.cpp) - Added shutdown checks and interruptible sleep patterns
- [`test-shutdown.sh`](test-shutdown.sh) - Test script to verify shutdown behavior
- [`SIGNAL_HANDLING_FIX.md`](SIGNAL_HANDLING_FIX.md) - This documentation