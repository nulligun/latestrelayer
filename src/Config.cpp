#include "Config.h"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <fstream>

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
        if (config["live_udp_port"]) {
            live_udp_port_ = config["live_udp_port"].as<uint16_t>();
        } else {
            std::cerr << "Missing required field: live_udp_port" << std::endl;
            return false;
        }
        
        if (config["fallback_udp_port"]) {
            fallback_udp_port_ = config["fallback_udp_port"].as<uint16_t>();
        } else {
            std::cerr << "Missing required field: fallback_udp_port" << std::endl;
            return false;
        }
        
        if (config["rtmp_url"]) {
            rtmp_url_ = config["rtmp_url"].as<std::string>();
        } else {
            std::cerr << "Missing required field: rtmp_url" << std::endl;
            return false;
        }
        
        // Load optional fields
        if (config["drone_udp_port"]) {
            drone_udp_port_ = config["drone_udp_port"].as<uint16_t>();
        }
        
        if (config["max_live_gap_ms"]) {
            max_live_gap_ms_ = config["max_live_gap_ms"].as<uint32_t>();
        }
        
        if (config["log_level"]) {
            log_level_ = config["log_level"].as<std::string>();
        }
        
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
    std::cout << "Live UDP Port:     " << live_udp_port_ << std::endl;
    std::cout << "Fallback UDP Port: " << fallback_udp_port_ << std::endl;
    std::cout << "Drone UDP Port:    " << drone_udp_port_ << std::endl;
    std::cout << "RTMP URL:          " << rtmp_url_ << std::endl;
    std::cout << "Max Live Gap (ms): " << max_live_gap_ms_ << std::endl;
    std::cout << "Log Level:         " << log_level_ << std::endl;
    std::cout << "=====================" << std::endl;
}