#include "TimestampManager.h"
#include <iostream>
#include <cstring>
#include <algorithm>

TimestampManager::TimestampManager()
    : live_offset_(0),
      fallback_offset_(0),
      last_output_pts_(0),
      last_output_dts_(0),
      last_output_pcr_(0),
      last_live_pts_(0),
      last_fallback_pts_(0),
      last_fallback_dts_(0) {
}

TimestampManager::~TimestampManager() {
}

bool TimestampManager::adjustPacket(ts::TSPacket& packet, Source source, const TimestampInfo& input_ts) {
    // Skip packets without timestamps
    if (!input_ts.pts.has_value() && !input_ts.dts.has_value()) {
        return true;
    }
    
    // Check for loop boundary in fallback stream
    if (source == Source::FALLBACK && input_ts.pts.has_value()) {
        if (detectLoopBoundary(source, input_ts.pts.value())) {
            std::cout << "[TimestampManager] Loop boundary detected in fallback stream" << std::endl;
            std::cout << "[TimestampManager] Recalculating offset to maintain continuity" << std::endl;
            onSourceSwitch(source, input_ts);
        }
    }
    
    // Determine which offset to use
    int64_t offset = (source == Source::LIVE) ? live_offset_ : fallback_offset_;
    
    std::optional<uint64_t> adjusted_pts;
    std::optional<uint64_t> adjusted_dts;
    
    // Adjust PTS if present
    if (input_ts.pts.has_value()) {
        uint64_t raw_pts = input_ts.pts.value();
        uint64_t new_pts = (raw_pts + offset) & MAX_TIMESTAMP;
        
        // Enforce monotonic increase
        uint64_t original_new_pts = new_pts;
        new_pts = enforceMonotonic(new_pts, last_output_pts_);
        
        if (new_pts != original_new_pts && false) {  // Disabled verbose logging
            std::cout << "[TimestampManager] PTS enforced monotonic: " << original_new_pts
                      << " -> " << new_pts << " (last=" << last_output_pts_ << ")" << std::endl;
        }
        
        adjusted_pts = new_pts;
        
        // Update source-specific tracking
        if (source == Source::LIVE) {
            last_live_pts_ = raw_pts;
        } else {
            last_fallback_pts_ = raw_pts;
        }
    }
    
    // Adjust DTS if present
    if (input_ts.dts.has_value()) {
        uint64_t raw_dts = input_ts.dts.value();
        uint64_t new_dts = (raw_dts + offset) & MAX_TIMESTAMP;
        
        // First apply monotonic enforcement
        uint64_t original_new_dts = new_dts;
        new_dts = enforceMonotonic(new_dts, last_output_dts_);
        
        static int dts_correction_count = 0;
        if (new_dts != original_new_dts) {
            dts_correction_count++;
            if (dts_correction_count % 100 == 1) {  // Log every 100th correction
                std::cout << "[TimestampManager] DTS enforced monotonic: " << original_new_dts
                          << " -> " << new_dts << " (last=" << last_output_dts_
                          << ", count=" << dts_correction_count << ")" << std::endl;
            }
        }
        
        adjusted_dts = new_dts;
        
        // Track fallback DTS
        if (source == Source::FALLBACK) {
            last_fallback_dts_ = raw_dts;
        }
    }
    
    // Enforce DTS <= PTS constraint AFTER monotonic enforcement
    // This must also maintain monotonic DTS
    if (adjusted_pts.has_value() && adjusted_dts.has_value()) {
        uint64_t pts = adjusted_pts.value();
        uint64_t dts = adjusted_dts.value();
        
        if (dts > pts) {
            // DTS cannot exceed PTS
            // But we also cannot let DTS go backwards
            // Set DTS to minimum of PTS and last_output_dts_+1
            uint64_t safe_dts = pts;
            
            // Ensure we don't go backwards
            if (safe_dts <= last_output_dts_) {
                safe_dts = last_output_dts_ + 1;
                // If this makes DTS > PTS again, we have a problem
                // In this case, also bump PTS
                if (safe_dts > pts) {
                    std::cout << "[TimestampManager] WARNING: Complex timestamp issue" << std::endl;
                    std::cout << "  Bumping both PTS and DTS to maintain ordering" << std::endl;
                    adjusted_pts = safe_dts;
                    last_output_pts_ = safe_dts;
                }
            }
            
            std::cout << "[TimestampManager] WARNING: DTS > PTS violation corrected" << std::endl;
            std::cout << "  Original: DTS=" << dts << " PTS=" << pts << std::endl;
            std::cout << "  Corrected: DTS=" << safe_dts << " PTS=" << adjusted_pts.value() << std::endl;
            
            adjusted_dts = safe_dts;
        }
    }
    
    // Update last output timestamps AFTER all adjustments
    if (adjusted_pts.has_value()) {
        last_output_pts_ = adjusted_pts.value();
    }
    if (adjusted_dts.has_value()) {
        last_output_dts_ = adjusted_dts.value();
    }
    
    // Apply PES timestamp adjustments
    if (adjusted_pts.has_value() || adjusted_dts.has_value()) {
        static int write_count = 0;
        write_count++;
        
        if (adjusted_dts.has_value() && write_count % 100 == 1) {
            std::cout << "[TimestampManager] Writing timestamps to packet #" << write_count << std::endl;
            if (input_ts.dts.has_value()) {
                std::cout << "  Input DTS: " << input_ts.dts.value()
                          << " -> Output DTS: " << adjusted_dts.value()
                          << " (last=" << last_output_dts_ << ")" << std::endl;
            }
        }
        
        adjustPESTimestamps(packet, adjusted_pts, adjusted_dts);
    }
    
    // Adjust PCR if present
    if (input_ts.pcr.has_value()) {
        uint64_t raw_pcr = input_ts.pcr.value();
        uint64_t new_pcr = (raw_pcr + offset) & MAX_TIMESTAMP;
        
        // Enforce monotonic increase
        new_pcr = enforceMonotonic(new_pcr, last_output_pcr_);
        adjustPCR(packet, new_pcr);
        last_output_pcr_ = new_pcr;
    }
    
    return true;
}

