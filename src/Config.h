#pragma once

#include <string>
#include <cstdint>
#include <cstdlib>

class Config {
public:
    // Load configuration from YAML file
    // Environment variables override YAML values if set
    bool loadFromFile(const std::string& filename);
    
    // Core configuration values
    uint16_t getLiveUdpPort() const { return live_udp_port_; }
    uint16_t getFallbackUdpPort() const { return fallback_udp_port_; }
    std::string getRtmpUrl() const { return rtmp_url_; }
    uint32_t getMaxLiveGapMs() const { return max_live_gap_ms_; }
    std::string getLogLevel() const { return log_level_; }
    
    // Drone input configuration
    std::string getDroneRtmpUrl() const { return drone_rtmp_url_; }
    std::string getInputSourceFile() const { return input_source_file_; }
    
    // Buffer and latency configuration values
    // These can be overridden via environment variables
    uint32_t getUdpRcvbufSize() const { return udp_rcvbuf_size_; }
    uint32_t getTsQueueSize() const { return ts_queue_size_; }
    uint32_t getRtmpPacingUs() const { return rtmp_pacing_us_; }
    uint32_t getMinConsecutiveForSwitch() const { return min_consecutive_for_switch_; }
    uint32_t getLiveIdrTimeoutMs() const { return live_idr_timeout_ms_; }
    uint32_t getFallbackIdrTimeoutMs() const { return fallback_idr_timeout_ms_; }
    
    // Print configuration
    void print() const;
    
private:
    // Helper to get environment variable with default value
    static uint32_t getEnvUint32(const char* name, uint32_t default_value);
    
    // Core settings
    uint16_t live_udp_port_ = 10000;
    uint16_t fallback_udp_port_ = 10001;
    std::string rtmp_url_;
    uint32_t max_live_gap_ms_ = 2000;
    std::string log_level_ = "INFO";
    
    // Drone input settings
    std::string drone_rtmp_url_ = "rtmp://nginx-rtmp:1935/publish/drone";
    std::string input_source_file_ = "/app/shared/input_source.json";
    
    // Buffer and latency settings (with defaults matching .env documentation)
    uint32_t udp_rcvbuf_size_ = 262144;          // UDP_RCVBUF_SIZE: 256KB default
    uint32_t ts_queue_size_ = 5000;              // TS_QUEUE_SIZE: 5000 packets default
    uint32_t rtmp_pacing_us_ = 10;               // RTMP_PACING_US: 10 microseconds default
    uint32_t min_consecutive_for_switch_ = 10;  // MIN_CONSECUTIVE_FOR_SWITCH: 10 packets
    uint32_t live_idr_timeout_ms_ = 10000;      // LIVE_IDR_TIMEOUT_MS: 10 seconds
    uint32_t fallback_idr_timeout_ms_ = 2000;   // FALLBACK_IDR_TIMEOUT_MS: 2 seconds
};