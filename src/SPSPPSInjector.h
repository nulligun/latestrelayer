#pragma once

#include <tsduck.h>
#include <vector>
#include <cstdint>
#include <optional>

/**
 * SPS/PPS Injector for MPEG-TS streams
 * 
 * This class generates TS packets containing H.264 SPS (Sequence Parameter Set)
 * and PPS (Picture Parameter Set) NAL units for injection at splice points.
 * 
 * Per splice.md requirements:
 * "send SPS/PPS immediately before the first IDR to reset decoder state"
 * 
 * The generated packets maintain proper PES framing and can be inserted
 * directly into the TS stream before an IDR frame.
 */
class SPSPPSInjector {
public:
    SPSPPSInjector();
    ~SPSPPSInjector();
    
    /**
     * Create TS packets containing SPS and PPS NAL units
     * 
     * The packets are created with:
     * - Proper PES headers with optional timestamps
     * - NAL start codes (00 00 00 01)
     * - Correct TS packet framing with adaptation field padding if needed
     * 
     * @param sps Raw SPS NAL unit data (without start code, includes NAL header)
     * @param pps Raw PPS NAL unit data (without start code, includes NAL header)
     * @param video_pid The video PID to use for the generated packets
     * @param pts Optional PTS value (should match the following IDR frame)
     * @param dts Optional DTS value (should match the following IDR frame)
     * @return Vector of TS packets ready for output, or empty if invalid input
     */
    std::vector<ts::TSPacket> createSPSPPSPackets(
        const std::vector<uint8_t>& sps,
        const std::vector<uint8_t>& pps,
        uint16_t video_pid,
        std::optional<uint64_t> pts = std::nullopt,
        std::optional<uint64_t> dts = std::nullopt
    );
    
    /**
     * Create TS packets containing only SPS NAL unit
     * 
     * @param sps Raw SPS NAL unit data (without start code)
     * @param video_pid The video PID to use
     * @param pts Optional PTS value
     * @return Vector of TS packets
     */
    std::vector<ts::TSPacket> createSPSPackets(
        const std::vector<uint8_t>& sps,
        uint16_t video_pid,
        std::optional<uint64_t> pts = std::nullopt
    );
    
    /**
     * Create TS packets containing only PPS NAL unit
     * 
     * @param pps Raw PPS NAL unit data (without start code)
     * @param video_pid The video PID to use
     * @param pts Optional PTS value
     * @return Vector of TS packets
     */
    std::vector<ts::TSPacket> createPPSPackets(
        const std::vector<uint8_t>& pps,
        uint16_t video_pid,
        std::optional<uint64_t> pts = std::nullopt
    );
    
private:
    /**
     * Build PES packet data from NAL units
     * 
     * @param nal_units Vector of NAL unit data to include
     * @param pts Optional PTS timestamp
     * @param dts Optional DTS timestamp
     * @return PES packet data including header and payload
     */
    std::vector<uint8_t> buildPESPacket(
        const std::vector<std::vector<uint8_t>>& nal_units,
        std::optional<uint64_t> pts,
        std::optional<uint64_t> dts
    );
    
    /**
     * Packetize PES data into TS packets
     * 
     * @param pes_data The PES packet data
     * @param pid The PID for the TS packets
     * @return Vector of TS packets
     */
    std::vector<ts::TSPacket> packetizePES(
        const std::vector<uint8_t>& pes_data,
        uint16_t pid
    );
    
    /**
     * Write PTS/DTS timestamp in PES header format
     * 
     * @param buffer Buffer to write to (must have at least 5 bytes available)
     * @param timestamp The 33-bit timestamp value
     * @param marker The marker bits (0x21 for PTS only, 0x31 for PTS with DTS, 0x11 for DTS)
     */
    void writePESTimestamp(uint8_t* buffer, uint64_t timestamp, uint8_t marker);
    
    // NAL start code prefix
    static constexpr uint8_t NAL_START_CODE[] = {0x00, 0x00, 0x00, 0x01};
    static constexpr size_t NAL_START_CODE_SIZE = 4;
    
    // PES stream ID for video (H.264)
    static constexpr uint8_t VIDEO_STREAM_ID = 0xE0;
    
    // Minimum PES header size (without optional fields)
    static constexpr size_t PES_HEADER_MIN_SIZE = 9;
    
    // TS packet constants
    static constexpr size_t TS_PACKET_SIZE = 188;
    static constexpr size_t TS_HEADER_SIZE = 4;
    static constexpr size_t TS_MAX_PAYLOAD = TS_PACKET_SIZE - TS_HEADER_SIZE;
    
    // Internal continuity counter for generated packets
    // Note: This will be fixed by PIDMapper during stream processing
    uint8_t internal_cc_ = 0;
    
    // Statistics
    uint64_t injections_created_ = 0;
};