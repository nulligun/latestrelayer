#pragma once

#include <tsduck.h>
#include "TSAnalyzer.h"
#include <map>
#include <cstdint>

class PIDMapper {
public:
    PIDMapper();
    ~PIDMapper();
    
    // Initialize PID mapping based on live and fallback stream info
    void initialize(const StreamInfo& live_info, const StreamInfo& fallback_info);
    
    // Remap a packet's PID from fallback to live
    void remapPacket(ts::TSPacket& packet);
    
    // Fix continuity counter for a packet
    void fixContinuityCounter(ts::TSPacket& packet);
    
    // Reset all mappings and counters
    void reset();
    
    // Check if initialized
    bool isInitialized() const { return initialized_; }
    
private:
    // Get next continuity counter for a PID
    uint8_t getNextCC(uint16_t pid);
    
    // PID mapping: fallback_pid -> live_pid
    std::map<uint16_t, uint16_t> pid_map_;
    
    // Continuity counters per output PID
    std::map<uint16_t, uint8_t> continuity_counters_;
    
    bool initialized_;
};