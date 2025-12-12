#include "StreamSplicer.h"
#include <iostream>
#include <cstring>

StreamSplicer::StreamSplicer()
    : global_pts_offset_(0),
      global_pcr_offset_(0) {
}

void StreamSplicer::initializeWithAlignmentOffset(int64_t alignment_offset) {
    // Per multi2 and splice.md: PTS must start at the alignment offset (not 0)
    // This preserves the decoder's buffer timing - the gap between PCR and PTS
    uint64_t pts_alignment_in_90khz = (alignment_offset > 0) ? (uint64_t)(alignment_offset / 300) : 0;
    global_pts_offset_ = pts_alignment_in_90khz;
    global_pcr_offset_ = 0;
    
    std::cout << "[StreamSplicer] Initialized with alignment offset: " << alignment_offset
              << " (27MHz) = " << pts_alignment_in_90khz << " (90kHz)" << std::endl;
    std::cout << "[StreamSplicer] Initial PTS offset: " << global_pts_offset_
              << ", PCR offset: " << global_pcr_offset_ << std::endl;
}

void StreamSplicer::rebasePacket(ts::TSPacket& packet, 
                                 uint64_t pts_base, uint64_t pcr_base,
                                 int64_t pcr_pts_alignment) {
    ts::PID pid = packet.getPID();
    
    // Rebase PCR if present
    if (packet.hasPCR()) {
        uint64_t pcr = packet.getPCR();
        uint64_t rebased_pcr = (pcr - pcr_base) + global_pcr_offset_;
        packet.setPCR(rebased_pcr);
    }
    
    // Rebase PTS/DTS if this is a PES packet start
    if (packet.getPUSI() && packet.hasPayload()) {
        size_t header_size = packet.getHeaderSize();
        uint8_t* payload = packet.b + header_size;
        size_t payload_size = ts::PKT_SIZE - header_size;
        
        // Check for PES start code
        if (payload_size >= 14 && payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
            uint8_t pts_dts_flags = (payload[7] >> 6) & 0x03;
            
            // Rebase PTS
            if (pts_dts_flags == 0x02 || pts_dts_flags == 0x03) {
                uint64_t pts = ((uint64_t)(payload[9] & 0x0E) << 29) |
                              ((uint64_t)(payload[10]) << 22) |
                              ((uint64_t)(payload[11] & 0xFE) << 14) |
                              ((uint64_t)(payload[12]) << 7) |
                              ((uint64_t)(payload[13] >> 1));
                
                pts = (pts - pts_base) + global_pts_offset_;
                pts &= 0x1FFFFFFFF;  // 33-bit wrap
                
                payload[9] = (payload[9] & 0xF1) | ((pts >> 29) & 0x0E);
                payload[10] = (pts >> 22) & 0xFF;
                payload[11] = (payload[11] & 0x01) | ((pts >> 14) & 0xFE);
                payload[12] = (pts >> 7) & 0xFF;
                payload[13] = (payload[13] & 0x01) | ((pts << 1) & 0xFE);
            }
            
            // Rebase DTS
            if (pts_dts_flags == 0x03 && payload_size >= 19) {
                uint64_t dts = ((uint64_t)(payload[14] & 0x0E) << 29) |
                              ((uint64_t)(payload[15]) << 22) |
                              ((uint64_t)(payload[16] & 0xFE) << 14) |
                              ((uint64_t)(payload[17]) << 7) |
                              ((uint64_t)(payload[18] >> 1));
                
                dts = (dts - pts_base) + global_pts_offset_;
                dts &= 0x1FFFFFFFF;  // 33-bit wrap
                
                payload[14] = (payload[14] & 0xF1) | ((dts >> 29) & 0x0E);
                payload[15] = (dts >> 22) & 0xFF;
                payload[16] = (payload[16] & 0x01) | ((dts >> 14) & 0xFE);
                payload[17] = (dts >> 7) & 0xFF;
                payload[18] = (payload[18] & 0x01) | ((dts << 1) & 0xFE);
            }
        }
    }
}

void StreamSplicer::fixContinuityCounter(ts::TSPacket& packet) {
    // Only update CC for packets with payload
    if (packet.hasPayload()) {
        packet.setCC(getNextCC(packet.getPID()));
    }
}

uint8_t StreamSplicer::getNextCC(ts::PID pid) {
    if (continuity_counters_.find(pid) == continuity_counters_.end()) {
        continuity_counters_[pid] = 0;
        return 0;
    }
    continuity_counters_[pid] = (continuity_counters_[pid] + 1) & 0x0F;
    return continuity_counters_[pid];
}

ts::TSPacket StreamSplicer::createPAT(uint16_t program_number, ts::PID pmt_pid) {
    ts::PAT pat;
    pat.pmts[program_number] = pmt_pid;
    pat.setVersion(0);
    
    ts::BinaryTable table;
    pat.serialize(duck_, table);
    
    ts::TSPacketVector packets;
    ts::OneShotPacketizer packetizer(duck_, ts::PID_PAT);
    packetizer.addTable(table);
    packetizer.getPackets(packets);
    
    return packets.empty() ? ts::TSPacket() : packets[0];
}

