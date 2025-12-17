#pragma once

#include <chrono>
#include <atomic>
#include <deque>
#include <mutex>

/**
 * Configuration for stream health thresholds
 */
struct StreamHealthConfig {
    // Maximum time without data before considering stream unhealthy (ms)
    int64_t max_data_age_ms = 3000;
    
    // Minimum bitrate required for healthy stream (bytes per second)
    // 0 = disabled (data age only)
    uint64_t min_bitrate_bps = 0;
    
    // Window size for bitrate calculation (seconds)
    int bitrate_window_seconds = 3;
};

/**
 * Tracks stream health metrics for an input source.
 * Thread-safe for concurrent read/write access.
 * 
 * Monitors two key health indicators:
 * 1. Data freshness - time since last packet received
 * 2. Bitrate - rolling average of bytes per second
 * 
 * Usage:
 *   health_metrics_.configure(config);
 *   // On data received:
 *   health_metrics_.recordDataReceived(num_bytes);
 *   // Check health:
 *   if (!health_metrics_.isHealthy()) { switch_to_fallback(); }
 */
class StreamHealthMetrics {
public:
    StreamHealthMetrics() = default;
    
    // Configure thresholds (call once at setup)
    void configure(const StreamHealthConfig& config) {
        config_ = config;
    }
    
    // Called when data is received
    void recordDataReceived(size_t bytes) {
        auto now = std::chrono::steady_clock::now();
        last_data_time_.store(now, std::memory_order_relaxed);
        total_bytes_received_.fetch_add(bytes, std::memory_order_relaxed);
        
        // Record in rolling window for bitrate calculation
        std::lock_guard<std::mutex> lock(window_mutex_);
        data_window_.push_back({now, bytes});
        
        // Trim old entries outside the window
        auto cutoff = now - std::chrono::seconds(config_.bitrate_window_seconds);
        while (!data_window_.empty() && data_window_.front().time < cutoff) {
            data_window_.pop_front();
        }
    }
    
    // Get time since last data in milliseconds
    int64_t getMsSinceLastData() const {
        auto last = last_data_time_.load(std::memory_order_relaxed);
        if (last == std::chrono::steady_clock::time_point{}) {
            return -1;  // No data received yet
        }
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    }
    
    // Get current bitrate in bytes per second (rolling average)
    uint64_t getCurrentBitrateBps() const {
        std::lock_guard<std::mutex> lock(window_mutex_);
        
        if (data_window_.empty()) {
            return 0;
        }
        
        auto now = std::chrono::steady_clock::now();
        auto window_start = now - std::chrono::seconds(config_.bitrate_window_seconds);
        
        uint64_t total_bytes = 0;
        for (const auto& entry : data_window_) {
            if (entry.time >= window_start) {
                total_bytes += entry.bytes;
            }
        }
        
        return total_bytes / config_.bitrate_window_seconds;
    }
    
    // Check if data is fresh (within max_data_age_ms)
    bool isDataFresh() const {
        int64_t age = getMsSinceLastData();
        if (age < 0) return false;  // No data yet
        return age < config_.max_data_age_ms;
    }
    
    // Check if bitrate meets minimum threshold
    bool isBitrateHealthy() const {
        if (config_.min_bitrate_bps == 0) {
            return true;  // Bitrate check disabled
        }
        return getCurrentBitrateBps() >= config_.min_bitrate_bps;
    }
    
    // Combined health check
    bool isHealthy() const {
        return isDataFresh() && isBitrateHealthy();
    }
    
    // Get total bytes received
    uint64_t getTotalBytesReceived() const {
        return total_bytes_received_.load(std::memory_order_relaxed);
    }
    
    // Reset all metrics (for reconnection scenarios)
    void reset() {
        last_data_time_.store({}, std::memory_order_relaxed);
        total_bytes_received_.store(0, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(window_mutex_);
        data_window_.clear();
    }
    
private:
    struct DataPoint {
        std::chrono::steady_clock::time_point time;
        size_t bytes;
    };
    
    StreamHealthConfig config_;
    std::atomic<std::chrono::steady_clock::time_point> last_data_time_{};
    std::atomic<uint64_t> total_bytes_received_{0};
    
    mutable std::mutex window_mutex_;
    std::deque<DataPoint> data_window_;
};
