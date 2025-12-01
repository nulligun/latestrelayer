#pragma once

#include <tsduck.h>
#include "TSAnalyzer.h"
#include <cstdint>
#include <optional>
#include <chrono>

enum class Source {
    LIVE,
    FALLBACK
};

class TimestampManager {
public:
    TimestampManager();
    ~TimestampManager();
    
    // Process and adjust timestamps in a packet
    // Returns true if timestamps were successfully adjusted
    // For LIVE: Applies live_offset_ if non-zero (for FALLBACK→LIVE continuity)
    // For FALLBACK: Applies fallback_offset_ (for LIVE→FALLBACK continuity)
    bool adjustPacket(ts::TSPacket& packet, Source source, const TimestampInfo& input_ts);
    
    // Track timestamps from live packets
    // When live_offset_ is non-zero, stores adjusted timestamps to maintain timeline continuity
    // Updates last_live_pts_ and wall-clock time for gap-aware fallback transitions
    void trackLiveTimestamps(const TimestampInfo& ts_info, uint64_t adjusted_pts, uint64_t adjusted_dts, uint64_t adjusted_pcr);
    
    // Called when switching sources to calculate new offset
    // For LIVE: Calculates offset to continue from fallback output timeline (bidirectional continuity)
    // For FALLBACK: Calculates offset based on last live PTS + elapsed gap time
    void onSourceSwitch(Source new_source, const TimestampInfo& first_packet_ts);
    
    // Get current output timestamps
    uint64_t getLastOutputPTS() const { return last_output_pts_; }
    uint64_t getLastOutputDTS() const { return last_output_dts_; }
    uint64_t getLastOutputPCR() const { return last_output_pcr_; }
    
    // Reset manager state
    void reset();
    
private:
    // Adjust PTS/DTS in PES header
    void adjustPESTimestamps(ts::TSPacket& packet,
                            std::optional<uint64_t> new_pts,
                            std::optional<uint64_t> new_dts);
    
    // Adjust PCR in adaptation field
    void adjustPCR(ts::TSPacket& packet, uint64_t new_pcr);
    
    // Write PTS value to PES header
    void writePTS(uint8_t* pes_header, uint64_t pts, uint8_t marker);
    
    // Handle 33-bit timestamp wraparound
    uint64_t handleWraparound(uint64_t ts, uint64_t reference);
    
    // Separate timestamp offsets for PTS/DTS and PCR per source
    // This is necessary because PTS-to-PCR relationships can differ between streams
    int64_t live_pts_offset_;
    int64_t live_pcr_offset_;
    int64_t fallback_pts_offset_;
    int64_t fallback_pcr_offset_;
    
    // Last output timestamps (after adjustment)
    uint64_t last_output_pts_;
    uint64_t last_output_dts_;
    uint64_t last_output_pcr_;
    
    // Last original timestamps per source (before offset)
    // PTS/DTS tracking
    uint64_t last_live_pts_;
    uint64_t last_fallback_pts_;
    uint64_t last_fallback_dts_;
    
    // PCR tracking per source for accurate gap calculations
    uint64_t last_live_pcr_;
    uint64_t last_fallback_pcr_;
    
    // Wall-clock time tracking for gap-aware fallback transitions
    std::chrono::steady_clock::time_point last_live_packet_wall_time_;
    bool has_live_reference_;  // True once we've received at least one live packet
    
    // Detect loop boundaries in fallback stream
    bool detectLoopBoundary(Source source, uint64_t current_pts);
    
    // Frame duration estimate (in 90kHz units, ~40ms for 25fps)
    static constexpr uint64_t DEFAULT_FRAME_DURATION = 3600;
    
    // Maximum timestamp value (33-bit)
    static constexpr uint64_t MAX_TIMESTAMP = 0x1FFFFFFFFULL;
    
    // Loop detection threshold: 50% of max timestamp range
    // If timestamp jumps backward by more than this, it's likely a loop
    static constexpr uint64_t LOOP_DETECTION_THRESHOLD = MAX_TIMESTAMP / 2;
};