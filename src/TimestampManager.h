#pragma once

#include <tsduck.h>
#include "TSAnalyzer.h"
#include <cstdint>
#include <optional>

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
    bool adjustPacket(ts::TSPacket& packet, Source source, const TimestampInfo& input_ts);
    
    // Called when switching sources to calculate new offset
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
    
    // Ensure timestamp is monotonic (doesn't go backwards)
    uint64_t enforceMonotonic(uint64_t ts, uint64_t last_ts);
    
    // Handle 33-bit timestamp wraparound
    uint64_t handleWraparound(uint64_t ts, uint64_t reference);
    
    // Timestamp offsets for each source
    int64_t live_offset_;
    int64_t fallback_offset_;
    
    // Last output timestamps (after adjustment)
    uint64_t last_output_pts_;
    uint64_t last_output_dts_;
    uint64_t last_output_pcr_;
    
    // Last original timestamps per source (before offset)
    uint64_t last_live_pts_;
    uint64_t last_fallback_pts_;
    uint64_t last_fallback_dts_;
    
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