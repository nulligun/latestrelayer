#include "OutputTimestampMonitor.h"
#include <iostream>
#include <iomanip>

OutputTimestampMonitor::OutputTimestampMonitor() 
    : last_summary_time_(std::chrono::steady_clock::now()),
      start_time_(std::chrono::steady_clock::now()) {
    std::cout << "[OutputTimestampMonitor] Initialized - will monitor output timestamps" << std::endl;
}

OutputTimestampMonitor::~OutputTimestampMonitor() {
}

void OutputTimestampMonitor::setVideoPID(uint16_t pid) {
    video_pid_ = pid;
    std::cout << "[OutputTimestampMonitor] Tracking video PID: " << pid << std::endl;
}

void OutputTimestampMonitor::setAudioPID(uint16_t pid) {
    audio_pid_ = pid;
    std::cout << "[OutputTimestampMonitor] Tracking audio PID: " << pid << std::endl;
}

bool OutputTimestampMonitor::checkPacket(const ts::TSPacket& packet) {
    ts::PID pid = packet.getPID();
    bool all_ok = true;
    
    // Check PCR if present (can be on any PID)
    if (packet.hasPCR()) {
        uint64_t pcr = packet.getPCR();
        pcr_packet_count_++;
        stats_.total_pcr_packets++;
        
        if (!checkPCR(pcr, pcr_packet_count_)) {
            all_ok = false;
        }
    }
    
    // Check video PID timestamps
    if (pid == video_pid_ && video_pid_ != 0) {
        video_state_.packet_count++;
        stats_.total_video_packets++;
        
        // Extract PTS/DTS if this is a PES packet start
        if (packet.getPUSI() && packet.hasPayload()) {
            auto pts = extractPTS(packet);
            auto dts = extractDTS(packet);
            
            // Check DTS (required for video, strictly monotonic)
            if (dts.has_value()) {
                if (!checkVideoDTS(dts.value(), video_state_.packet_count)) {
                    all_ok = false;
                }
            }
            
            // Check PTS (allow B-frame reordering)
            if (pts.has_value()) {
                if (!checkVideoPTS(pts.value(), video_state_.packet_count)) {
                    all_ok = false;
                }
            }
        }
    }
    
    // Check audio PID timestamps
    if (pid == audio_pid_ && audio_pid_ != 0) {
        audio_state_.packet_count++;
        stats_.total_audio_packets++;
        
        // Extract PTS/DTS if this is a PES packet start
        if (packet.getPUSI() && packet.hasPayload()) {
            auto pts = extractPTS(packet);
            auto dts = extractDTS(packet);
            
            // Check DTS (strictly monotonic)
            if (dts.has_value()) {
                if (!checkAudioDTS(dts.value(), audio_state_.packet_count)) {
                    all_ok = false;
                }
            }
            
            // Check PTS (strictly monotonic for audio)
            if (pts.has_value()) {
                if (!checkAudioPTS(pts.value(), audio_state_.packet_count)) {
                    all_ok = false;
                }
            }
        }
    }
    
    return all_ok;
}

bool OutputTimestampMonitor::checkVideoDTS(uint64_t current_dts, uint64_t packet_num) {
    if (!video_state_.last_dts.has_value()) {
        // First DTS - just store it
        video_state_.last_dts = current_dts;
        return true;
    }
    
    uint64_t prev_dts = video_state_.last_dts.value();
    
    // DTS must be strictly monotonically increasing
    if (!isTimestampIncreasing(prev_dts, current_dts)) {
        int64_t delta = static_cast<int64_t>(current_dts) - static_cast<int64_t>(prev_dts);
        
        std::cerr << "[OutputTimestampMonitor] =======================================" << std::endl;
        std::cerr << "[OutputTimestampMonitor] DISCONTINUITY ALERT - Video DTS not increasing!" << std::endl;
        std::cerr << "[OutputTimestampMonitor] =======================================" << std::endl;
        std::cerr << "[OutputTimestampMonitor]   PID: " << video_pid_ << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Previous DTS: " << prev_dts << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Current DTS:  " << current_dts << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Expected min: " << (prev_dts + 1) << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Delta:        " << delta << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Packet #:     " << packet_num << std::endl;
        std::cerr << "[OutputTimestampMonitor] =======================================" << std::endl;
        
        video_state_.dts_discontinuity_count++;
        stats_.video_dts_discontinuities++;
        video_state_.last_dts = current_dts;
        return false;
    }
    
    video_state_.last_dts = current_dts;
    return true;
}

