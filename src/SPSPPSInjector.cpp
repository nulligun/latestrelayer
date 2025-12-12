#include "SPSPPSInjector.h"
#include <iostream>
#include <cstring>

SPSPPSInjector::SPSPPSInjector() {
    std::cout << "[SPSPPSInjector] Initialized - ready to generate SPS/PPS injection packets" << std::endl;
}

SPSPPSInjector::~SPSPPSInjector() {
    std::cout << "[SPSPPSInjector] Destroyed - created " << injections_created_ << " injection packet sets" << std::endl;
}

std::vector<ts::TSPacket> SPSPPSInjector::createSPSPPSPackets(
    const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps,
    uint16_t video_pid,
    std::optional<uint64_t> pts,
    std::optional<uint64_t> dts
) {
    if (sps.empty() || pps.empty()) {
        std::cerr << "[SPSPPSInjector] Cannot create packets: SPS or PPS is empty" << std::endl;
        return {};
    }
    
    if (video_pid == ts::PID_NULL || video_pid > 0x1FFF) {
        std::cerr << "[SPSPPSInjector] Invalid video PID: " << video_pid << std::endl;
        return {};
    }
    
    std::cout << "[SPSPPSInjector] Creating SPS/PPS injection packets for PID " << video_pid
              << " (SPS: " << sps.size() << " bytes, PPS: " << pps.size() << " bytes)"
              << std::endl;
    
    // Build NAL units with start codes
    std::vector<std::vector<uint8_t>> nal_units;
    nal_units.push_back(sps);
    nal_units.push_back(pps);
    
    // Build PES packet containing both SPS and PPS
    std::vector<uint8_t> pes_data = buildPESPacket(nal_units, pts, dts);
    
    if (pes_data.empty()) {
        std::cerr << "[SPSPPSInjector] Failed to build PES packet" << std::endl;
        return {};
    }
    
    // Packetize into TS packets
    std::vector<ts::TSPacket> packets = packetizePES(pes_data, video_pid);
    
    if (!packets.empty()) {
        injections_created_++;
        std::cout << "[SPSPPSInjector] Created " << packets.size() << " TS packets for SPS/PPS injection"
                  << " (total injections: " << injections_created_ << ")" << std::endl;
    }
    
    return packets;
}

std::vector<ts::TSPacket> SPSPPSInjector::createSPSPackets(
    const std::vector<uint8_t>& sps,
    uint16_t video_pid,
    std::optional<uint64_t> pts
) {
    if (sps.empty()) {
        return {};
    }
    
    std::vector<std::vector<uint8_t>> nal_units;
    nal_units.push_back(sps);
    
    std::vector<uint8_t> pes_data = buildPESPacket(nal_units, pts, std::nullopt);
    return packetizePES(pes_data, video_pid);
}

std::vector<ts::TSPacket> SPSPPSInjector::createPPSPackets(
    const std::vector<uint8_t>& pps,
    uint16_t video_pid,
    std::optional<uint64_t> pts
) {
    if (pps.empty()) {
        return {};
    }
    
    std::vector<std::vector<uint8_t>> nal_units;
    nal_units.push_back(pps);
    
    std::vector<uint8_t> pes_data = buildPESPacket(nal_units, pts, std::nullopt);
    return packetizePES(pes_data, video_pid);
}