void TimestampManager::onSourceSwitch(Source new_source, const TimestampInfo& first_packet_ts) {
    if (!first_packet_ts.pts.has_value()) {
        return; // Can't calculate offset without PTS
    }
    
    uint64_t first_pts = first_packet_ts.pts.value();
    
    // Calculate offset so that output continues from last_output_pts
    // new_output_pts = first_pts + offset
    // We want: new_output_pts = last_output_pts + frame_duration
    
    uint64_t target_pts = (last_output_pts_ + DEFAULT_FRAME_DURATION) & MAX_TIMESTAMP;
    int64_t new_offset = static_cast<int64_t>(target_pts) - static_cast<int64_t>(first_pts);
    
    if (new_source == Source::LIVE) {
        live_offset_ = new_offset;
        std::cout << "[TimestampManager] Switched to LIVE, offset=" << live_offset_ << std::endl;
    } else {
        fallback_offset_ = new_offset;
        std::cout << "[TimestampManager] Switched to FALLBACK, offset=" << fallback_offset_ << std::endl;
    }
}

void TimestampManager::adjustPESTimestamps(ts::TSPacket& packet, 
                                          std::optional<uint64_t> new_pts,
                                          std::optional<uint64_t> new_dts) {
    if (!packet.getPUSI()) {
        return; // Not a PES start
    }
    
    uint8_t* payload = packet.getPayload();
    size_t payload_size = packet.getPayloadSize();
    
    if (payload_size < 14) {
        return; // PES header too small
    }
    
    // Verify PES start code
    if (payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01) {
        return;
    }
    
    uint8_t pts_dts_flags = (payload[7] >> 6) & 0x03;
    
    // Write PTS
    if (new_pts.has_value() && pts_dts_flags >= 2 && payload_size >= 14) {
        writePTS(payload + 9, new_pts.value(), (pts_dts_flags == 3) ? 0x03 : 0x02);
    }
    
    // Write DTS
    if (new_dts.has_value() && pts_dts_flags == 3 && payload_size >= 19) {
        writePTS(payload + 14, new_dts.value(), 0x01);
    }
}

