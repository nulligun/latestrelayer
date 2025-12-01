#include "NALParser.h"
#include <iostream>
#include <cstring>

NALParser::NALParser() {
    std::cout << "[NALParser] Initialized - H.264 NAL unit parser ready" << std::endl;
}

NALParser::~NALParser() {
    std::cout << "[NALParser] Destroyed - parsed " << frames_parsed_ 
              << " frames, found " << idr_frames_found_ << " IDR frames" << std::endl;
}

FrameInfo NALParser::parseVideoPayload(const uint8_t* data, size_t size) {
    FrameInfo info;
    
    if (data == nullptr || size == 0) {
        return info;
    }
    
    parseNALUnits(data, size, info);
    
    frames_parsed_++;
    if (info.is_idr) {
        idr_frames_found_++;
        
        // Log IDR detection for debugging
        std::cout << "[NALParser] IDR frame detected! (total: " << idr_frames_found_ 
                  << ", has_sps=" << info.has_sps 
                  << ", has_pps=" << info.has_pps
                  << ", clean_switch=" << info.isCleanSwitchPoint() << ")" << std::endl;
    }
    
    return info;
}

void NALParser::parseNALUnits(const uint8_t* data, size_t size, FrameInfo& info) {
    if (data == nullptr || size < 4) {
        return;
    }
    
    size_t offset = 0;
    
    // Find and process all NAL units in the payload
    while (offset < size) {
        // Find the start of the next NAL unit
        size_t nal_start = findNALStart(data, size, offset);
        
        if (nal_start >= size) {
            // No more NAL units found
            break;
        }
        
        // Find the end of this NAL unit (start of next, or end of buffer)
        size_t nal_end = findNALEnd(data, size, nal_start);
        
        // Parse the NAL unit header
        if (nal_start < size) {
            uint8_t nal_header = data[nal_start];
            NALUnitType nal_type = getNALType(nal_header);
            
            info.nal_types.push_back(nal_type);
            
            // Process based on NAL type
            switch (nal_type) {
                case NALUnitType::CODED_SLICE_IDR:
                    info.is_idr = true;
                    if (!info.primary_nal_type.has_value()) {
                        info.primary_nal_type = nal_type;
                    }
                    break;
                    
                case NALUnitType::CODED_SLICE_NON_IDR:
                case NALUnitType::CODED_SLICE_DATA_PARTITION_A:
                case NALUnitType::CODED_SLICE_DATA_PARTITION_B:
                case NALUnitType::CODED_SLICE_DATA_PARTITION_C:
                    if (!info.primary_nal_type.has_value()) {
                        info.primary_nal_type = nal_type;
                    }
                    break;
                    
                case NALUnitType::SPS:
                    info.has_sps = true;
                    // Store SPS for potential injection
                    storeParameterSet(data + nal_start, nal_end - nal_start, nal_type);
                    break;
                    
                case NALUnitType::PPS:
                    info.has_pps = true;
                    // Store PPS for potential injection
                    storeParameterSet(data + nal_start, nal_end - nal_start, nal_type);
                    break;
                    
                case NALUnitType::ACCESS_UNIT_DELIMITER:
                    info.has_aud = true;
                    break;
                    
                default:
                    // Other NAL types (SEI, etc.) - just record them
                    break;
            }
        }
        
        // Move to the next NAL unit
        offset = nal_end;
    }
}

size_t NALParser::findNALStart(const uint8_t* data, size_t size, size_t offset) {
    // Look for start code: 00 00 01 or 00 00 00 01
    // Return the position of the first byte AFTER the start code (i.e., the NAL header)
    
    while (offset + 3 < size) {
        // Check for 3-byte start code: 00 00 01
        if (data[offset] == 0x00 && data[offset + 1] == 0x00 && data[offset + 2] == 0x01) {
            return offset + 3;  // Return position after start code
        }
        
        // Check for 4-byte start code: 00 00 00 01
        if (offset + 4 <= size &&
            data[offset] == 0x00 && data[offset + 1] == 0x00 && 
            data[offset + 2] == 0x00 && data[offset + 3] == 0x01) {
            return offset + 4;  // Return position after start code
        }
        
        offset++;
    }
    
    return size;  // No start code found
}

size_t NALParser::findNALEnd(const uint8_t* data, size_t size, size_t nal_start) {
    // Find the start of the next NAL unit, which marks the end of the current one
    // We search for the next start code pattern
    
    size_t offset = nal_start + 1;  // Start searching after the NAL header
    
    while (offset + 2 < size) {
        // Check for 3-byte start code: 00 00 01
        if (data[offset] == 0x00 && data[offset + 1] == 0x00 && data[offset + 2] == 0x01) {
            return offset;
        }
        
        // Check for 4-byte start code prefix: 00 00 00
        if (offset + 3 < size &&
            data[offset] == 0x00 && data[offset + 1] == 0x00 && 
            data[offset + 2] == 0x00 && data[offset + 3] == 0x01) {
            return offset;
        }
        
        offset++;
    }
    
    return size;  // This NAL unit extends to the end of the buffer
}

NALUnitType NALParser::getNALType(uint8_t nal_header) {
    // NAL header format (H.264):
    // +---------------+
    // |0|NRI|  Type   |
    // +---------------+
    //  Bit 7: forbidden_zero_bit (must be 0)
    //  Bits 5-6: nal_ref_idc (importance)
    //  Bits 0-4: nal_unit_type
    
    uint8_t type = nal_header & 0x1F;  // Lower 5 bits
    
    // Validate and return
    if (type <= 23) {
        return static_cast<NALUnitType>(type);
    }
    
    return NALUnitType::UNSPECIFIED;
}

void NALParser::storeParameterSet(const uint8_t* data, size_t size, NALUnitType type) {
    if (data == nullptr || size == 0) {
        return;
    }
    
    // Store the parameter set including the NAL header
    // (but not the start code - that's handled separately)
    std::vector<uint8_t>& target = (type == NALUnitType::SPS) ? last_sps_ : last_pps_;
    
    target.clear();
    target.reserve(size);
    target.insert(target.end(), data, data + size);
    
    std::cout << "[NALParser] Stored " << (type == NALUnitType::SPS ? "SPS" : "PPS")
              << " (" << size << " bytes)" << std::endl;
}

void NALParser::reset() {
    last_sps_.clear();
    last_pps_.clear();
    
    std::cout << "[NALParser] Reset - cleared stored parameter sets" << std::endl;
}