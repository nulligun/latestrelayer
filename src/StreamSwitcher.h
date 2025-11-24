#pragma once

#include "TimestampManager.h"
#include <chrono>
#include <atomic>
#include <mutex>

enum class Mode {
    LIVE,
    FALLBACK
};

class StreamSwitcher {
public:
    explicit StreamSwitcher(uint32_t max_live_gap_ms);
    ~StreamSwitcher();
    
    // Get current mode
    Mode getMode() const { return current_mode_.load(); }
    
    // Update live packet timestamp (call when live packet received)
    void updateLiveTimestamp();
    
    // Check if we need to switch to fallback due to live timeout
    // Returns true if mode changed
    bool checkLiveTimeout();
    
    // Try to switch back to live (call when live packets resume)
    // Returns true if mode changed
    bool tryReturnToLive();
    
    // Force switch to a specific mode
    void setMode(Mode mode);
    
    // Get time since last live packet
    std::chrono::milliseconds getTimeSinceLastLive() const;
    
private:
    std::atomic<Mode> current_mode_;
    uint32_t max_live_gap_ms_;
    
    mutable std::mutex timestamp_mutex_;
    std::chrono::steady_clock::time_point last_live_packet_time_;
    
    // Track consecutive live packets for switch-back stability
    std::atomic<uint32_t> consecutive_live_packets_;
    static constexpr uint32_t MIN_CONSECUTIVE_FOR_SWITCH = 10;
};