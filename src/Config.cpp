#include "Config.h"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <fstream>

// Helper to get environment variable as uint32_t with default value
uint32_t Config::getEnvUint32(const char* name, uint32_t default_value) {
    const char* env_value = std::getenv(name);
    if (env_value != nullptr && env_value[0] != '\0') {
        try {
            unsigned long val = std::stoul(env_value);
            return static_cast<uint32_t>(val);
        } catch (const std::exception& e) {
            std::cerr << "[Config] Warning: Invalid value for " << name
                      << " ('" << env_value << "'): " << e.what()
                      << " - using default " << default_value << std::endl;
        }
    }
    return default_value;
}

// Helper to get environment variable as double with default value
static double getEnvDouble(const char* name, double default_value) {
    const char* env_value = std::getenv(name);
    if (env_value != nullptr && env_value[0] != '\0') {
        try {
            return std::stod(env_value);
        } catch (const std::exception& e) {
            std::cerr << "[Config] Warning: Invalid value for " << name
                      << " ('" << env_value << "'): " << e.what()
                      << " - using default " << default_value << std::endl;
        }
    }
    return default_value;
}

bool Config::loadFromFile(const std::string& filename) {
    try {
        // Check if file exists
        std::ifstream infile(filename);
        if (!infile.good()) {
            std::cerr << "Configuration file not found: " << filename << std::endl;
            return false;
        }
        
        YAML::Node config = YAML::LoadFile(filename);
        
        // Load required fields
        if (config["live_tcp_port"]) {
            live_tcp_port_ = config["live_tcp_port"].as<uint16_t>();
        } else {
            std::cerr << "Missing required field: live_tcp_port" << std::endl;
            return false;
        }
        
        if (config["fallback_tcp_port"]) {
            fallback_tcp_port_ = config["fallback_tcp_port"].as<uint16_t>();
        } else {
            std::cerr << "Missing required field: fallback_tcp_port" << std::endl;
            return false;
        }
        
        if (config["rtmp_url"]) {
            rtmp_url_ = config["rtmp_url"].as<std::string>();
        } else {
            std::cerr << "Missing required field: rtmp_url" << std::endl;
            return false;
        }
        
        // Load optional fields from YAML (use as defaults for env var overrides)
        if (config["max_live_gap_ms"]) {
            max_live_gap_ms_ = config["max_live_gap_ms"].as<uint32_t>();
        }
        
        if (config["log_level"]) {
            log_level_ = config["log_level"].as<std::string>();
        }
        
        // Drone input settings
        if (config["drone_rtmp_url"]) {
            drone_rtmp_url_ = config["drone_rtmp_url"].as<std::string>();
        }
        
        if (config["input_source_file"]) {
            input_source_file_ = config["input_source_file"].as<std::string>();
        }
        
        // Drone reconnect settings
        if (config["drone_reconnect_initial_ms"]) {
            drone_reconnect_initial_ms_ = config["drone_reconnect_initial_ms"].as<uint32_t>();
        }
        if (config["drone_reconnect_max_ms"]) {
            drone_reconnect_max_ms_ = config["drone_reconnect_max_ms"].as<uint32_t>();
        }
        if (config["drone_reconnect_backoff"]) {
            drone_reconnect_backoff_ = config["drone_reconnect_backoff"].as<double>();
        }
        
        // Load buffer/latency settings from YAML first (as defaults)
        if (config["udp_rcvbuf_size"]) {
            udp_rcvbuf_size_ = config["udp_rcvbuf_size"].as<uint32_t>();
        }
        if (config["ts_queue_size"]) {
            ts_queue_size_ = config["ts_queue_size"].as<uint32_t>();
        }
        if (config["rtmp_pacing_us"]) {
            rtmp_pacing_us_ = config["rtmp_pacing_us"].as<uint32_t>();
        }
        if (config["min_consecutive_for_switch"]) {
            min_consecutive_for_switch_ = config["min_consecutive_for_switch"].as<uint32_t>();
        }
        if (config["live_idr_timeout_ms"]) {
            live_idr_timeout_ms_ = config["live_idr_timeout_ms"].as<uint32_t>();
        }
        if (config["fallback_idr_timeout_ms"]) {
            fallback_idr_timeout_ms_ = config["fallback_idr_timeout_ms"].as<uint32_t>();
        }
        
        // Environment variables override YAML values
        // Priority: env var > YAML > hardcoded default
        udp_rcvbuf_size_ = getEnvUint32("UDP_RCVBUF_SIZE", udp_rcvbuf_size_);
        ts_queue_size_ = getEnvUint32("TS_QUEUE_SIZE", ts_queue_size_);
        rtmp_pacing_us_ = getEnvUint32("RTMP_PACING_US", rtmp_pacing_us_);
        min_consecutive_for_switch_ = getEnvUint32("MIN_CONSECUTIVE_FOR_SWITCH", min_consecutive_for_switch_);
        live_idr_timeout_ms_ = getEnvUint32("LIVE_IDR_TIMEOUT_MS", live_idr_timeout_ms_);
        fallback_idr_timeout_ms_ = getEnvUint32("FALLBACK_IDR_TIMEOUT_MS", fallback_idr_timeout_ms_);
        drone_reconnect_initial_ms_ = getEnvUint32("DRONE_RECONNECT_INITIAL_MS", drone_reconnect_initial_ms_);
        drone_reconnect_max_ms_ = getEnvUint32("DRONE_RECONNECT_MAX_MS", drone_reconnect_max_ms_);
        drone_reconnect_backoff_ = getEnvDouble("DRONE_RECONNECT_BACKOFF", drone_reconnect_backoff_);
        
        return true;
        
    } catch (const YAML::Exception& e) {
        std::cerr << "YAML parsing error: " << e.what() << std::endl;
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Error loading configuration: " << e.what() << std::endl;
        return false;
    }
}

