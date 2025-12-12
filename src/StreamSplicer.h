#ifndef STREAM_SPLICER_H
#define STREAM_SPLICER_H

#include <cstdint>
#include <map>
#include <vector>
#include <tsduck.h>

/**
 * StreamSplicer - Handle clean splicing with timestamp rebasing
 * 
 * Based on multi2/src/tcp_main.cpp splice logic:
 * - Maintains global PTS/PCR offsets for continuous timeline
 * - Rebases timestamps using relative calculation
 * - Manages continuity counters across all PIDs
 * - Creates PAT/PMT with TSDuck
 * - Creates SPS/PPS injection packets
 */
class StreamSplicer {
public:
    StreamSplicer();
    
    // Initialize with PCR/PTS alignment offset (from first stream)
    void initializeWithAlignmentOffset(int64_t alignment_offset);
    
    // Rebase packet timestamps
    void rebasePacket(ts::TSPacket& packet, 
                     uint64_t pts_base, uint64_t pcr_base,
                     int64_t pcr_pts_alignment);
    
    // Fix continuity counter for packet
    void fixContinuityCounter(ts::TSPacket& packet);
    
    // Create PAT packet
    ts::TSPacket createPAT(uint16_t program_number, ts::PID pmt_pid);
    
    // Create PMT packet with stream info
    ts::TSPacket createPMT(uint16_t program_number, ts::PID pcr_pid,
                          ts::PID video_pid, ts::PID audio_pid,
                          uint8_t video_stream_type, uint8_t audio_stream_type);
    
    // Create SPS/PPS injection packets
    std::vector<ts::TSPacket> createSPSPPSPackets(
        const std::vector<uint8_t>& sps,
        const std::vector<uint8_t>& pps,
        ts::PID video_pid,
        uint64_t pts);
    
    // Update global offsets after processing a segment
    void updateOffsetsFromMaxTimestamps(uint64_t max_pts, uint64_t max_pcr);
    
    // Get current global offsets
    uint64_t getGlobalPTSOffset() const { return global_pts_offset_; }
    uint64_t getGlobalPCROffset() const { return global_pcr_offset_; }
    
private:
    // Global timestamp offsets
    uint64_t global_pts_offset_;
    uint64_t global_pcr_offset_;
    
    // Continuity counter state for all PIDs
    std::map<ts::PID, uint8_t> continuity_counters_;
    
    // TSDuck context for table generation
    ts::DuckContext duck_;
    
    // Helper: Get next continuity counter for PID
    uint8_t getNextCC(ts::PID pid);
};

#endif // STREAM_SPLICER_H