void TimestampManager::adjustPCR(ts::TSPacket& packet, uint64_t new_pcr) {
    if (!packet.hasAF() || !packet.hasPCR()) {
        return;
    }
    
    // Get adaptation field
    uint8_t* data = packet.b;
    size_t af_length = data[4];
    
    if (af_length < 6) {
        return;
    }
    
    // PCR is at bytes 6-11 (after header + AF length + AF flags)
    uint8_t* pcr_bytes = data + 6;
    
    // PCR is 42 bits: 33 bits base + 9 bits extension
    // We'll set extension to 0 for simplicity
    uint64_t pcr_base = new_pcr & MAX_TIMESTAMP;
    
    pcr_bytes[0] = (pcr_base >> 25) & 0xFF;
    pcr_bytes[1] = (pcr_base >> 17) & 0xFF;
    pcr_bytes[2] = (pcr_base >> 9) & 0xFF;
    pcr_bytes[3] = (pcr_base >> 1) & 0xFF;
    pcr_bytes[4] = ((pcr_base & 0x01) << 7) | 0x7E;
    pcr_bytes[5] = 0x00;
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

uint64_t TimestampManager::enforceMonotonic(uint64_t ts, uint64_t last_ts) {
    // Handle wraparound case
    if (ts < last_ts && (last_ts - ts) > (MAX_TIMESTAMP / 2)) {
        // This looks like a wraparound, allow it
        return ts;
    }
    
    // If timestamp would go backwards (and it's not wraparound), advance it
    if (ts <= last_ts) {
        ts = last_ts + 1;
    }
    
    return ts & MAX_TIMESTAMP;
}

uint64_t TimestampManager::handleWraparound(uint64_t ts, uint64_t reference) {
    // Handle 33-bit wraparound
    int64_t diff = static_cast<int64_t>(ts) - static_cast<int64_t>(reference);
    
    if (diff < -(MAX_TIMESTAMP / 2)) {
        // Wraparound forward
        return ts + MAX_TIMESTAMP + 1;
    } else if (diff > (MAX_TIMESTAMP / 2)) {
        // Wraparound backward
        return ts - MAX_TIMESTAMP - 1;
    }
    
    return ts;
}

bool TimestampManager::detectLoopBoundary(Source source, uint64_t current_pts) {
    if (source != Source::FALLBACK) {
        return false;
    }
    
    // First packet from fallback - not a loop
    if (last_fallback_pts_ == 0) {
        return false;
    }
    
    // Check if timestamp jumped backward significantly
    // This indicates the fallback video has looped
    if (current_pts < last_fallback_pts_) {
        uint64_t backward_jump = last_fallback_pts_ - current_pts;
        
        // If backward jump is more than 50% of timestamp range, it's a loop
        if (backward_jump > LOOP_DETECTION_THRESHOLD) {
            std::cout << "[TimestampManager] Large backward timestamp jump detected:" << std::endl;
            std::cout << "  Previous PTS: " << last_fallback_pts_ << std::endl;
            std::cout << "  Current PTS: " << current_pts << std::endl;
            std::cout << "  Backward jump: " << backward_jump << " (threshold: "
                      << LOOP_DETECTION_THRESHOLD << ")" << std::endl;
            return true;
        }
    }
    
    return false;
}

void TimestampManager::reset() {
    live_offset_ = 0;
    fallback_offset_ = 0;
    last_output_pts_ = 0;
    last_output_dts_ = 0;
    last_output_pcr_ = 0;
    last_live_pts_ = 0;
    last_fallback_pts_ = 0;
    last_fallback_dts_ = 0;
}