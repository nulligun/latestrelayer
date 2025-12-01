#include "TSAnalyzer.h"
#include "NALParser.h"
#include <iostream>
#include <cstring>

TSAnalyzer::TSAnalyzer() : duck_(), demux_(duck_, this) {
    // Register PID 0 (PAT) to start receiving PAT tables
    demux_.addPID(ts::PID_PAT);
    
    std::cout << "[TSAnalyzer] Initialized with SectionDemux - monitoring PAT on PID 0" << std::endl;
}

TSAnalyzer::~TSAnalyzer() {
}

void TSAnalyzer::analyzePacket(const ts::TSPacket& packet) {
    // Feed packet to section demux - it will call our handleTable callback
    demux_.feedPacket(packet);
    
    // Track packet counts per PID
    uint16_t pid = packet.getPID();
    pid_packet_count_[pid]++;
    
    // Only log first occurrence of important PIDs (PAT, PMT, video, audio)
    if (pid_packet_count_[pid] == 1) {
        bool is_important = (pid == ts::PID_PAT) ||
                           (pid == stream_info_.pmt_pid) ||
                           (pid == stream_info_.video_pid) ||
                           (pid == stream_info_.audio_pid);
        
        if (is_important) {
            std::cout << "[TSAnalyzer] Detected PID " << pid
                      << " (0x" << std::hex << pid << std::dec << ")" << std::endl;
        }
    }
    
    // Validate media packets with timestamps (only after stream is initialized)
    if (stream_info_.initialized) {
        // Check if this is a video packet
        if (pid == stream_info_.video_pid) {
            TimestampInfo ts_info = extractTimestamps(packet);
            if (ts_info.pts.has_value() || ts_info.dts.has_value()) {
                stream_info_.valid_video_packets++;
                
                // Log progress for first few valid packets
                if (stream_info_.valid_video_packets <= StreamInfo::MIN_VALID_VIDEO_PACKETS) {
                    std::cout << "[TSAnalyzer] Valid video packet with timestamps: "
                              << stream_info_.valid_video_packets << "/"
                              << StreamInfo::MIN_VALID_VIDEO_PACKETS << std::endl;
                }
            }
        }
        // Check if this is an audio packet
        // For audio, we count PUSI packets (start of PES) rather than requiring timestamps
        // This is because audio has ~15x fewer packets than video due to lower bitrate
        else if (pid == stream_info_.audio_pid) {
            if (packet.getPUSI()) {
                stream_info_.valid_audio_packets++;
                
                // Log progress for first few valid packets
                if (stream_info_.valid_audio_packets <= StreamInfo::MIN_VALID_AUDIO_PACKETS) {
                    std::cout << "[TSAnalyzer] Valid audio packet with PUSI: "
                              << stream_info_.valid_audio_packets << "/"
                              << StreamInfo::MIN_VALID_AUDIO_PACKETS << std::endl;
                }
            }
        }
    }
}

void TSAnalyzer::handleTable(ts::SectionDemux&, const ts::BinaryTable& table) {
    switch (table.tableId()) {
        case ts::TID_PAT: {
            ts::PAT pat(duck_, table);
            if (pat.isValid()) {
                handlePAT(pat);
            } else {
                std::cout << "[TSAnalyzer] ✗ Received invalid PAT" << std::endl;
            }
            break;
        }
        case ts::TID_PMT: {
            ts::PMT pmt(duck_, table);
            if (pmt.isValid()) {
                handlePMT(pmt);
            } else {
                std::cout << "[TSAnalyzer] ✗ Received invalid PMT" << std::endl;
            }
            break;
        }
        default:
            // Ignore other tables
            break;
    }
}

void TSAnalyzer::handlePAT(const ts::PAT& pat) {
    static int pat_count = 0;
    pat_count++;
    std::cout << "[TSAnalyzer] ✓ PAT received and parsed by SectionDemux (PAT #" << pat_count << ")" << std::endl;
    
    if (pat.pmts.empty()) {
        std::cout << "[TSAnalyzer] ✗ PAT contains no PMTs" << std::endl;
        return;
    }
    
    // Get first PMT PID
    uint16_t new_pmt_pid = pat.pmts.begin()->second;
    std::cout << "[TSAnalyzer] ✓ PAT parsed - PMT PID: " << new_pmt_pid
              << " (current pmt_pid=" << stream_info_.pmt_pid << ")" << std::endl;
    
    // Check if PMT PID changed (reconnection scenario)
    if (stream_info_.pmt_pid != ts::PID_NULL && stream_info_.pmt_pid != new_pmt_pid) {
        std::cout << "[TSAnalyzer] DEBUG: PMT PID changed from " << stream_info_.pmt_pid
                  << " to " << new_pmt_pid << std::endl;
    }
    
    stream_info_.pmt_pid = new_pmt_pid;
    
    // Now register to receive PMT on this PID
    std::cout << "[TSAnalyzer] DEBUG: Adding PMT PID " << stream_info_.pmt_pid << " to demux" << std::endl;
    demux_.addPID(stream_info_.pmt_pid);
}

