#include "TimestampManager.h"
#include <iostream>
#include <cstring>

TimestampManager::TimestampManager()
    : global_pts_offset_(0),
      global_pcr_offset_(0) {
}

TimestampManager::~TimestampManager() {
}

void TimestampManager::rebasePacket(ts::TSPacket& packet,
                                   uint64_t ptsBase,
                                   uint64_t pcrBase,
                                   int64_t pcrPtsAlignmentOffset) {
    // Skip PAT/PMT packets - they don't have timestamps
    ts::PID pid = packet.getPID();
    if (pid == ts::PID_PAT) {
        return;
    }
    
    // Rebase PCR if present
    if (packet.hasPCR()) {
        uint64_t orig_pcr = packet.getPCR();
        uint64_t rebased_pcr = (orig_pcr - pcrBase + global_pcr_offset_) & MAX_TIMESTAMP_33BIT;
        
        // Set PCR in adaptation field
        // PCR is stored as 42 bits: 33 bits base + 9 bits extension
        // We only use base and set extension to 0
        uint8_t* data = packet.b;
        size_t af_length = data[4];
        
        if (af_length >= 6) {
            uint8_t* pcr_bytes = data + 6;
            uint64_t pcr_base = rebased_pcr & MAX_TIMESTAMP_33BIT;
            
            pcr_bytes[0] = (pcr_base >> 25) & 0xFF;
            pcr_bytes[1] = (pcr_base >> 17) & 0xFF;
            pcr_bytes[2] = (pcr_base >> 9) & 0xFF;
            pcr_bytes[3] = (pcr_base >> 1) & 0xFF;
            pcr_bytes[4] = ((pcr_base & 0x01) << 7) | 0x7E;
            pcr_bytes[5] = 0x00;
        }
    }
    
    // Rebase PTS/DTS if this is a PES packet start
    if (packet.getPUSI() && packet.hasPayload()) {
        size_t header_size = packet.getHeaderSize();
        uint8_t* payload = packet.b + header_size;
        size_t payload_size = ts::PKT_SIZE - header_size;
        
        // Verify PES start code
        if (payload_size >= 14 && 
            payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
            
            uint8_t pts_dts_flags = (payload[7] >> 6) & 0x03;
            
            // Rebase PTS
            if ((pts_dts_flags == 0x02 || pts_dts_flags == 0x03) && payload_size >= 14) {
                // Extract original PTS
                uint64_t orig_pts = ((uint64_t)(payload[9] & 0x0E) << 29) |
                                   ((uint64_t)(payload[10]) << 22) |
                                   ((uint64_t)(payload[11] & 0xFE) << 14) |
                                   ((uint64_t)(payload[12]) << 7) |
                                   ((uint64_t)(payload[13] >> 1));
                
                // Rebase: new_pts = (orig_pts - ptsBase) + globalPTSOffset
                uint64_t rebased_pts = (orig_pts - ptsBase + global_pts_offset_) & MAX_TIMESTAMP_33BIT;
                
                // Write rebased PTS
                uint8_t marker = (pts_dts_flags == 0x03) ? 0x03 : 0x02;
                writePTS(payload + 9, rebased_pts, marker);
            }
            
            // Rebase DTS if present
            if (pts_dts_flags == 0x03 && payload_size >= 19) {
                // Extract original DTS
                uint64_t orig_dts = ((uint64_t)(payload[14] & 0x0E) << 29) |
                                   ((uint64_t)(payload[15]) << 22) |
                                   ((uint64_t)(payload[16] & 0xFE) << 14) |
                                   ((uint64_t)(payload[17]) << 7) |
                                   ((uint64_t)(payload[18] >> 1));
                
                // Rebase: new_dts = (orig_dts - ptsBase) + globalPTSOffset  
                // Note: Use same base and offset as PTS (DTS is in same timeline)
                uint64_t rebased_dts = (orig_dts - ptsBase + global_pts_offset_) & MAX_TIMESTAMP_33BIT;
                
                // Write rebased DTS
                writePTS(payload + 14, rebased_dts, 0x01);
            }
        }
    }
}

void TimestampManager::updateOffsetsAfterSegment(uint64_t segmentDurationPTS) {
    // Increment global offsets by segment duration
    // This maintains continuous timeline across switches
    global_pts_offset_ = (global_pts_offset_ + segmentDurationPTS) & MAX_TIMESTAMP_33BIT;
    
    // PCR uses 27MHz clock, PTS uses 90kHz clock
    // 1 PTS tick = 300 PCR ticks (27MHz / 90kHz = 300)
    uint64_t segmentDurationPCR = segmentDurationPTS * 300;
    global_pcr_offset_ = (global_pcr_offset_ + segmentDurationPCR) & MAX_TIMESTAMP_33BIT;
    
    std::cout << "[TimestampManager] Updated offsets after segment:" << std::endl;
    std::cout << "  Global PTS offset: " << global_pts_offset_ << std::endl;
    std::cout << "  Global PCR offset: " << global_pcr_offset_ << std::endl;
}

void TimestampManager::writePTS(uint8_t* pes_header, uint64_t pts, uint8_t marker) {
    // PTS/DTS format: 33 bits in 5 bytes
    // marker(4) | pts[32-30](3) | marker(1) | pts[29-22](8) | marker(1) | 
    // pts[21-15](7) | marker(1) | pts[14-7](8) | marker(1) | pts[6-0](7) | marker(1)
    
    pes_header[0] = (marker << 4) | ((pts >> 29) & 0x0E) | 0x01;
    pes_header[1] = (pts >> 22) & 0xFF;
    pes_header[2] = ((pts >> 14) & 0xFE) | 0x01;
    pes_header[3] = (pts >> 7) & 0xFF;
    pes_header[4] = ((pts << 1) & 0xFE) | 0x01;
}

void TimestampManager::reset() {
    global_pts_offset_ = 0;
    global_pcr_offset_ = 0;
    std::cout << "[TimestampManager] Reset - offsets cleared" << std::endl;
}