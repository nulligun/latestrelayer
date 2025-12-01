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
      last_fallback_dts_(0),
      last_live_packet_wall_time_(std::chrono::steady_clock::now()),
      has_live_reference_(false) {
}

TimestampManager::~TimestampManager() {
}

void TimestampManager::trackLiveTimestamps(const TimestampInfo& ts_info, uint64_t adjusted_pts, uint64_t adjusted_dts, uint64_t adjusted_pcr) {
    // Track timestamps from live packets
    // When live_offset_ is non-zero (FALLBACK→LIVE transition), we store the ADJUSTED values
    // to maintain continuous output timeline tracking
    
    auto now = std::chrono::steady_clock::now();
    
    if (ts_info.pts.has_value()) {
        last_live_pts_ = ts_info.pts.value();  // Store original for reference
        last_live_packet_wall_time_ = now;
        has_live_reference_ = true;
        
        // Store the adjusted PTS as output (maintains timeline continuity)
        last_output_pts_ = adjusted_pts;
    }
    
    if (ts_info.dts.has_value()) {
        last_output_dts_ = adjusted_dts;
    }
    
    if (ts_info.pcr.has_value()) {
        last_output_pcr_ = adjusted_pcr;
    }
}

bool TimestampManager::adjustPacket(ts::TSPacket& packet, Source source, const TimestampInfo& input_ts) {
    // LIVE packets: Apply live_offset_ if non-zero (for FALLBACK→LIVE continuity)
    // When live_offset_ is 0, live packets pass through unmodified
    if (source == Source::LIVE) {
        if (live_offset_ == 0) {
            // Pure passthrough mode - no offset needed
            // Track with original timestamps
            uint64_t pts = input_ts.pts.value_or(last_output_pts_);
            uint64_t dts = input_ts.dts.value_or(last_output_dts_);
            uint64_t pcr = input_ts.pcr.value_or(last_output_pcr_);
            trackLiveTimestamps(input_ts, pts, dts, pcr);
            return true;
        }
        
        // FALLBACK→LIVE transition: Apply offset to maintain timeline continuity
        // This ensures smooth playback when returning from fallback to live
        
        std::optional<uint64_t> adjusted_pts;
        std::optional<uint64_t> adjusted_dts;
        
        if (input_ts.pts.has_value()) {
            uint64_t raw_pts = input_ts.pts.value();
            adjusted_pts = (raw_pts + live_offset_) & MAX_TIMESTAMP;
        }
        
        if (input_ts.dts.has_value()) {
            uint64_t raw_dts = input_ts.dts.value();
            adjusted_dts = (raw_dts + live_offset_) & MAX_TIMESTAMP;
        }
        
        // Apply PES timestamp adjustments to the packet
        if (adjusted_pts.has_value() || adjusted_dts.has_value()) {
            adjustPESTimestamps(packet, adjusted_pts, adjusted_dts);
        }
        
        // Adjust PCR if present
        std::optional<uint64_t> adjusted_pcr;
        if (input_ts.pcr.has_value()) {
            uint64_t raw_pcr = input_ts.pcr.value();
            adjusted_pcr = (raw_pcr + live_offset_) & MAX_TIMESTAMP;
            adjustPCR(packet, adjusted_pcr.value());
        }
        
        // Track with adjusted timestamps
        uint64_t final_pts = adjusted_pts.value_or(last_output_pts_);
        uint64_t final_dts = adjusted_dts.value_or(last_output_dts_);
        uint64_t final_pcr = adjusted_pcr.value_or(last_output_pcr_);
        trackLiveTimestamps(input_ts, final_pts, final_dts, final_pcr);
        
        return true;
    }
    
    // FALLBACK processing below
    
    // Skip packets without timestamps
    if (!input_ts.pts.has_value() && !input_ts.dts.has_value()) {
        return true;
    }
    
    // Check for loop boundary in fallback stream
    if (input_ts.pts.has_value()) {
        if (detectLoopBoundary(source, input_ts.pts.value())) {
            std::cout << "[TimestampManager] Loop boundary detected in fallback stream" << std::endl;
            std::cout << "[TimestampManager] Recalculating offset to maintain continuity" << std::endl;
            onSourceSwitch(source, input_ts);
        }
    }
    
    // Use fallback offset (live_offset_ is always 0 since live is passthrough)
    int64_t offset = fallback_offset_;
    
    std::optional<uint64_t> adjusted_pts;
    std::optional<uint64_t> adjusted_dts;
    
    // Adjust PTS if present - NO monotonic enforcement to preserve B-frame timing
    if (input_ts.pts.has_value()) {
        uint64_t raw_pts = input_ts.pts.value();
        uint64_t new_pts = (raw_pts + offset) & MAX_TIMESTAMP;
        
        adjusted_pts = new_pts;
        last_fallback_pts_ = raw_pts;
    }
    
    // Adjust DTS if present - NO monotonic enforcement to preserve B-frame timing
    if (input_ts.dts.has_value()) {
        uint64_t raw_dts = input_ts.dts.value();
        uint64_t new_dts = (raw_dts + offset) & MAX_TIMESTAMP;
        
        adjusted_dts = new_dts;
        last_fallback_dts_ = raw_dts;
    }
    
    // Update last output timestamps
    // Note: No DTS > PTS constraint enforcement - trust the source stream
    if (adjusted_pts.has_value()) {
        last_output_pts_ = adjusted_pts.value();
    }
    if (adjusted_dts.has_value()) {
        last_output_dts_ = adjusted_dts.value();
    }
    
    // Apply PES timestamp adjustments
    if (adjusted_pts.has_value() || adjusted_dts.has_value()) {
        adjustPESTimestamps(packet, adjusted_pts, adjusted_dts);
    }
    
    // Adjust PCR if present - NO monotonic enforcement
    if (input_ts.pcr.has_value()) {
        uint64_t raw_pcr = input_ts.pcr.value();
        uint64_t new_pcr = (raw_pcr + offset) & MAX_TIMESTAMP;
        
        adjustPCR(packet, new_pcr);
        last_output_pcr_ = new_pcr;
    }
    
    return true;
}