void TSAnalyzer::handlePMT(const ts::PMT& pmt) {
    static int pmt_count = 0;
    pmt_count++;
    std::cout << "[TSAnalyzer] ✓ PMT received and parsed by SectionDemux (PMT #" << pmt_count << ")" << std::endl;
    std::cout << "[TSAnalyzer] DEBUG: PMT details - service_id=" << pmt.service_id
              << ", pcr_pid=" << pmt.pcr_pid
              << ", stream count=" << pmt.streams.size() << std::endl;
    
    stream_info_.pcr_pid = pmt.pcr_pid;
    
    // Find video and audio streams
    for (const auto& stream : pmt.streams) {
        if (stream.second.stream_type == ts::ST_MPEG2_VIDEO ||
            stream.second.stream_type == ts::ST_MPEG4_VIDEO ||
            stream.second.stream_type == ts::ST_AVC_VIDEO ||
            stream.second.stream_type == ts::ST_HEVC_VIDEO) {
            if (stream_info_.video_pid == ts::PID_NULL) {
                stream_info_.video_pid = stream.first;
                std::cout << "[TSAnalyzer] Found video stream: PID " << stream_info_.video_pid
                          << ", type 0x" << std::hex << static_cast<int>(stream.second.stream_type) << std::dec << std::endl;
            }
        } else if (stream.second.stream_type == ts::ST_MPEG2_AUDIO ||
                   stream.second.stream_type == ts::ST_MPEG4_AUDIO ||
                   stream.second.stream_type == ts::ST_AAC_AUDIO ||
                   stream.second.stream_type == ts::ST_AC3_AUDIO ||
                   stream.second.stream_type == ts::ST_EAC3_AUDIO) {
            if (stream_info_.audio_pid == ts::PID_NULL) {
                stream_info_.audio_pid = stream.first;
                std::cout << "[TSAnalyzer] Found audio stream: PID " << stream_info_.audio_pid
                          << ", type 0x" << std::hex << static_cast<int>(stream.second.stream_type) << std::dec << std::endl;
            }
        }
    }
    
    // Mark as initialized if we have at least video
    if (stream_info_.video_pid != ts::PID_NULL) {
        stream_info_.initialized = true;
        std::cout << "[TSAnalyzer] ✓ Stream initialized! Video PID: " << stream_info_.video_pid
                  << ", Audio PID: " << stream_info_.audio_pid
                  << ", PCR PID: " << stream_info_.pcr_pid << std::endl;
    } else {
        std::cout << "[TSAnalyzer] ✗ PMT parsed but no video PID found" << std::endl;
    }
}

TimestampInfo TSAnalyzer::extractTimestamps(const ts::TSPacket& packet) {
    TimestampInfo info;
    
    // Extract PCR if present
    if (packet.hasPCR()) {
        info.pcr = packet.getPCR();
    }
    
    // Extract PTS/DTS from PES header
    if (packet.getPUSI()) {  // Payload Unit Start Indicator
        const uint8_t* payload = packet.getPayload();
        size_t payload_size = packet.getPayloadSize();
        
        if (payload_size >= 14) {  // Minimum PES header size
            // Check for PES start code (00 00 01)
            if (payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
                // Check PTS/DTS flags
                uint8_t pts_dts_flags = (payload[7] >> 6) & 0x03;
                
                static int extraction_count = 0;
                extraction_count++;
                
                if (pts_dts_flags >= 2 && payload_size >= 14) {
                    // PTS is present
                    info.pts = extractPTS(payload + 9);
                }
                
                if (pts_dts_flags == 3 && payload_size >= 19) {
                    // DTS is present (both PTS and DTS)
                    info.dts = extractDTS(payload + 14);
                    
                    if (extraction_count % 100 == 1) {
                        std::cout << "[TSAnalyzer] Extracted PTS+DTS from packet (count=" << extraction_count
                                  << ", PTS=" << (info.pts.has_value() ? std::to_string(info.pts.value()) : "none")
                                  << ", DTS=" << info.dts.value() << ")" << std::endl;
                    }
                } else if (pts_dts_flags == 2 && info.pts.has_value()) {
                    // Only PTS present - generate DTS = PTS for timestamp continuity
                    // This is necessary because we need to adjust ALL timestamps
                    info.dts = info.pts.value();
                    
                    if (extraction_count % 100 == 1) {
                        std::cout << "[TSAnalyzer] Generated DTS=PTS for PTS-only packet (count=" << extraction_count
                                  << ", PTS=DTS=" << info.pts.value() << ")" << std::endl;
                    }
                }
            }
        }
    }
    
    return info;
}

