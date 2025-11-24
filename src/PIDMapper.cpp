#include "PIDMapper.h"
#include <iostream>

PIDMapper::PIDMapper() : initialized_(false) {
}

PIDMapper::~PIDMapper() {
}

void PIDMapper::initialize(const StreamInfo& live_info, const StreamInfo& fallback_info) {
    reset();
    
    // Create mapping from fallback PIDs to live PIDs
    if (fallback_info.video_pid != ts::PID_NULL && live_info.video_pid != ts::PID_NULL) {
        pid_map_[fallback_info.video_pid] = live_info.video_pid;
        std::cout << "[PIDMapper] Video: " << fallback_info.video_pid 
                  << " -> " << live_info.video_pid << std::endl;
    }
    
    if (fallback_info.audio_pid != ts::PID_NULL && live_info.audio_pid != ts::PID_NULL) {
        pid_map_[fallback_info.audio_pid] = live_info.audio_pid;
        std::cout << "[PIDMapper] Audio: " << fallback_info.audio_pid 
                  << " -> " << live_info.audio_pid << std::endl;
    }
    
    // Map PAT and PMT to standard values
    pid_map_[ts::PID_PAT] = ts::PID_PAT;
    if (fallback_info.pmt_pid != ts::PID_NULL && live_info.pmt_pid != ts::PID_NULL) {
        pid_map_[fallback_info.pmt_pid] = live_info.pmt_pid;
        std::cout << "[PIDMapper] PMT: " << fallback_info.pmt_pid 
                  << " -> " << live_info.pmt_pid << std::endl;
    }
    
    initialized_ = !pid_map_.empty();
}

void PIDMapper::remapPacket(ts::TSPacket& packet) {
    if (!initialized_) {
        return;
    }
    
    uint16_t original_pid = packet.getPID();
    
    // Check if this PID needs remapping
    auto it = pid_map_.find(original_pid);
    if (it != pid_map_.end()) {
        uint16_t new_pid = it->second;
        packet.setPID(new_pid);
    }
}

void PIDMapper::fixContinuityCounter(ts::TSPacket& packet) {
    // Only fix CC for packets with payload
    if (!packet.hasPayload()) {
        return;
    }
    
    uint16_t pid = packet.getPID();
    
    // Get next CC for this PID
    uint8_t next_cc = getNextCC(pid);
    
    // Set the CC in the packet
    packet.setCC(next_cc);
}

uint8_t PIDMapper::getNextCC(uint16_t pid) {
    // Continuity counter is 4 bits (0-15)
    auto it = continuity_counters_.find(pid);
    
    if (it == continuity_counters_.end()) {
        // First packet for this PID, start at 0
        continuity_counters_[pid] = 0;
        return 0;
    } else {
        // Increment and wrap at 16
        uint8_t next = (it->second + 1) & 0x0F;
        continuity_counters_[pid] = next;
        return next;
    }
}

void PIDMapper::reset() {
    pid_map_.clear();
    continuity_counters_.clear();
    initialized_ = false;
}