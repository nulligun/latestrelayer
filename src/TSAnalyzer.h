#pragma once

#include <tsduck.h>
#include <map>
#include <optional>
#include <cstdint>
#include <memory>
#include "NALParser.h"

struct StreamInfo {
    uint16_t video_pid = ts::PID_NULL;
    uint16_t audio_pid = ts::PID_NULL;
    uint16_t pcr_pid = ts::PID_NULL;
    uint16_t pmt_pid = ts::PID_NULL;
    uint16_t program_number = 0;
    uint8_t video_stream_type = 0;
    uint8_t audio_stream_type = 0;
    bool initialized = false;
    
    // Media packet validation counters
    uint32_t valid_video_packets = 0;      // Video packets with timestamps (PTS/DTS)
    uint32_t valid_audio_packets = 0;      // Audio packets with PUSI flag (start of PES)
    
    // Minimum required packets for stream validation
    // Video: requires 5 packets with timestamps to ensure valid PES stream
    // Audio: requires only 2 PUSI packets due to ~15x lower packet rate (128kbps vs 2Mbps)
    static constexpr uint32_t MIN_VALID_VIDEO_PACKETS = 5;
    static constexpr uint32_t MIN_VALID_AUDIO_PACKETS = 2;
    
    // PAT/PMT table version tracking for detecting updates
    uint8_t pat_version = 0xFF;  // 0xFF = not yet received
    uint8_t pmt_version = 0xFF;  // 0xFF = not yet received
};

struct TimestampInfo {
    std::optional<uint64_t> pts;
    std::optional<uint64_t> dts;
    std::optional<uint64_t> pcr;
};

class TSAnalyzer : private ts::TableHandlerInterface {
public:
    TSAnalyzer();
    ~TSAnalyzer();
    
    // Analyze a packet to extract stream information
    void analyzePacket(const ts::TSPacket& packet);
    
    // Get stream information
    const StreamInfo& getStreamInfo() const { return stream_info_; }
    bool isInitialized() const { return stream_info_.initialized; }
    
    // Check if stream has valid media data (not just PSI tables)
    bool hasValidMediaData() const {
        return stream_info_.initialized &&
               stream_info_.valid_video_packets >= StreamInfo::MIN_VALID_VIDEO_PACKETS &&
               stream_info_.valid_audio_packets >= StreamInfo::MIN_VALID_AUDIO_PACKETS;
    }
    
    // Extract timestamps from a packet
    TimestampInfo extractTimestamps(const ts::TSPacket& packet);
    
    // Extract frame information (IDR detection, SPS/PPS) from a video packet
    // Returns FrameInfo with is_idr, has_sps, has_pps flags
    // Only meaningful for video PID packets with PUSI set
    FrameInfo extractFrameInfo(const ts::TSPacket& packet);
    
    // Check if this is a video packet
    bool isVideoPacket(const ts::TSPacket& packet) const {
        return packet.getPID() == stream_info_.video_pid;
    }
    
    // Get the NAL parser (for accessing stored SPS/PPS)
    NALParser& getNALParser() { return nal_parser_; }
    const NALParser& getNALParser() const { return nal_parser_; }
    
    // Reset analyzer state
    void reset();
    
    // Get stored PAT packets for injection at splice points
    // Returns the raw TS packets that carried the last complete PAT
    const std::vector<ts::TSPacket>& getLastPATPackets() const { return last_pat_packets_; }
    
    // Get stored PMT packets for injection at splice points
    // Returns the raw TS packets that carried the last complete PMT
    const std::vector<ts::TSPacket>& getLastPMTPackets() const { return last_pmt_packets_; }
    
    // Check if we have valid PAT/PMT packets stored for injection
    bool hasPATPackets() const { return !last_pat_packets_.empty(); }
    bool hasPMTPackets() const { return !last_pmt_packets_.empty(); }
    
private:
    // TableHandlerInterface implementation
    virtual void handleTable(ts::SectionDemux&, const ts::BinaryTable&) override;
    
    // Process PAT (Program Association Table)
    void handlePAT(const ts::PAT& pat);
    
    // Process PMT (Program Map Table)
    void handlePMT(const ts::PMT& pmt);
    
    // Extract PTS/DTS from PES header
    std::optional<uint64_t> extractPTS(const uint8_t* pes_header);
    std::optional<uint64_t> extractDTS(const uint8_t* pes_header);
    
    StreamInfo stream_info_;
    
    // TSDuck context
    ts::DuckContext duck_;
    
    // Section demux for PAT/PMT parsing
    ts::SectionDemux demux_;
    
    // Track which PIDs we've seen
    std::map<uint16_t, uint64_t> pid_packet_count_;
    
    // NAL unit parser for IDR detection
    NALParser nal_parser_;
    
    // Storage for raw PAT/PMT packets for injection at splice points
    // Per splice.md: "Emit a new PAT/PMT at the splice" and "ensure tables repeat a few times for safety"
    std::vector<ts::TSPacket> last_pat_packets_;
    std::vector<ts::TSPacket> last_pmt_packets_;
    
    // Temporary buffers for collecting packets during table assembly
    std::vector<ts::TSPacket> pending_pat_packets_;
    std::vector<ts::TSPacket> pending_pmt_packets_;
};