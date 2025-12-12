#include "TimestampManager.h"
#include <iostream>
#include <cstring>

TimestampManager::TimestampManager()
    : global_pts_offset_(0),
      global_pcr_offset_(0) {
}

TimestampManager::~TimestampManager() {
}

void TimestampManager::initializeWithAlignmentOffset(int64_t alignmentOffset) {
    // Per splice.md and multi2/tcp_main.cpp:
    // The alignment offset is in 27MHz PCR units, convert to 90kHz PTS units
    // PTS should start at this offset (not 0) to preserve decoder buffer timing
    // PCR starts at 0, so the gap between them gives the decoder buffering time
    
    if (alignmentOffset > 0) {
        global_pts_offset_ = (uint64_t)(alignmentOffset / 300);  // 27MHz to 90kHz
        global_pcr_offset_ = 0;  // PCR starts at 0, PTS starts ahead
        
        std::cout << "[TimestampManager] Initialized with alignment offset: "
                  << alignmentOffset << " (27MHz) = " << global_pts_offset_
                  << " (90kHz)" << std::endl;
        std::cout << "[TimestampManager] globalPTSOffset=" << global_pts_offset_
                  << ", globalPCROffset=" << global_pcr_offset_ << std::endl;
    } else {
        std::cout << "[TimestampManager] No alignment offset - starting at 0" << std::endl;
    }
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
    // CRITICAL: Use TSDuck's setPCR() to correctly handle 27MHz format
    // getPCR() returns 27MHz value, we must use setPCR() to write it back correctly
    if (packet.hasPCR()) {
        uint64_t orig_pcr = packet.getPCR();
        // PCR is in 27MHz units, so offset must also be 27MHz
        uint64_t rebased_pcr = (orig_pcr - pcrBase + global_pcr_offset_);
        
        // DEBUG: Log first few PCR rebases
        static int pcr_debug_count = 0;
        if (pcr_debug_count < 5) {
            std::cout << "[TimestampManager-DEBUG] PCR rebase: orig=" << orig_pcr
                      << " (27MHz), base=" << pcrBase
                      << ", offset=" << global_pcr_offset_
                      << ", rebased=" << rebased_pcr
                      << " (" << (rebased_pcr / 27000.0) << "ms)" << std::endl;
            pcr_debug_count++;
        }
        
        // Use TSDuck's setPCR for correct 27MHz handling
        packet.setPCR(rebased_pcr);
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
                
                // DEBUG: Log first several PTS rebases
                static int pts_debug_count = 0;
                if (pts_debug_count < 30) {
                    std::cout << "[TimestampManager-DEBUG] PTS: PID=" << pid
                              << ", orig=" << orig_pts
                              << ", rebased=" << rebased_pts
                              << " (diff from base: " << (orig_pts - ptsBase) << ")" << std::endl;
                }
                
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
                
                // DEBUG: Log DTS values including tracking for backwards detection
                static int dts_debug_count = 0;
                static uint64_t last_dts = 0;
                if (dts_debug_count < 30) {
                    bool backwards = (last_dts > 0 && rebased_dts < last_dts);
                    std::cout << "[TimestampManager-DEBUG] DTS: PID=" << pid
                              << ", orig=" << orig_dts
                              << ", rebased=" << rebased_dts
                              << ", last=" << last_dts
                              << (backwards ? " *** BACKWARDS! ***" : "") << std::endl;
                    dts_debug_count++;
                }
                last_dts = rebased_dts;
                
                // Write rebased DTS
                writePTS(payload + 14, rebased_dts, 0x01);
            }
            
            // DEBUG: Increment counter for both paths
            static int pts_debug_count_2 = 0;
            pts_debug_count_2++;
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