bool OutputTimestampMonitor::checkVideoPTS(uint64_t current_pts, uint64_t packet_num) {
    if (!video_state_.last_pts.has_value()) {
        // First PTS - just store it
        video_state_.last_pts = current_pts;
        return true;
    }
    
    uint64_t prev_pts = video_state_.last_pts.value();
    
    // For video with B-frames, PTS can reorder slightly
    // But it should not go far backward (more than ~6 frames @ 30fps = ~180ms = 16200 ticks @ 90kHz)
    static constexpr int64_t MAX_BACKWARD_PTS = 16200;  // Allow ~6 frames of reordering
    
    int64_t delta = static_cast<int64_t>(current_pts) - static_cast<int64_t>(prev_pts);
    
    // Alert if PTS goes backward by more than expected B-frame reordering
    if (delta < -MAX_BACKWARD_PTS) {
        std::cerr << "[OutputTimestampMonitor] =======================================" << std::endl;
        std::cerr << "[OutputTimestampMonitor] DISCONTINUITY ALERT - Video PTS large backward jump!" << std::endl;
        std::cerr << "[OutputTimestampMonitor] =======================================" << std::endl;
        std::cerr << "[OutputTimestampMonitor]   PID: " << video_pid_ << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Previous PTS: " << prev_pts << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Current PTS:  " << current_pts << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Delta:        " << delta << " (max allowed: " << -MAX_BACKWARD_PTS << ")" << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Packet #:     " << packet_num << std::endl;
        std::cerr << "[OutputTimestampMonitor] =======================================" << std::endl;
        
        video_state_.pts_discontinuity_count++;
        stats_.video_pts_discontinuities++;
        video_state_.last_pts = current_pts;
        return false;
    }
    
    video_state_.last_pts = current_pts;
    return true;
}

bool OutputTimestampMonitor::checkAudioDTS(uint64_t current_dts, uint64_t packet_num) {
    if (!audio_state_.last_dts.has_value()) {
        // First DTS - just store it
        audio_state_.last_dts = current_dts;
        return true;
    }
    
    uint64_t prev_dts = audio_state_.last_dts.value();
    
    // DTS must be strictly monotonically increasing
    if (!isTimestampIncreasing(prev_dts, current_dts)) {
        int64_t delta = static_cast<int64_t>(current_dts) - static_cast<int64_t>(prev_dts);
        
        std::cerr << "[OutputTimestampMonitor] =======================================" << std::endl;
        std::cerr << "[OutputTimestampMonitor] DISCONTINUITY ALERT - Audio DTS not increasing!" << std::endl;
        std::cerr << "[OutputTimestampMonitor] =======================================" << std::endl;
        std::cerr << "[OutputTimestampMonitor]   PID: " << audio_pid_ << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Previous DTS: " << prev_dts << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Current DTS:  " << current_dts << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Expected min: " << (prev_dts + 1) << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Delta:        " << delta << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Packet #:     " << packet_num << std::endl;
        std::cerr << "[OutputTimestampMonitor] =======================================" << std::endl;
        
        audio_state_.dts_discontinuity_count++;
        stats_.audio_dts_discontinuities++;
        audio_state_.last_dts = current_dts;
        return false;
    }
    
    audio_state_.last_dts = current_dts;
    return true;
}