std::vector<uint8_t> SPSPPSInjector::buildPESPacket(
    const std::vector<std::vector<uint8_t>>& nal_units,
    std::optional<uint64_t> pts,
    std::optional<uint64_t> dts
) {
    // Calculate total payload size (NAL start codes + NAL data)
    size_t payload_size = 0;
    for (const auto& nal : nal_units) {
        payload_size += NAL_START_CODE_SIZE + nal.size();
    }
    
    // Determine PES header size
    size_t pes_header_data_length = 0;
    uint8_t pts_dts_flags = 0;
    
    if (pts.has_value() && dts.has_value()) {
        pts_dts_flags = 0xC0;  // Both PTS and DTS present
        pes_header_data_length = 10;  // 5 bytes PTS + 5 bytes DTS
    } else if (pts.has_value()) {
        pts_dts_flags = 0x80;  // PTS only
        pes_header_data_length = 5;
    } else {
        pts_dts_flags = 0x00;  // No timestamps
        pes_header_data_length = 0;
    }
    
    // PES packet size calculation
    // packet_length = 3 (flags + header_data_length) + header_data_length + payload_size
    size_t pes_packet_length = 3 + pes_header_data_length + payload_size;
    
    // Check if PES packet length fits in 16 bits (0 = unbounded for video is allowed)
    uint16_t pes_length_field = 0;
    if (pes_packet_length <= 0xFFFF) {
        pes_length_field = static_cast<uint16_t>(pes_packet_length);
    }
    // For video PES, length can be 0 meaning unbounded
    
    std::vector<uint8_t> pes_data;
    pes_data.reserve(6 + pes_packet_length);  // Start code (3) + stream ID (1) + length (2) + rest
    
    // PES start code: 00 00 01
    pes_data.push_back(0x00);
    pes_data.push_back(0x00);
    pes_data.push_back(0x01);
    
    // Stream ID (video)
    pes_data.push_back(VIDEO_STREAM_ID);
    
    // PES packet length (2 bytes, big endian)
    pes_data.push_back(static_cast<uint8_t>((pes_length_field >> 8) & 0xFF));
    pes_data.push_back(static_cast<uint8_t>(pes_length_field & 0xFF));
    
    // Optional PES header
    // Byte 1: '10' + PES_scrambling_control(2) + PES_priority(1) + data_alignment_indicator(1) 
    //         + copyright(1) + original_or_copy(1)
    // For our purposes: 0x80 = '10' + all zeros, plus data_alignment_indicator set
    pes_data.push_back(0x84);  // 10 00 0100 - data alignment indicator set
    
    // Byte 2: PTS_DTS_flags(2) + ESCR_flag(1) + ES_rate_flag(1) + DSM_trick_mode_flag(1)
    //         + additional_copy_info_flag(1) + PES_CRC_flag(1) + PES_extension_flag(1)
    pes_data.push_back(pts_dts_flags);
    
    // Byte 3: PES_header_data_length
    pes_data.push_back(static_cast<uint8_t>(pes_header_data_length));
    
    // Optional fields: PTS and/or DTS
    // Marker values are 4-bit nibbles that get shifted left by 4 in writePESTimestamp:
    // - PTS only: '0010' = 0x02 -> after shift becomes 0x2x
    // - PTS when DTS present: '0011' = 0x03 -> after shift becomes 0x3x
    // - DTS: '0001' = 0x01 -> after shift becomes 0x1x
    if (pts.has_value() && dts.has_value()) {
        // PTS
        uint8_t pts_bytes[5];
        writePESTimestamp(pts_bytes, pts.value(), 0x03);  // '0011' marker for PTS when both present
        pes_data.insert(pes_data.end(), pts_bytes, pts_bytes + 5);
        
        // DTS
        uint8_t dts_bytes[5];
        writePESTimestamp(dts_bytes, dts.value(), 0x01);  // '0001' marker for DTS
        pes_data.insert(pes_data.end(), dts_bytes, dts_bytes + 5);
    } else if (pts.has_value()) {
        // PTS only
        uint8_t pts_bytes[5];
        writePESTimestamp(pts_bytes, pts.value(), 0x02);  // '0010' marker for PTS only
        pes_data.insert(pes_data.end(), pts_bytes, pts_bytes + 5);
    }
    
    // Append NAL units with start codes
    for (const auto& nal : nal_units) {
        // NAL start code
        pes_data.insert(pes_data.end(), NAL_START_CODE, NAL_START_CODE + NAL_START_CODE_SIZE);
        // NAL data
        pes_data.insert(pes_data.end(), nal.begin(), nal.end());
    }
    
    return pes_data;
}

