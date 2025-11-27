#pragma once

#include "TimestampManager.h"
#include "HttpClient.h"
#include <chrono>
#include <atomic>
#include <mutex>
#include <memory>
#include <functional>

enum class Mode {
    LIVE,
    FALLBACK
};

class StreamSwitcher {
public:
    explicit StreamSwitcher(uint32_t max_live_gap_ms, std::shared_ptr<HttpClient> http_client = nullptr);
    ~StreamSwitcher();
    
    // Get current mode
    Mode getMode() const { return current_mode_.load(); }
    
    // Privacy mode
    bool isPrivacyMode() const { return privacy_mode_.load(); }
    void setPrivacyMode(bool enabled);
    
    // Update live packet timestamp (call when live packet received)
    void updateLiveTimestamp();
    
    // Check if we need to switch to fallback due to live timeout
    // Returns true if mode changed
    bool checkLiveTimeout();
    
    // Try to switch back to live (call when live packets resume)
    // Returns true if mode changed
    bool tryReturnToLive();
    
    // Force switch to a specific mode (also notifies controller)
    void setMode(Mode mode);
    
    // Get time since last live packet
    std::chrono::milliseconds getTimeSinceLastLive() const;
    
    // Set callback for mode changes (called after mode switch)
    using ModeChangeCallback = std::function<void(Mode new_mode)>;
    void setModeChangeCallback(ModeChangeCallback callback);
    
private:
    // Internal mode switch (notifies controller via HTTP)
    void switchToMode(Mode mode);
    
    std::atomic<Mode> current_mode_;
    uint32_t max_live_gap_ms_;
    
    // Privacy mode - when enabled, force FALLBACK and ignore live
    std::atomic<bool> privacy_mode_;
    
    mutable std::mutex timestamp_mutex_;
    std::chrono::steady_clock::time_point last_live_packet_time_;
    
    // Track consecutive live packets for switch-back stability
    std::atomic<uint32_t> consecutive_live_packets_;
    static constexpr uint32_t MIN_CONSECUTIVE_FOR_SWITCH = 10;
    
    // HTTP client for notifying controller
    std::shared_ptr<HttpClient> http_client_;
    
    // Mode change callback
    ModeChangeCallback mode_change_callback_;
    std::mutex callback_mutex_;
};