ts::TSPacket StreamSplicer::createPMT(uint16_t program_number, ts::PID pcr_pid,
                                     ts::PID video_pid, ts::PID audio_pid,
                                     uint8_t video_stream_type, uint8_t audio_stream_type) {
    ts::PMT pmt;
    pmt.service_id = program_number;
    pmt.pcr_pid = pcr_pid;
    pmt.setVersion(0);
    
    pmt.streams[video_pid].stream_type = video_stream_type;
    
    if (audio_pid != ts::PID_NULL) {
        pmt.streams[audio_pid].stream_type = audio_stream_type;
    }
    
    ts::BinaryTable table;
    pmt.serialize(duck_, table);
    
    ts::TSPacketVector packets;
    ts::OneShotPacketizer packetizer(duck_, ts::PID(4096));  // PMT PID
    packetizer.addTable(table);
    packetizer.getPackets(packets);
    
    return packets.empty() ? ts::TSPacket() : packets[0];
}

std::vector<ts::TSPacket> StreamSplicer::createSPSPPSPackets(
    const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps,
    ts::PID video_pid,
    uint64_t pts) {
    
    std::vector<ts::TSPacket> packets;
    
    // Build elementary stream data (SPS + PPS with their start codes)
    std::vector<uint8_t> es_data;
    es_data.insert(es_data.end(), sps.begin(), sps.end());
    es_data.insert(es_data.end(), pps.begin(), pps.end());
    
    // Build PES packet
    std::vector<uint8_t> pes_packet;
    pes_packet.push_back(0x00);  // Start code prefix
    pes_packet.push_back(0x00);
    pes_packet.push_back(0x01);
    pes_packet.push_back(0xE0);  // Video stream ID
    
    // PES packet length: 3 (header extension) + 5 (PTS) + ES data size
    uint16_t pes_length = 8 + es_data.size();
    pes_packet.push_back((pes_length >> 8) & 0xFF);
    pes_packet.push_back(pes_length & 0xFF);
    
    pes_packet.push_back(0x80);  // Marker bits (10) + other flags
    pes_packet.push_back(0x80);  // PTS_DTS_flags = 10 (PTS only)
    pes_packet.push_back(0x05);  // PES header data length
    
    // Encode PTS (33 bits)
    pts &= 0x1FFFFFFFF;
    pes_packet.push_back(0x21 | ((pts >> 29) & 0x0E));
    pes_packet.push_back((pts >> 22) & 0xFF);
    pes_packet.push_back(0x01 | ((pts >> 14) & 0xFE));
    pes_packet.push_back((pts >> 7) & 0xFF);
    pes_packet.push_back(0x01 | ((pts << 1) & 0xFE));
    
    // Append ES data
    pes_packet.insert(pes_packet.end(), es_data.begin(), es_data.end());
    
    // Packetize into TS packets
    size_t offset = 0;
    bool first = true;
    constexpr size_t TS_HEADER_SIZE = 4;
    constexpr size_t MAX_PAYLOAD = ts::PKT_SIZE - TS_HEADER_SIZE;  // 184 bytes
    
    while (offset < pes_packet.size()) {
        ts::TSPacket pkt;
        std::memset(pkt.b, 0xFF, ts::PKT_SIZE);  // Pre-fill with stuffing
        
        pkt.b[0] = 0x47;  // Sync byte
        pkt.b[1] = (video_pid >> 8) & 0x1F;
        if (first) {
            pkt.b[1] |= 0x40;  // Set PUSI bit
            first = false;
        }
        pkt.b[2] = video_pid & 0xFF;
        
        size_t remaining_pes = pes_packet.size() - offset;
        
        if (remaining_pes >= MAX_PAYLOAD) {
            pkt.b[3] = 0x10;  // payload only, CC will be set by caller
            std::memcpy(pkt.b + TS_HEADER_SIZE, pes_packet.data() + offset, MAX_PAYLOAD);
            offset += MAX_PAYLOAD;
        } else {
            size_t adaptation_field_total = MAX_PAYLOAD - remaining_pes;
            pkt.b[3] = 0x30;  // adaptation field + payload
            pkt.b[4] = adaptation_field_total - 1;
            
            if (adaptation_field_total >= 2) {
                pkt.b[5] = 0x00;  // No flags set
            }
            
            size_t payload_start = TS_HEADER_SIZE + adaptation_field_total;
            std::memcpy(pkt.b + payload_start, pes_packet.data() + offset, remaining_pes);
            offset += remaining_pes;
        }
        
        packets.push_back(pkt);
    }
    
    return packets;
}

void StreamSplicer::updateOffsetsFromMaxTimestamps(uint64_t max_pts, uint64_t max_pcr) {
    if (max_pts > 0) {
        global_pts_offset_ = max_pts;
    }
    if (max_pcr > 0) {
        global_pcr_offset_ = max_pcr;
    }
    
    std::cout << "[StreamSplicer] Updated offsets: PTS=" << global_pts_offset_
              << ", PCR=" << global_pcr_offset_ << std::endl;
}