bool OutputTimestampMonitor::checkAudioPTS(uint64_t current_pts, uint64_t packet_num) {
    if (!audio_state_.last_pts.has_value()) {
        // First PTS - just store it
        audio_state_.last_pts = current_pts;
        return true;
    }
    
    uint64_t prev_pts = audio_state_.last_pts.value();
    
    // Audio PTS should be strictly monotonically increasing (no B-frames)
    if (!isTimestampIncreasing(prev_pts, current_pts)) {
        int64_t delta = static_cast<int64_t>(current_pts) - static_cast<int64_t>(prev_pts);
        
        std::cerr << "[OutputTimestampMonitor] =======================================" << std::endl;
        std::cerr << "[OutputTimestampMonitor] DISCONTINUITY ALERT - Audio PTS not increasing!" << std::endl;
        std::cerr << "[OutputTimestampMonitor] =======================================" << std::endl;
        std::cerr << "[OutputTimestampMonitor]   PID: " << audio_pid_ << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Previous PTS: " << prev_pts << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Current PTS:  " << current_pts << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Expected min: " << (prev_pts + 1) << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Delta:        " << delta << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Packet #:     " << packet_num << std::endl;
        std::cerr << "[OutputTimestampMonitor] =======================================" << std::endl;
        
        audio_state_.pts_discontinuity_count++;
        stats_.audio_pts_discontinuities++;
        audio_state_.last_pts = current_pts;
        return false;
    }
    
    audio_state_.last_pts = current_pts;
    return true;
}

bool OutputTimestampMonitor::checkPCR(uint64_t current_pcr, uint64_t packet_num) {
    if (!last_pcr_.has_value()) {
        // First PCR - just store it
        last_pcr_ = current_pcr;
        return true;
    }
    
    uint64_t prev_pcr = last_pcr_.value();
    
    // PCR must be strictly monotonically increasing
    if (!isTimestampIncreasing(prev_pcr, current_pcr)) {
        int64_t delta = static_cast<int64_t>(current_pcr) - static_cast<int64_t>(prev_pcr);
        
        std::cerr << "[OutputTimestampMonitor] =======================================" << std::endl;
        std::cerr << "[OutputTimestampMonitor] DISCONTINUITY ALERT - PCR not increasing!" << std::endl;
        std::cerr << "[OutputTimestampMonitor] =======================================" << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Previous PCR: " << prev_pcr << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Current PCR:  " << current_pcr << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Expected min: " << (prev_pcr + 1) << std::endl;
        std::cerr << "[OutputTimestampMonitor]   Delta:        " << delta << std::endl;
        std::cerr << "[OutputTimestampMonitor]   PCR Packet #: " << packet_num << std::endl;
        std::cerr << "[OutputTimestampMonitor] =======================================" << std::endl;
        
        pcr_discontinuity_count_++;
        stats_.pcr_discontinuities++;
        last_pcr_ = current_pcr;
        return false;
    }
    
    last_pcr_ = current_pcr;
    return true;
}

std::optional<uint64_t> OutputTimestampMonitor::extractPTS(const ts::TSPacket& packet) {
    if (!packet.getPUSI() || !packet.hasPayload()) {
        return std::nullopt;
    }
    
    size_t header_size = packet.getHeaderSize();
    const uint8_t* payload = packet.b + header_size;
    size_t payload_size = ts::PKT_SIZE - header_size;
    
    // Verify PES start code
    if (payload_size < 14 || 
        payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01) {
        return std::nullopt;
    }
    
    uint8_t pts_dts_flags = (payload[7] >> 6) & 0x03;
    
    // Extract PTS if present
    if ((pts_dts_flags == 0x02 || pts_dts_flags == 0x03) && payload_size >= 14) {
        uint64_t pts = ((uint64_t)(payload[9] & 0x0E) << 29) |
                       ((uint64_t)(payload[10]) << 22) |
                       ((uint64_t)(payload[11] & 0xFE) << 14) |
                       ((uint64_t)(payload[12]) << 7) |
                       ((uint64_t)(payload[13] >> 1));
        return pts;
    }
    
    return std::nullopt;
}

std::optional<uint64_t> OutputTimestampMonitor::extractDTS(const ts::TSPacket& packet) {
    if (!packet.getPUSI() || !packet.hasPayload()) {
        return std::nullopt;
    }
    
    size_t header_size = packet.getHeaderSize();
    const uint8_t* payload = packet.b + header_size;
    size_t payload_size = ts::PKT_SIZE - header_size;
    
    // Verify PES start code
    if (payload_size < 19 || 
        payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01) {
        return std::nullopt;
    }
    
    uint8_t pts_dts_flags = (payload[7] >> 6) & 0x03;
    
    // Extract DTS if present (only when pts_dts_flags == 0x03)
    if (pts_dts_flags == 0x03 && payload_size >= 19) {
        uint64_t dts = ((uint64_t)(payload[14] & 0x0E) << 29) |
                       ((uint64_t)(payload[15]) << 22) |
                       ((uint64_t)(payload[16] & 0xFE) << 14) |
                       ((uint64_t)(payload[17]) << 7) |
                       ((uint64_t)(payload[18] >> 1));
        return dts;
    }
    
    // If only PTS present, use PTS as DTS
    if (pts_dts_flags == 0x02) {
        return extractPTS(packet);
    }
    
    return std::nullopt;
}

