#pragma once

#include <tsduck.h>
#include <cstdint>
#include <optional>

/**
 * TimestampManager - Timestamp Rebasing for Continuous Output Timeline
 * 
 * Based on tcp_main.cpp approach:
 * - Uses simple rebasing formula: new_ts = (orig_ts - base) + globalOffset
 * - Maintains global PTS and PCR offsets across stream switches
 * - Preserves PCR/PTS alignment offset for decoder buffer timing
 * - Ensures monotonic output timeline regardless of source switches
 */
class TimestampManager {
public:
    TimestampManager();
    ~TimestampManager();
    
    /**
     * Rebase timestamps in a packet using current stream bases and global offsets
     * @param packet The TS packet to modify
     * @param ptsBase PTS base from current stream
     * @param pcrBase PCR base from current stream  
     * @param pcrPtsAlignmentOffset PCR/PTS alignment offset from stream
     */
    void rebasePacket(ts::TSPacket& packet, 
                     uint64_t ptsBase, 
                     uint64_t pcrBase,
                     int64_t pcrPtsAlignmentOffset);
    
    /**
     * Update global offsets after processing a segment
     * Called at the end of each stream segment before switching
     * @param segmentDurationPTS Duration of segment in PTS units (90kHz)
     */
    void updateOffsetsAfterSegment(uint64_t segmentDurationPTS);
    
    /**
     * Initialize with PCR/PTS alignment offset
     * This must be called BEFORE processing the first stream to ensure
     * proper decoder buffer timing.
     *
     * Per splice.md: The alignment offset preserves the PCR/PTS relationship
     * that provides the decoder its buffering margin. Without this, both
     * PCR and PTS would rebase to near-zero, destroying the buffer time.
     *
     * @param alignmentOffset PCR/PTS alignment offset in 27MHz PCR units
     */
    void initializeWithAlignmentOffset(int64_t alignmentOffset);
    
    /**
     * Get current global offsets
     */
    uint64_t getGlobalPTSOffset() const { return global_pts_offset_; }
    uint64_t getGlobalPCROffset() const { return global_pcr_offset_; }
    
    /**
     * Reset manager state
     */
    void reset();
    
private:
    // Write PTS/DTS value to PES header field
    void writePTS(uint8_t* pes_header, uint64_t pts, uint8_t marker);
    
    // Global timestamp offsets (accumulated across all switches)
    uint64_t global_pts_offset_;
    uint64_t global_pcr_offset_;
    
    // Maximum timestamp value (33-bit for PTS/DTS, used for wraparound)
    static constexpr uint64_t MAX_TIMESTAMP_33BIT = 0x1FFFFFFFFULL;
};