void Config::print() const {
    std::cout << "=== Configuration ===" << std::endl;
    std::cout << "Live TCP Port:     " << live_tcp_port_ << std::endl;
    std::cout << "Fallback TCP Port: " << fallback_tcp_port_ << std::endl;
    std::cout << "RTMP URL:          " << rtmp_url_ << std::endl;
    std::cout << "Max Live Gap (ms): " << max_live_gap_ms_ << std::endl;
    std::cout << "Log Level:         " << log_level_ << std::endl;
    std::cout << "--- Drone Input Settings ---" << std::endl;
    std::cout << "Drone RTMP URL:    " << drone_rtmp_url_ << std::endl;
    std::cout << "Input Source File: " << input_source_file_ << std::endl;
    std::cout << "Drone Reconnect Initial:      " << drone_reconnect_initial_ms_ << " ms" << std::endl;
    std::cout << "Drone Reconnect Max:          " << drone_reconnect_max_ms_ << " ms" << std::endl;
    std::cout << "Drone Reconnect Backoff:      " << drone_reconnect_backoff_ << "x" << std::endl;
    std::cout << "--- Buffer/Latency Settings ---" << std::endl;
    std::cout << "UDP Rcvbuf Size:              " << udp_rcvbuf_size_ << " bytes" << std::endl;
    std::cout << "TS Queue Size:                " << ts_queue_size_ << " packets" << std::endl;
    std::cout << "RTMP Pacing:                  " << rtmp_pacing_us_ << " µs" << std::endl;
    std::cout << "Min Consecutive for Switch:   " << min_consecutive_for_switch_ << " packets" << std::endl;
    std::cout << "Live IDR Timeout:             " << live_idr_timeout_ms_ << " ms" << std::endl;
    std::cout << "Fallback IDR Timeout:         " << fallback_idr_timeout_ms_ << " ms" << std::endl;
    std::cout << "=====================" << std::endl;
}