#include "StreamSwitcher.h"
#include <iostream>

StreamSwitcher::StreamSwitcher(uint32_t max_live_gap_ms, std::shared_ptr<HttpClient> http_client)
    : current_mode_(Mode::LIVE),
      max_live_gap_ms_(max_live_gap_ms),
      privacy_mode_(false),
      last_live_packet_time_(std::chrono::steady_clock::now()),
      consecutive_live_packets_(0),
      http_client_(http_client) {
}

StreamSwitcher::~StreamSwitcher() {
}

void StreamSwitcher::setPrivacyMode(bool enabled) {
    bool old_value = privacy_mode_.exchange(enabled);
    if (old_value != enabled) {
        std::cout << "[StreamSwitcher] Privacy mode "
                  << (enabled ? "ENABLED" : "DISABLED") << std::endl;
        
        // If privacy enabled, force fallback mode
        if (enabled && current_mode_.load() == Mode::LIVE) {
            switchToMode(Mode::FALLBACK);
        }
    }
}

void StreamSwitcher::updateLiveTimestamp() {
    // Don't track live packets if privacy mode is enabled
    if (privacy_mode_.load()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(timestamp_mutex_);
    last_live_packet_time_ = std::chrono::steady_clock::now();
    
    // Increment consecutive packet counter
    uint32_t prev_count = consecutive_live_packets_.load();
    consecutive_live_packets_++;
    
    // DEBUG: Log when we reach important thresholds
    uint32_t new_count = consecutive_live_packets_.load();
    if (new_count == 1) {
        std::cout << "[StreamSwitcher] DEBUG: First consecutive live packet detected" << std::endl;
    } else if (new_count == MIN_CONSECUTIVE_FOR_SWITCH) {
        std::cout << "[StreamSwitcher] DEBUG: Reached MIN_CONSECUTIVE_FOR_SWITCH ("
                  << MIN_CONSECUTIVE_FOR_SWITCH << ")" << std::endl;
    } else if (new_count % 100 == 0) {
        std::cout << "[StreamSwitcher] DEBUG: consecutive_live_packets_ = " << new_count << std::endl;
    }
}

bool StreamSwitcher::checkLiveTimeout() {
    auto time_since_live = getTimeSinceLastLive();
    
    // If currently in LIVE mode and timeout exceeded, switch to FALLBACK
    if (current_mode_.load() == Mode::LIVE &&
        time_since_live.count() > max_live_gap_ms_) {
        
        std::cout << "[StreamSwitcher] LIVE → FALLBACK (gap="
                  << time_since_live.count() << "ms, max=" << max_live_gap_ms_ << "ms)" << std::endl;
        std::cout << "[StreamSwitcher] DEBUG: Resetting consecutive_live_packets_ from "
                  << consecutive_live_packets_.load() << " to 0" << std::endl;
        
        switchToMode(Mode::FALLBACK);
        consecutive_live_packets_ = 0;
        return true;
    }
    
    return false;
}

bool StreamSwitcher::tryReturnToLive() {
    // Don't allow returning to live if privacy mode is enabled
    if (privacy_mode_.load()) {
        std::cout << "[StreamSwitcher] DEBUG: tryReturnToLive() blocked by privacy mode" << std::endl;
        return false;
    }
    
    Mode mode = current_mode_.load();
    uint32_t consec = consecutive_live_packets_.load();
    
    // DEBUG: Log the check
    static uint64_t try_return_count = 0;
    try_return_count++;
    if (try_return_count % 500 == 0) {
        std::cout << "[StreamSwitcher] DEBUG: tryReturnToLive() check #" << try_return_count
                  << " - mode=" << (mode == Mode::LIVE ? "LIVE" : "FALLBACK")
                  << ", consecutive_live_packets=" << consec
                  << ", MIN_CONSECUTIVE=" << MIN_CONSECUTIVE_FOR_SWITCH << std::endl;
    }
    
    // Only switch back if we're in FALLBACK mode and have enough consecutive packets
    if (mode == Mode::FALLBACK && consec >= MIN_CONSECUTIVE_FOR_SWITCH) {
        
        std::cout << "[StreamSwitcher] FALLBACK → LIVE (consecutive packets="
                  << consec << ")" << std::endl;
        
        switchToMode(Mode::LIVE);
        consecutive_live_packets_ = 0;
        return true;
    }
    
    return false;
}

void StreamSwitcher::setMode(Mode mode) {
    // If privacy mode is enabled and trying to set LIVE, force FALLBACK
    if (privacy_mode_.load() && mode == Mode::LIVE) {
        std::cout << "[StreamSwitcher] Cannot set LIVE mode while privacy mode is enabled" << std::endl;
        mode = Mode::FALLBACK;
    }
    
    Mode old_mode = current_mode_.load();
    if (old_mode != mode) {
        switchToMode(mode);
        consecutive_live_packets_ = 0;
    }
}

void StreamSwitcher::switchToMode(Mode mode) {
    Mode old_mode = current_mode_.exchange(mode);
    
    if (old_mode != mode) {
        std::cout << "[StreamSwitcher] Mode set to "
                  << (mode == Mode::LIVE ? "LIVE" : "FALLBACK") << std::endl;
        
        // Notify controller via HTTP
        if (http_client_) {
            if (mode == Mode::LIVE) {
                http_client_->notifySceneLive();
            } else {
                http_client_->notifySceneFallback();
            }
        }
        
        // Call mode change callback
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (mode_change_callback_) {
                mode_change_callback_(mode);
            }
        }
    }
}

std::chrono::milliseconds StreamSwitcher::getTimeSinceLastLive() const {
    std::lock_guard<std::mutex> lock(timestamp_mutex_);
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_live_packet_time_
    );
}

void StreamSwitcher::setModeChangeCallback(ModeChangeCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    mode_change_callback_ = std::move(callback);
}