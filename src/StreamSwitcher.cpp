#include "StreamSwitcher.h"
#include <iostream>

StreamSwitcher::StreamSwitcher(uint32_t max_live_gap_ms)
    : current_mode_(Mode::LIVE),
      max_live_gap_ms_(max_live_gap_ms),
      last_live_packet_time_(std::chrono::steady_clock::now()),
      consecutive_live_packets_(0) {
}

StreamSwitcher::~StreamSwitcher() {
}

void StreamSwitcher::updateLiveTimestamp() {
    std::lock_guard<std::mutex> lock(timestamp_mutex_);
    last_live_packet_time_ = std::chrono::steady_clock::now();
    
    // Increment consecutive packet counter
    consecutive_live_packets_++;
}

bool StreamSwitcher::checkLiveTimeout() {
    auto time_since_live = getTimeSinceLastLive();
    
    // If currently in LIVE mode and timeout exceeded, switch to FALLBACK
    if (current_mode_.load() == Mode::LIVE && 
        time_since_live.count() > max_live_gap_ms_) {
        
        std::cout << "[StreamSwitcher] LIVE → FALLBACK (gap=" 
                  << time_since_live.count() << "ms)" << std::endl;
        
        current_mode_ = Mode::FALLBACK;
        consecutive_live_packets_ = 0;
        return true;
    }
    
    return false;
}

bool StreamSwitcher::tryReturnToLive() {
    // Only switch back if we're in FALLBACK mode and have enough consecutive packets
    if (current_mode_.load() == Mode::FALLBACK &&
        consecutive_live_packets_.load() >= MIN_CONSECUTIVE_FOR_SWITCH) {
        
        std::cout << "[StreamSwitcher] FALLBACK → LIVE (consecutive packets=" 
                  << consecutive_live_packets_.load() << ")" << std::endl;
        
        current_mode_ = Mode::LIVE;
        consecutive_live_packets_ = 0;
        return true;
    }
    
    return false;
}

void StreamSwitcher::setMode(Mode mode) {
    Mode old_mode = current_mode_.load();
    if (old_mode != mode) {
        current_mode_ = mode;
        consecutive_live_packets_ = 0;
        std::cout << "[StreamSwitcher] Mode set to " 
                  << (mode == Mode::LIVE ? "LIVE" : "FALLBACK") << std::endl;
    }
}

std::chrono::milliseconds StreamSwitcher::getTimeSinceLastLive() const {
    std::lock_guard<std::mutex> lock(timestamp_mutex_);
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_live_packet_time_
    );
}