#pragma once

#include <tsduck.h>
#include <map>
#include <optional>
#include <cstdint>

struct StreamInfo {
    uint16_t video_pid = ts::PID_NULL;
    uint16_t audio_pid = ts::PID_NULL;
    uint16_t pcr_pid = ts::PID_NULL;
    uint16_t pmt_pid = ts::PID_NULL;
    bool initialized = false;
    
    // Media packet validation counters
    uint32_t valid_video_packets = 0;
    uint32_t valid_audio_packets = 0;
    
    // Minimum required packets for stream validation
    static constexpr uint32_t MIN_VALID_VIDEO_PACKETS = 5;
    static constexpr uint32_t MIN_VALID_AUDIO_PACKETS = 5;
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
    
    // Reset analyzer state
    void reset();
    
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
};