void TimestampManager::onSourceSwitch(Source new_source, const TimestampInfo& first_packet_ts) {
    if (new_source == Source::LIVE) {
        // FALLBACK→LIVE: Calculate offset to maintain timeline continuity
        // The output timeline should continue smoothly from where fallback left off
        
        if (!first_packet_ts.pts.has_value()) {
            std::cout << "[TimestampManager] WARNING: Cannot calculate live offset without PTS - using passthrough" << std::endl;
            live_offset_ = 0;
            return;
        }
        
        uint64_t first_live_pts = first_packet_ts.pts.value();
        
        // Target PTS = last output PTS + one frame duration (for smooth continuity)
        uint64_t target_pts = (last_output_pts_ + DEFAULT_FRAME_DURATION) & MAX_TIMESTAMP;
        
        // Calculate offset: live_pts + offset = target_pts
        live_offset_ = static_cast<int64_t>(target_pts) - static_cast<int64_t>(first_live_pts);
        
        std::cout << "[TimestampManager] Switched to LIVE with timeline continuity offset" << std::endl;
        std::cout << "  Last output PTS: " << last_output_pts_ << std::endl;
        std::cout << "  Target PTS: " << target_pts << std::endl;
        std::cout << "  First live PTS: " << first_live_pts << std::endl;
        std::cout << "  Calculated live_offset: " << live_offset_ << std::endl;
        return;
    }
    
    // Switching to FALLBACK - calculate gap-aware offset
    if (!first_packet_ts.pts.has_value()) {
        std::cout << "[TimestampManager] WARNING: Cannot calculate fallback offset without PTS" << std::endl;
        return;
    }
    
    uint64_t first_fallback_pts = first_packet_ts.pts.value();
    uint64_t target_pts;
    
    if (has_live_reference_) {
        // Calculate elapsed time since last live packet (gap time)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_live_packet_wall_time_
        );
        
        // Convert to 90kHz timestamp units (90 ticks per millisecond)
        uint64_t elapsed_90kHz = static_cast<uint64_t>(elapsed.count()) * 90;
        
        // Target PTS = last live PTS + elapsed gap time
        // This makes the timeline appear continuous as if the live stream never stopped
        target_pts = (last_live_pts_ + elapsed_90kHz) & MAX_TIMESTAMP;
        
        std::cout << "[TimestampManager] Switched to FALLBACK with gap-aware offset" << std::endl;
        std::cout << "  Last live PTS: " << last_live_pts_ << std::endl;
        std::cout << "  Elapsed gap: " << elapsed.count() << "ms (" << elapsed_90kHz << " @ 90kHz)" << std::endl;
        std::cout << "  Target PTS: " << target_pts << std::endl;
    } else {
        // No live reference yet - use default frame duration from last output
        target_pts = (last_output_pts_ + DEFAULT_FRAME_DURATION) & MAX_TIMESTAMP;
        std::cout << "[TimestampManager] Switched to FALLBACK (no live reference, using default)" << std::endl;
    }
    
    // Calculate offset: fallback_pts + offset = target_pts
    fallback_offset_ = static_cast<int64_t>(target_pts) - static_cast<int64_t>(first_fallback_pts);
    
    std::cout << "  First fallback PTS: " << first_fallback_pts << std::endl;
    std::cout << "  Calculated offset: " << fallback_offset_ << std::endl;
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
    last_live_packet_wall_time_ = std::chrono::steady_clock::now();
    has_live_reference_ = false;
}