std::optional<uint64_t> TSAnalyzer::extractPTS(const uint8_t* pes_header) {
    // PTS is 33 bits encoded in 5 bytes
    // Format: xxxx xxxM MMMMMMMM MMMMMMMM MMMMMMMM
    uint64_t pts = 0;
    pts |= ((uint64_t)(pes_header[0] & 0x0E)) << 29;
    pts |= ((uint64_t)(pes_header[1])) << 22;
    pts |= ((uint64_t)(pes_header[2] & 0xFE)) << 14;
    pts |= ((uint64_t)(pes_header[3])) << 7;
    pts |= ((uint64_t)(pes_header[4] & 0xFE)) >> 1;
    return pts;
}

std::optional<uint64_t> TSAnalyzer::extractDTS(const uint8_t* pes_header) {
    // DTS has same encoding as PTS
    uint64_t dts = 0;
    dts |= ((uint64_t)(pes_header[0] & 0x0E)) << 29;
    dts |= ((uint64_t)(pes_header[1])) << 22;
    dts |= ((uint64_t)(pes_header[2] & 0xFE)) << 14;
    dts |= ((uint64_t)(pes_header[3])) << 7;
    dts |= ((uint64_t)(pes_header[4] & 0xFE)) >> 1;
    return dts;
}

FrameInfo TSAnalyzer::extractFrameInfo(const ts::TSPacket& packet) {
    FrameInfo info;
    
    // Only process video packets with PUSI (Payload Unit Start Indicator)
    // This indicates the start of a new PES packet which contains the NAL units
    if (!isVideoPacket(packet) || !packet.getPUSI()) {
        return info;
    }
    
    const uint8_t* payload = packet.getPayload();
    size_t payload_size = packet.getPayloadSize();
    
    if (payload_size < 9) {  // Minimum PES header size check
        return info;
    }
    
    // Check for PES start code (00 00 01)
    if (payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01) {
        return info;
    }
    
    // Get PES header data length (byte 8)
    uint8_t pes_header_data_length = payload[8];
    
    // Calculate where the video payload starts (after PES header)
    // PES header: 3 (start code) + 1 (stream id) + 2 (packet length) + 3 (flags) + header_data_length
    size_t video_payload_offset = 9 + pes_header_data_length;
    
    if (video_payload_offset >= payload_size) {
        return info;  // No video data in this packet
    }
    
    // Parse NAL units from the video payload
    const uint8_t* video_data = payload + video_payload_offset;
    size_t video_size = payload_size - video_payload_offset;
    
    info = nal_parser_.parseVideoPayload(video_data, video_size);
    
    return info;
}

void TSAnalyzer::reset() {
    std::cout << "[TSAnalyzer] DEBUG: reset() called" << std::endl;
    std::cout << "[TSAnalyzer] DEBUG: Pre-reset state: initialized=" << stream_info_.initialized
              << ", video_pid=" << stream_info_.video_pid
              << ", audio_pid=" << stream_info_.audio_pid
              << ", valid_video=" << stream_info_.valid_video_packets
              << ", valid_audio=" << stream_info_.valid_audio_packets << std::endl;
    
    stream_info_ = StreamInfo();
    pid_packet_count_.clear();
    
    std::cout << "[TSAnalyzer] DEBUG: Calling demux_.reset()..." << std::endl;
    demux_.reset();
    
    // Re-register PID 0 (PAT) after reset to continue parsing PSI tables
    std::cout << "[TSAnalyzer] DEBUG: Re-adding PID_PAT to demux..." << std::endl;
    demux_.addPID(ts::PID_PAT);
    
    std::cout << "[TSAnalyzer] DEBUG: Post-reset state: initialized=" << stream_info_.initialized
              << ", video_pid=" << stream_info_.video_pid
              << ", audio_pid=" << stream_info_.audio_pid << std::endl;
    
    std::cout << "[TSAnalyzer] Reset complete - re-monitoring PAT on PID 0" << std::endl;
    
    // Reset NAL parser as well
    nal_parser_.reset();
}