#pragma once

#include <tsduck.h>
#include <cstdint>
#include <optional>
#include <chrono>

/**
 * OutputTimestampMonitor - Monitor output timestamps for continuity
 * 
 * Tracks PCR, PTS, and DTS values in outgoing packets to detect discontinuities
 * that could cause FFmpeg "Packet corrupt" errors. Logs detailed alerts when
 * timestamps go backward or have unexpected gaps.
 */
class OutputTimestampMonitor {
public:
    struct DiscontinuityStats {
        uint64_t video_dts_discontinuities = 0;
        uint64_t video_pts_discontinuities = 0;
        uint64_t audio_dts_discontinuities = 0;
        uint64_t audio_pts_discontinuities = 0;
        uint64_t pcr_discontinuities = 0;
        uint64_t total_video_packets = 0;
        uint64_t total_audio_packets = 0;
        uint64_t total_pcr_packets = 0;
    };
    
    OutputTimestampMonitor();
    ~OutputTimestampMonitor();
    
    // Configure which PIDs to track
    void setVideoPID(uint16_t pid);
    void setAudioPID(uint16_t pid);
    
    // Check packet and log any discontinuities
    // Returns true if packet is OK, false if discontinuity detected
    bool checkPacket(const ts::TSPacket& packet);
    
    // Get/reset statistics
    DiscontinuityStats getStats() const;
    void resetStats();
    
    // Periodic summary logging (call periodically, logs if interval elapsed)
    void logSummaryIfNeeded();
    
private:
    struct PIDState {
        std::optional<uint64_t> last_dts;
        std::optional<uint64_t> last_pts;
        uint64_t packet_count = 0;
        uint64_t dts_discontinuity_count = 0;
        uint64_t pts_discontinuity_count = 0;
    };
    
    PIDState video_state_;
    PIDState audio_state_;
    
    // PCR tracking (can be on any PID)
    std::optional<uint64_t> last_pcr_;
    uint64_t pcr_packet_count_ = 0;
    uint64_t pcr_discontinuity_count_ = 0;
    
    uint16_t video_pid_ = 0;
    uint16_t audio_pid_ = 0;
    
    // Discontinuity counters
    DiscontinuityStats stats_;
    
    // For periodic logging
    std::chrono::steady_clock::time_point last_summary_time_;
    std::chrono::steady_clock::time_point start_time_;
    static constexpr int SUMMARY_INTERVAL_SEC = 10;
    
    // Detection methods
    bool checkVideoDTS(uint64_t current_dts, uint64_t packet_num);
    bool checkVideoPTS(uint64_t current_pts, uint64_t packet_num);
    bool checkAudioDTS(uint64_t current_dts, uint64_t packet_num);
    bool checkAudioPTS(uint64_t current_pts, uint64_t packet_num);
    bool checkPCR(uint64_t current_pcr, uint64_t packet_num);
    
    // Helper to extract timestamps from packet
    std::optional<uint64_t> extractPTS(const ts::TSPacket& packet);
    std::optional<uint64_t> extractDTS(const ts::TSPacket& packet);
    
    // Timestamp wraparound handling (33-bit timestamps wrap at 0x1FFFFFFFF)
    static constexpr uint64_t MAX_TIMESTAMP_33BIT = 0x1FFFFFFFFULL;
    bool isTimestampIncreasing(uint64_t prev, uint64_t current);
};