bool OutputTimestampMonitor::isTimestampIncreasing(uint64_t prev, uint64_t current) {
    // Handle 33-bit wraparound
    // If current < prev, check if it's a wraparound or a backward jump
    if (current <= prev) {
        // Check for wraparound: if prev is near max and current is near 0
        uint64_t distance_to_max = MAX_TIMESTAMP_33BIT - prev;
        if (distance_to_max < (MAX_TIMESTAMP_33BIT / 4) && current < (MAX_TIMESTAMP_33BIT / 4)) {
            // Likely a wraparound - consider it valid
            return true;
        }
        // Not a wraparound - timestamp went backward or stayed same
        return false;
    }
    
    return true;
}

OutputTimestampMonitor::DiscontinuityStats OutputTimestampMonitor::getStats() const {
    return stats_;
}

void OutputTimestampMonitor::resetStats() {
    stats_ = DiscontinuityStats();
    video_state_ = PIDState();
    audio_state_ = PIDState();
    last_pcr_.reset();
    pcr_packet_count_ = 0;
    pcr_discontinuity_count_ = 0;
    start_time_ = std::chrono::steady_clock::now();
    last_summary_time_ = std::chrono::steady_clock::now();
}

void OutputTimestampMonitor::logSummaryIfNeeded() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_summary_time_).count();
    
    if (elapsed >= SUMMARY_INTERVAL_SEC) {
        auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
        
        std::cout << "\n[OutputTimestampMonitor] ========================================" << std::endl;
        std::cout << "[OutputTimestampMonitor] Timestamp Continuity Summary (" << SUMMARY_INTERVAL_SEC << "s interval, " << total_elapsed << "s total)" << std::endl;
        std::cout << "[OutputTimestampMonitor] ========================================" << std::endl;
        
        if (stats_.total_video_packets > 0) {
            std::cout << "[OutputTimestampMonitor] Video (PID " << video_pid_ << "):" << std::endl;
            std::cout << "[OutputTimestampMonitor]   Total packets: " << stats_.total_video_packets << std::endl;
            std::cout << "[OutputTimestampMonitor]   DTS discontinuities: " << stats_.video_dts_discontinuities << std::endl;
            std::cout << "[OutputTimestampMonitor]   PTS discontinuities: " << stats_.video_pts_discontinuities << std::endl;
        }
        
        if (stats_.total_audio_packets > 0) {
            std::cout << "[OutputTimestampMonitor] Audio (PID " << audio_pid_ << "):" << std::endl;
            std::cout << "[OutputTimestampMonitor]   Total packets: " << stats_.total_audio_packets << std::endl;
            std::cout << "[OutputTimestampMonitor]   DTS discontinuities: " << stats_.audio_dts_discontinuities << std::endl;
            std::cout << "[OutputTimestampMonitor]   PTS discontinuities: " << stats_.audio_pts_discontinuities << std::endl;
        }
        
        if (stats_.total_pcr_packets > 0) {
            std::cout << "[OutputTimestampMonitor] PCR:" << std::endl;
            std::cout << "[OutputTimestampMonitor]   Total packets: " << stats_.total_pcr_packets << std::endl;
            std::cout << "[OutputTimestampMonitor]   Discontinuities: " << stats_.pcr_discontinuities << std::endl;
        }
        
        uint64_t total_discontinuities = stats_.video_dts_discontinuities + 
                                         stats_.video_pts_discontinuities +
                                         stats_.audio_dts_discontinuities +
                                         stats_.audio_pts_discontinuities +
                                         stats_.pcr_discontinuities;
        
        std::cout << "[OutputTimestampMonitor] Total discontinuities: " << total_discontinuities << std::endl;
        std::cout << "[OutputTimestampMonitor] ========================================\n" << std::endl;
        
        last_summary_time_ = now;
    }
}