#pragma once

#include <cstdint>
#include <vector>
#include <optional>

/**
 * H.264 NAL Unit Types (ITU-T H.264 Table 7-1)
 */
enum class NALUnitType : uint8_t {
    UNSPECIFIED = 0,
    CODED_SLICE_NON_IDR = 1,    // P-frame or B-frame slice
    CODED_SLICE_DATA_PARTITION_A = 2,
    CODED_SLICE_DATA_PARTITION_B = 3,
    CODED_SLICE_DATA_PARTITION_C = 4,
    CODED_SLICE_IDR = 5,         // IDR frame (keyframe) - this is what we look for!
    SEI = 6,                     // Supplemental Enhancement Information
    SPS = 7,                     // Sequence Parameter Set - needed before IDR
    PPS = 8,                     // Picture Parameter Set - needed before IDR
    ACCESS_UNIT_DELIMITER = 9,
    END_OF_SEQUENCE = 10,
    END_OF_STREAM = 11,
    FILLER_DATA = 12,
    SPS_EXTENSION = 13,
    PREFIX_NAL = 14,
    SUBSET_SPS = 15,
    // 16-18 reserved
    CODED_SLICE_AUX = 19,
    CODED_SLICE_EXTENSION = 20,
    // 21-23 reserved
    // 24-31 unspecified
};

/**
 * Information about a video frame extracted from NAL units
 */
struct FrameInfo {
    bool is_idr = false;           // True if this is an IDR (keyframe)
    bool has_sps = false;          // True if SPS was found
    bool has_pps = false;          // True if PPS was found
    bool has_aud = false;          // True if Access Unit Delimiter found
    
    // Primary NAL type (the slice type, not SPS/PPS)
    std::optional<NALUnitType> primary_nal_type;
    
    // All NAL types found in this access unit
    std::vector<NALUnitType> nal_types;
    
    // Check if this frame can be used as a clean switch point
    bool isCleanSwitchPoint() const {
        return is_idr && has_sps && has_pps;
    }
    
    // Reset all fields
    void reset() {
        is_idr = false;
        has_sps = false;
        has_pps = false;
        has_aud = false;
        primary_nal_type.reset();
        nal_types.clear();
    }
};

/**
 * Parser for H.264 NAL units in MPEG-TS PES packets
 * 
 * This class parses the video elementary stream to detect:
 * - IDR frames (NAL type 5) for clean splice points
 * - SPS (NAL type 7) and PPS (NAL type 8) for decoder initialization
 * 
 * Usage:
 *   NALParser parser;
 *   FrameInfo info = parser.parseVideoPacket(pes_payload, payload_size);
 *   if (info.isCleanSwitchPoint()) {
 *       // Safe to switch streams here
 *   }
 */
class NALParser {
public:
    NALParser();
    ~NALParser();
    
    /**
     * Parse video data from a PES packet payload
     * 
     * @param data Pointer to PES payload (after PES header)
     * @param size Size of the payload in bytes
     * @return FrameInfo describing the NAL units found
     */
    FrameInfo parseVideoPayload(const uint8_t* data, size_t size);
    
    /**
     * Parse raw NAL unit data (for accumulating across multiple TS packets)
     * This is useful when a single access unit spans multiple TS packets.
     * 
     * @param data Pointer to raw video data
     * @param size Size of the data in bytes
     * @param info FrameInfo to update with findings
     */
    void parseNALUnits(const uint8_t* data, size_t size, FrameInfo& info);
    
    /**
     * Store the most recent SPS data for potential injection
     * @return Reference to stored SPS data (empty if none seen)
     */
    const std::vector<uint8_t>& getLastSPS() const { return last_sps_; }
    
    /**
     * Store the most recent PPS data for potential injection
     * @return Reference to stored PPS data (empty if none seen)
     */
    const std::vector<uint8_t>& getLastPPS() const { return last_pps_; }
    
    /**
     * Check if we have valid SPS and PPS stored
     */
    bool hasParameterSets() const { return !last_sps_.empty() && !last_pps_.empty(); }
    
    /**
     * Reset parser state (call when switching streams)
     */
    void reset();

private:
    /**
     * Find the next NAL unit start code in the data
     * Looks for 00 00 01 or 00 00 00 01
     * 
     * @param data Pointer to data buffer
     * @param size Size of buffer
     * @param offset Starting offset to search from
     * @return Offset of the NAL unit (after start code), or size if not found
     */
    size_t findNALStart(const uint8_t* data, size_t size, size_t offset);
    
    /**
     * Find the end of a NAL unit (start of next, or end of buffer)
     * 
     * @param data Pointer to data buffer
     * @param size Size of buffer
     * @param nal_start Start of current NAL unit
     * @return Offset of next start code, or size if at end
     */
    size_t findNALEnd(const uint8_t* data, size_t size, size_t nal_start);
    
    /**
     * Extract NAL unit type from NAL header byte
     * 
     * @param nal_header First byte of NAL unit (after start code)
     * @return NAL unit type
     */
    NALUnitType getNALType(uint8_t nal_header);
    
    /**
     * Store parameter set data (SPS or PPS)
     * 
     * @param data Pointer to NAL unit data (including header)
     * @param size Size of NAL unit
     * @param type NAL unit type (SPS or PPS)
     */
    void storeParameterSet(const uint8_t* data, size_t size, NALUnitType type);
    
    // Stored parameter sets for potential injection
    std::vector<uint8_t> last_sps_;
    std::vector<uint8_t> last_pps_;
    
    // Statistics for debugging
    uint64_t frames_parsed_ = 0;
    uint64_t idr_frames_found_ = 0;
};