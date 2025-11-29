#pragma once

#include <string>
#include <cstdint>

class Config {
public:
    // Load configuration from YAML file
    bool loadFromFile(const std::string& filename);
    
    // Configuration values
    uint16_t getLiveUdpPort() const { return live_udp_port_; }
    uint16_t getFallbackUdpPort() const { return fallback_udp_port_; }
    uint16_t getDroneUdpPort() const { return drone_udp_port_; }
    std::string getRtmpUrl() const { return rtmp_url_; }
    uint32_t getMaxLiveGapMs() const { return max_live_gap_ms_; }
    std::string getLogLevel() const { return log_level_; }
    
    // Print configuration
    void print() const;
    
private:
    uint16_t live_udp_port_ = 10000;
    uint16_t fallback_udp_port_ = 10001;
    uint16_t drone_udp_port_ = 10002;
    std::string rtmp_url_;
    uint32_t max_live_gap_ms_ = 2000;
    std::string log_level_ = "INFO";
};