void SPSPPSInjector::writePESTimestamp(uint8_t* buffer, uint64_t timestamp, uint8_t marker) {
    // PTS/DTS encoding format:
    // 4 bits marker + 3 bits TS[32..30] + 1 marker bit
    // 15 bits TS[29..15] + 1 marker bit
    // 15 bits TS[14..0] + 1 marker bit
    
    // Ensure timestamp fits in 33 bits
    timestamp &= 0x1FFFFFFFF;
    
    buffer[0] = static_cast<uint8_t>((marker << 4) | ((timestamp >> 29) & 0x0E) | 0x01);
    buffer[1] = static_cast<uint8_t>((timestamp >> 22) & 0xFF);
    buffer[2] = static_cast<uint8_t>(((timestamp >> 14) & 0xFE) | 0x01);
    buffer[3] = static_cast<uint8_t>((timestamp >> 7) & 0xFF);
    buffer[4] = static_cast<uint8_t>(((timestamp << 1) & 0xFE) | 0x01);
}

std::vector<ts::TSPacket> SPSPPSInjector::packetizePES(
    const std::vector<uint8_t>& pes_data,
    uint16_t pid
) {
    std::vector<ts::TSPacket> packets;
    
    if (pes_data.empty()) {
        return packets;
    }
    
    size_t offset = 0;
    bool first_packet = true;
    
    while (offset < pes_data.size()) {
        ts::TSPacket packet;
        
        // Initialize packet to all 0xFF (stuffing bytes)
        std::memset(packet.b, 0xFF, TS_PACKET_SIZE);
        
        // Sync byte
        packet.b[0] = 0x47;
        
        // PID and flags
        // Byte 1: transport_error_indicator(1) + payload_unit_start_indicator(1) + 
        //         transport_priority(1) + PID[12..8](5)
        // Byte 2: PID[7..0](8)
        uint8_t pusi = first_packet ? 0x40 : 0x00;
        packet.b[1] = pusi | static_cast<uint8_t>((pid >> 8) & 0x1F);
        packet.b[2] = static_cast<uint8_t>(pid & 0xFF);
        
        // Calculate remaining PES data
        size_t remaining = pes_data.size() - offset;
        size_t payload_space = TS_MAX_PAYLOAD;
        
        // Check if we need adaptation field for padding
        bool need_adaptation = (remaining < payload_space);
        
        if (need_adaptation) {
            // adaptation_field_control = 0x30 (adaptation field + payload)
            packet.b[3] = 0x30 | (internal_cc_ & 0x0F);
            
            // Calculate padding needed
            size_t payload_with_adaptation = remaining;
            size_t adaptation_needed = payload_space - payload_with_adaptation;
            
            if (adaptation_needed < 2) {
                // Need at least 2 bytes for minimal adaptation field (length + flags)
                // If only 1 byte needed, we have edge case - just use padding
                adaptation_needed = 2;
                payload_with_adaptation = payload_space - adaptation_needed;
            }
            
            // Adaptation field length (not including the length byte itself)
            packet.b[4] = static_cast<uint8_t>(adaptation_needed - 1);
            
            if (adaptation_needed > 1) {
                // Flags byte (all zeros - no PCR, no splice, etc.)
                packet.b[5] = 0x00;
                
                // Fill rest with stuffing bytes (0xFF) - already done by memset
            }
            
            // Copy payload after adaptation field
            size_t payload_start = 4 + adaptation_needed;
            size_t copy_size = std::min(remaining, TS_PACKET_SIZE - payload_start);
            std::memcpy(&packet.b[payload_start], &pes_data[offset], copy_size);
            offset += copy_size;
        } else {
            // No adaptation field needed - full payload
            // adaptation_field_control = 0x10 (payload only)
            packet.b[3] = 0x10 | (internal_cc_ & 0x0F);
            
            // Copy payload
            size_t copy_size = std::min(remaining, TS_MAX_PAYLOAD);
            std::memcpy(&packet.b[4], &pes_data[offset], copy_size);
            offset += copy_size;
        }
        
        packets.push_back(packet);
        internal_cc_ = (internal_cc_ + 1) & 0x0F;
        first_packet = false;
    }
    
    return packets;
}