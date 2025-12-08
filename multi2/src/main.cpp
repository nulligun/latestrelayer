#include <iostream>
#include <iomanip>
#include <vector>
#include <map>
#include <string>
#include <cstdint>
#include <cstring>
#include <tsduck.h>

// Global DuckContext for TSDuck operations
static ts::DuckContext duck;

// Normalized PID values for output
constexpr ts::PID NORM_VIDEO_PID = 256;
constexpr ts::PID NORM_AUDIO_PID = 257;
constexpr ts::PID NORM_PMT_PID = 4096;

// Stream information extracted from a TS file
struct StreamInfo {
    ts::PID videoPID = ts::PID_NULL;
    ts::PID audioPID = ts::PID_NULL;
    ts::PID pcrPID = ts::PID_NULL;
    ts::PID pmtPID = ts::PID_NULL;
    uint16_t programNumber = 0;
    uint8_t videoStreamType = 0;
    uint8_t audioStreamType = 0;
};

// Information about a file to be spliced
struct FileInfo {
    std::string path;
    std::vector<ts::TSPacket> packets;
    StreamInfo streamInfo;
    size_t firstIDRIndex = 0;
    uint64_t ptsBase = 0;
    uint64_t dtsBase = 0;
    uint64_t pcrBase = 0;
    uint64_t ptsDuration = 0;
    uint64_t pcrDuration = 0;
    std::vector<uint8_t> spsData;  // SPS NAL unit bytes
    std::vector<uint8_t> ppsData;  // PPS NAL unit bytes
};

// Continuity counter manager
class ContinuityManager {
    std::map<ts::PID, uint8_t> counters;
public:
    uint8_t getNext(ts::PID pid) {
        if (counters.find(pid) == counters.end()) {
            counters[pid] = 0;
            return 0;
        }
        counters[pid] = (counters[pid] + 1) & 0x0F;
        return counters[pid];
    }
};

// Find IDR frame in PES payload
bool findIDRInPES(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size - 4; i++) {
        if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x01) {
            uint8_t nal_type = data[i+3] & 0x1F;
            if (nal_type == 5) {
                return true;
            }
        }
    }
    return false;
}

// Extract SPS and PPS NAL units from video stream
bool extractSPSPPS(const std::vector<ts::TSPacket>& packets, const StreamInfo& info,
                   std::vector<uint8_t>& spsData, std::vector<uint8_t>& ppsData) {
    std::vector<uint8_t> pes_buffer;
    bool foundSPS = false;
    bool foundPPS = false;
    
    for (const auto& pkt : packets) {
        if (pkt.getPID() != info.videoPID || !pkt.hasPayload()) {
            continue;
        }
        
        if (pkt.getPUSI()) {
            pes_buffer.clear();
        }
        
        size_t header_size = pkt.getHeaderSize();
        const uint8_t* payload = pkt.b + header_size;
        size_t payload_size = ts::PKT_SIZE - header_size;
        
        if (payload_size > 0) {
            pes_buffer.insert(pes_buffer.end(), payload, payload + payload_size);
        }
        
        // Look for NAL units in accumulated PES data
        if (pes_buffer.size() > 14) { // Skip PES header
            // Find PES header size to skip it
            size_t pes_header_len = 9;
            if (pes_buffer[0] == 0x00 && pes_buffer[1] == 0x00 && pes_buffer[2] == 0x01) {
                pes_header_len = 9 + pes_buffer[8];
            }
            
            // Scan for NAL start codes in ES data
            for (size_t i = pes_header_len; i < pes_buffer.size() - 4; i++) {
                if (pes_buffer[i] == 0x00 && pes_buffer[i+1] == 0x00 &&
                    (pes_buffer[i+2] == 0x01 || (pes_buffer[i+2] == 0x00 && pes_buffer[i+3] == 0x01))) {
                    
                    size_t nal_start = (pes_buffer[i+2] == 0x01) ? i : i + 1;
                    size_t nal_header = nal_start + 3;
                    
                    if (nal_header >= pes_buffer.size()) continue;
                    
                    uint8_t nal_type = pes_buffer[nal_header] & 0x1F;
                    
                    // Find end of this NAL unit (next start code or end of buffer)
                    size_t nal_end = pes_buffer.size();
                    for (size_t j = nal_header + 1; j < pes_buffer.size() - 3; j++) {
                        if (pes_buffer[j] == 0x00 && pes_buffer[j+1] == 0x00 &&
                            (pes_buffer[j+2] == 0x01 || (pes_buffer[j+2] == 0x00 && pes_buffer[j+3] == 0x01))) {
                            nal_end = j;
                            break;
                        }
                    }
                    
                    if (nal_type == 7 && !foundSPS) { // SPS
                        spsData.assign(pes_buffer.begin() + nal_start, pes_buffer.begin() + nal_end);
                        foundSPS = true;
                        std::cerr << "Found SPS NAL unit, size: " << spsData.size() << " bytes\n";
                    } else if (nal_type == 8 && !foundPPS) { // PPS
                        ppsData.assign(pes_buffer.begin() + nal_start, pes_buffer.begin() + nal_end);
                        foundPPS = true;
                        std::cerr << "Found PPS NAL unit, size: " << ppsData.size() << " bytes\n";
                    }
                    
                    if (foundSPS && foundPPS) {
                        return true;
                    }
                }
            }
        }
        
        // Stop after we have both or after processing enough packets
        if (foundSPS && foundPPS) {
            break;
        }
    }
    
    if (!foundSPS) {
        std::cerr << "Warning: SPS not found in stream\n";
    }
    if (!foundPPS) {
        std::cerr << "Warning: PPS not found in stream\n";
    }
    
    return foundSPS && foundPPS;
}

// Create TS packets containing SPS+PPS in a PES packet
std::vector<ts::TSPacket> createSPSPPSPackets(const std::vector<uint8_t>& spsData,
                                               const std::vector<uint8_t>& ppsData,
                                               uint64_t pts) {
    std::vector<ts::TSPacket> packets;
    
    // Build elementary stream data: SPS + PPS
    std::vector<uint8_t> es_data;
    es_data.insert(es_data.end(), spsData.begin(), spsData.end());
    es_data.insert(es_data.end(), ppsData.begin(), ppsData.end());
    
    // Build PES packet header
    std::vector<uint8_t> pes_packet;
    
    // PES start code
    pes_packet.push_back(0x00);
    pes_packet.push_back(0x00);
    pes_packet.push_back(0x01);
    pes_packet.push_back(0xE0); // Video stream ID
    
    // PES packet length (header + data)
    uint16_t pes_length = 8 + es_data.size(); // 8 = PES header after length field
    pes_packet.push_back((pes_length >> 8) & 0xFF);
    pes_packet.push_back(pes_length & 0xFF);
    
    // PES header flags
    pes_packet.push_back(0x80); // '10' marker bits + flags
    pes_packet.push_back(0x80); // PTS only (0x80), no DTS
    pes_packet.push_back(0x05); // PES header data length (5 bytes for PTS)
    
    // PTS (5 bytes)
    pts &= 0x1FFFFFFFF; // 33 bits
    pes_packet.push_back(0x21 | ((pts >> 29) & 0x0E)); // '0010' + PTS[32..30] + marker
    pes_packet.push_back((pts >> 22) & 0xFF);
    pes_packet.push_back(0x01 | ((pts >> 14) & 0xFE)); // PTS[22..15] + marker
    pes_packet.push_back((pts >> 7) & 0xFF);
    pes_packet.push_back(0x01 | ((pts << 1) & 0xFE)); // PTS[7..0] + marker
    
    // Append ES data
    pes_packet.insert(pes_packet.end(), es_data.begin(), es_data.end());
    
    // Packetize into TS packets
    size_t offset = 0;
    bool first = true;
    
    while (offset < pes_packet.size()) {
        ts::TSPacket pkt;
        pkt.init();
        pkt.setPID(NORM_VIDEO_PID);
        
        if (first) {
            pkt.setPUSI(true);
            first = false;
        }
        
        pkt.setPayloadSize(ts::PKT_SIZE - 4); // Full payload
        
        uint8_t* payload = pkt.b + 4;
        size_t to_copy = std::min(size_t(ts::PKT_SIZE - 4), pes_packet.size() - offset);
        
        std::memcpy(payload, pes_packet.data() + offset, to_copy);
        
        // Pad with 0xFF if needed
        if (to_copy < ts::PKT_SIZE - 4) {
            std::memset(payload + to_copy, 0xFF, ts::PKT_SIZE - 4 - to_copy);
        }
        
        packets.push_back(pkt);
        offset += to_copy;
    }
    
    std::cerr << "Created " << packets.size() << " TS packets for SPS+PPS\n";
    return packets;
}

// Load TS file into memory
bool loadTSFile(const std::string& path, std::vector<ts::TSPacket>& packets) {
    ts::TSFile file;
    if (!file.openRead(path, 0, CERR)) {
        std::cerr << "Error: Failed to open " << path << "\n";
        return false;
    }
    
    ts::TSPacket pkt;
    while (file.readPackets(&pkt, nullptr, 1, CERR) > 0) {
        packets.push_back(pkt);
    }
    file.close(CERR);
    
    std::cerr << "Loaded " << packets.size() << " packets from " << path << "\n";
    return true;
}

// Custom table handler for analyzing PAT/PMT
class StreamAnalyzer : public ts::TableHandlerInterface {
public:
    StreamInfo& info;
    bool foundPAT = false;
    bool foundPMT = false;
    
    StreamAnalyzer(StreamInfo& si) : info(si) {}
    
    virtual void handleTable(ts::SectionDemux&, const ts::BinaryTable& table) override {
        if (table.tableId() == ts::TID_PAT && !foundPAT) {
            ts::PAT pat(duck, table);
            if (pat.isValid() && !pat.pmts.empty()) {
                auto it = pat.pmts.begin();
                info.programNumber = it->first;
                info.pmtPID = it->second;
                foundPAT = true;
                std::cerr << "Found PAT: program=" << info.programNumber << ", PMT PID=" << info.pmtPID << "\n";
            }
        }
        else if (table.tableId() == ts::TID_PMT && !foundPMT) {
            ts::PMT pmt(duck, table);
            if (pmt.isValid()) {
                info.pcrPID = pmt.pcr_pid;
                
                for (const auto& stream : pmt.streams) {
                    if (stream.second.stream_type == 0x1B || // H.264
                        stream.second.stream_type == 0x24) { // HEVC
                        info.videoPID = stream.first;
                        info.videoStreamType = stream.second.stream_type;
                    } else if (stream.second.stream_type == 0x0F || // AAC
                               stream.second.stream_type == 0x03 || // MP3
                               stream.second.stream_type == 0x81) { // AC-3
                        info.audioPID = stream.first;
                        info.audioStreamType = stream.second.stream_type;
                    }
                }
                foundPMT = true;
                std::cerr << "Found PMT\n";
            }
        }
    }
};

// Analyze stream to find PIDs and stream types
bool analyzeStream(const std::vector<ts::TSPacket>& packets, StreamInfo& info) {
    StreamAnalyzer analyzer(info);
    ts::SectionDemux demux(duck, &analyzer);
    
    // Register PAT PID (PID 0) to receive PAT tables
    demux.addPID(ts::PID_PAT);
    
    // First pass: extract PAT to find PMT PID
    for (const auto& pkt : packets) {
        demux.feedPacket(pkt);
        if (analyzer.foundPAT) {
            break; // Stop after finding PAT
        }
    }
    
    if (info.pmtPID == ts::PID_NULL) {
        std::cerr << "Error: No PAT found in stream\n";
        return false;
    }
    
    // Register PMT PID to receive PMT tables
    demux.addPID(info.pmtPID);
    
    // Second pass: extract PMT to find video/audio PIDs
    for (const auto& pkt : packets) {
        demux.feedPacket(pkt);
        if (analyzer.foundPMT) {
            break; // Stop after finding PMT
        }
    }
    
    if (info.videoPID == ts::PID_NULL) {
        std::cerr << "Error: No video PID found\n";
        return false;
    }
    
    std::cerr << "Stream info: Video PID=" << info.videoPID
              << ", Audio PID=" << info.audioPID
              << ", PCR PID=" << info.pcrPID << "\n";
    
    return true;
}

// Find first IDR frame
bool findFirstIDR(const std::vector<ts::TSPacket>& packets, const StreamInfo& info, size_t& idrIndex) {
    std::vector<uint8_t> pes_buffer;
    
    for (size_t i = 0; i < packets.size(); i++) {
        const auto& pkt = packets[i];
        
        if (pkt.getPID() != info.videoPID || !pkt.hasPayload()) {
            continue;
        }
        
        if (pkt.getPUSI()) {
            if (!pes_buffer.empty() && findIDRInPES(pes_buffer.data(), pes_buffer.size())) {
                idrIndex = i;
                std::cerr << "First IDR found at packet " << i << "\n";
                return true;
            }
            pes_buffer.clear();
        }
        
        size_t header_size = pkt.getHeaderSize();
        const uint8_t* payload = pkt.b + header_size;
        size_t payload_size = ts::PKT_SIZE - header_size;
        
        if (payload_size > 0) {
            pes_buffer.insert(pes_buffer.end(), payload, payload + payload_size);
        }
    }
    
    if (!pes_buffer.empty() && findIDRInPES(pes_buffer.data(), pes_buffer.size())) {
        idrIndex = packets.size() - 1;
        std::cerr << "First IDR found at packet " << idrIndex << "\n";
        return true;
    }
    
    std::cerr << "Error: No IDR frame found\n";
    return false;
}

// Extract PTS from PES header
bool extractPTS(const uint8_t* pes, size_t size, uint64_t& pts) {
    if (size < 14) return false;
    if (pes[0] != 0x00 || pes[1] != 0x00 || pes[2] != 0x01) return false;
    
    uint8_t pts_dts_flags = (pes[7] >> 6) & 0x03;
    if (pts_dts_flags == 0x02 || pts_dts_flags == 0x03) {
        pts = ((uint64_t)(pes[9] & 0x0E) << 29) |
              ((uint64_t)(pes[10]) << 22) |
              ((uint64_t)(pes[11] & 0xFE) << 14) |
              ((uint64_t)(pes[12]) << 7) |
              ((uint64_t)(pes[13] >> 1));
        return true;
    }
    return false;
}

// Extract PCR from adaptation field
bool extractPCR(const ts::TSPacket& pkt, uint64_t& pcr) {
    if (!pkt.hasPCR()) return false;
    pcr = pkt.getPCR();
    return true;
}

// Find first PCR in packet buffer, searching from beginning
bool findPCRBase(const std::vector<ts::TSPacket>& packets, const StreamInfo& info, uint64_t& pcrBase) {
    for (size_t i = 0; i < packets.size(); i++) {
        const auto& pkt = packets[i];
        if (pkt.getPID() == info.pcrPID && extractPCR(pkt, pcrBase)) {
            std::cerr << "PCR base: " << pcrBase << " (found at packet index " << i << ")\n";
            return true;
        }
    }
    std::cerr << "[DEBUG] PCR not found in " << packets.size() << " packets with PCR PID " << info.pcrPID << "\n";
    return false;
}

// Get timestamp base values from file
bool getTimestampBases(const std::vector<ts::TSPacket>& packets, const StreamInfo& info,
                       size_t startIdx, uint64_t& ptsBase, uint64_t& pcrBase) {
    std::vector<uint8_t> video_pes_buffer;
    std::vector<uint8_t> audio_pes_buffer;
    uint64_t videoPTSBase = UINT64_MAX;
    uint64_t audioPTSBase = UINT64_MAX;
    bool foundVideoPTS = false;
    bool foundAudioPTS = false;
    bool foundPCR = false;
    size_t pcrSearched = 0;
    
    for (size_t i = startIdx; i < packets.size() && (!foundVideoPTS || !foundAudioPTS || !foundPCR); i++) {
        const auto& pkt = packets[i];
        
        // Look for first video PTS
        if (!foundVideoPTS && pkt.getPID() == info.videoPID && pkt.hasPayload()) {
            if (pkt.getPUSI()) {
                video_pes_buffer.clear();
            }
            
            size_t header_size = pkt.getHeaderSize();
            const uint8_t* payload = pkt.b + header_size;
            size_t payload_size = ts::PKT_SIZE - header_size;
            
            if (payload_size > 0) {
                video_pes_buffer.insert(video_pes_buffer.end(), payload, payload + payload_size);
                if (video_pes_buffer.size() >= 14 && extractPTS(video_pes_buffer.data(), video_pes_buffer.size(), videoPTSBase)) {
                    foundVideoPTS = true;
                    std::cerr << "Video PTS base: " << videoPTSBase << "\n";
                }
            }
        }
        
        // Look for first audio PTS
        if (!foundAudioPTS && info.audioPID != ts::PID_NULL && pkt.getPID() == info.audioPID && pkt.hasPayload()) {
            if (pkt.getPUSI()) {
                audio_pes_buffer.clear();
            }
            
            size_t header_size = pkt.getHeaderSize();
            const uint8_t* payload = pkt.b + header_size;
            size_t payload_size = ts::PKT_SIZE - header_size;
            
            if (payload_size > 0) {
                audio_pes_buffer.insert(audio_pes_buffer.end(), payload, payload + payload_size);
                if (audio_pes_buffer.size() >= 14 && extractPTS(audio_pes_buffer.data(), audio_pes_buffer.size(), audioPTSBase)) {
                    foundAudioPTS = true;
                    std::cerr << "Audio PTS base: " << audioPTSBase << "\n";
                }
            }
        }
        
        // Look for first PCR
        if (!foundPCR && pkt.getPID() == info.pcrPID) {
            pcrSearched++;
            if (extractPCR(pkt, pcrBase)) {
                foundPCR = true;
                std::cerr << "PCR base: " << pcrBase << " (found after searching " << pcrSearched << " packets with PCR PID)\n";
            }
        }
    }
    
    // Debug output
    if (!foundPCR) {
        std::cerr << "[DEBUG] PCR not found! Searched " << pcrSearched << " packets with PID " << info.pcrPID << "\n";
    }
    
    // Use minimum of video and audio PTS as the base to prevent underflow
    if (foundVideoPTS && foundAudioPTS) {
        ptsBase = std::min(videoPTSBase, audioPTSBase);
        std::cerr << "Using minimum PTS base: " << ptsBase
                  << " (video=" << videoPTSBase << ", audio=" << audioPTSBase << ")\n";
    } else if (foundVideoPTS) {
        ptsBase = videoPTSBase;
        std::cerr << "Using video PTS base: " << ptsBase << " (no audio PTS found)\n";
    } else if (foundAudioPTS) {
        ptsBase = audioPTSBase;
        std::cerr << "Using audio PTS base: " << ptsBase << " (no video PTS found)\n";
    } else {
        std::cerr << "[DEBUG] No PTS found at all!\n";
        return false;
    }
    
    // FIX: Allow success even if PCR not found, as long as we have PTS
    if (!foundPCR && (foundVideoPTS || foundAudioPTS)) {
        // CRITICAL: PCR and PTS have different time bases!
        // PTS is 90kHz, PCR is 27MHz, so multiply by 300
        pcrBase = ptsBase * 300;
        std::cerr << "Warning: PCR not found, using video PTS base for PCR: " << ptsBase
                  << " (converted to PCR: " << pcrBase << ")\n";
        return true;
    }
    
    return (foundVideoPTS || foundAudioPTS) && foundPCR;
}

// Create PAT packet
ts::TSPacket createPAT(uint16_t programNumber, ts::PID pmtPID) {
    ts::PAT pat;
    pat.pmts[programNumber] = pmtPID;
    pat.setVersion(0);
    
    ts::BinaryTable table;
    pat.serialize(duck, table);
    
    ts::TSPacketVector packets;
    ts::OneShotPacketizer packetizer(duck, ts::PID_PAT);
    packetizer.addTable(table);
    packetizer.getPackets(packets);
    
    if (packets.empty()) {
        std::cerr << "Error: Failed to create PAT packet\n";
        return ts::TSPacket();
    }
    
    return packets[0];
}

// Create PMT packet
ts::TSPacket createPMT(uint16_t programNumber, const StreamInfo& info) {
    ts::PMT pmt;
    pmt.service_id = programNumber;
    pmt.pcr_pid = NORM_VIDEO_PID;
    pmt.setVersion(0);
    
    pmt.streams[NORM_VIDEO_PID].stream_type = info.videoStreamType;
    
    if (info.audioPID != ts::PID_NULL) {
        pmt.streams[NORM_AUDIO_PID].stream_type = info.audioStreamType;
    }
    
    ts::BinaryTable table;
    pmt.serialize(duck, table);
    
    ts::TSPacketVector packets;
    ts::OneShotPacketizer packetizer(duck, NORM_PMT_PID);
    packetizer.addTable(table);
    packetizer.getPackets(packets);
    
    if (packets.empty()) {
        std::cerr << "Error: Failed to create PMT packet\n";
        return ts::TSPacket();
    }
    
    return packets[0];
}

// Rebase timestamps in packet
void rebasePacket(ts::TSPacket& pkt, const StreamInfo& origInfo,
                  uint64_t ptsOffset, uint64_t pcrOffset, uint64_t ptsBase, uint64_t pcrBase) {
    // Normalize PIDs
    ts::PID origPID = pkt.getPID();
    if (origPID == ts::PID_PAT || origPID == origInfo.pmtPID) {
        return; // Skip PSI packets
    }
    
    if (origPID == origInfo.videoPID) {
        pkt.setPID(NORM_VIDEO_PID);
    } else if (origPID == origInfo.audioPID) {
        pkt.setPID(NORM_AUDIO_PID);
    }
    
    // Rebase PCR using relative calculation to prevent underflow
    // Formula: new_PCR = (original_PCR - pcrBase) + pcrOffset
    if (pkt.hasPCR() && origPID == origInfo.pcrPID) {
        uint64_t pcr = pkt.getPCR();
        uint64_t rebasedPCR = (pcr - pcrBase) + pcrOffset;
        // Debug: Log first PCR rebasing calculation
        static bool firstPCRLogged = false;
        if (!firstPCRLogged) {
            std::cerr << "[PCR REBASE] original=" << pcr
                      << " base=" << pcrBase
                      << " offset=" << pcrOffset
                      << " rebased=" << rebasedPCR << "\n";
            firstPCRLogged = true;
        }
        pkt.setPCR(rebasedPCR);
    }
    
    // Rebase PTS/DTS in PES header
    if (pkt.getPUSI() && pkt.hasPayload()) {
        size_t header_size = pkt.getHeaderSize();
        uint8_t* payload = pkt.b + header_size;
        size_t payload_size = ts::PKT_SIZE - header_size;
        
        if (payload_size >= 14 && payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
            uint8_t pts_dts_flags = (payload[7] >> 6) & 0x03;
            
            if (pts_dts_flags == 0x02 || pts_dts_flags == 0x03) {
                // Extract and rebase PTS
                uint64_t pts = ((uint64_t)(payload[9] & 0x0E) << 29) |
                              ((uint64_t)(payload[10]) << 22) |
                              ((uint64_t)(payload[11] & 0xFE) << 14) |
                              ((uint64_t)(payload[12]) << 7) |
                              ((uint64_t)(payload[13] >> 1));
                // FIX: Subtract base before adding offset: new_pts = (original_pts - ptsBase) + ptsOffset
                pts = (pts - ptsBase) + ptsOffset;
                pts &= 0x1FFFFFFFF; // 33 bits
                
                payload[9] = (payload[9] & 0xF1) | ((pts >> 29) & 0x0E);
                payload[10] = (pts >> 22) & 0xFF;
                payload[11] = (payload[11] & 0x01) | ((pts >> 14) & 0xFE);
                payload[12] = (pts >> 7) & 0xFF;
                payload[13] = (payload[13] & 0x01) | ((pts << 1) & 0xFE);
            }
            
            if (pts_dts_flags == 0x03) {
                // Extract and rebase DTS
                uint64_t dts = ((uint64_t)(payload[14] & 0x0E) << 29) |
                              ((uint64_t)(payload[15]) << 22) |
                              ((uint64_t)(payload[16] & 0xFE) << 14) |
                              ((uint64_t)(payload[17]) << 7) |
                              ((uint64_t)(payload[18] >> 1));
                // FIX: Subtract base before adding offset
                dts = (dts - ptsBase) + ptsOffset;
                dts &= 0x1FFFFFFFF;
                
                payload[14] = (payload[14] & 0xF1) | ((dts >> 29) & 0x0E);
                payload[15] = (dts >> 22) & 0xFF;
                payload[16] = (payload[16] & 0x01) | ((dts >> 14) & 0xFE);
                payload[17] = (dts >> 7) & 0xFF;
                payload[18] = (payload[18] & 0x01) | ((dts << 1) & 0xFE);
            }
        }
    }
}


int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " [-loop N] <file1.ts> [file2.ts] [...]\n";
        std::cerr << "Options:\n";
        std::cerr << "  -loop N           Loop the entire input sequence N times (default: 1)\n";
        std::cerr << "\nOutput is written to stdout in binary format\n";
        std::cerr << "Examples:\n";
        std::cerr << "  " << argv[0] << " in1.ts in2.ts | ffmpeg -i pipe:0 -c copy out.mp4\n";
        std::cerr << "  " << argv[0] << " -loop 3 video.ts | ffmpeg -i pipe:0 -c copy out.mp4\n";
        return 1;
    }
    
    // Parse command line arguments
    int loopCount = 1;
    int fileArgStart = 1;
    
    // Parse options
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "-loop" && i + 1 < argc) {
            loopCount = std::atoi(argv[i + 1]);
            if (loopCount < 1) {
                std::cerr << "Error: Loop count must be >= 1\n";
                return 1;
            }
            i++; // Skip next arg
            fileArgStart = i + 1;
        } else if (arg[0] != '-') {
            // First non-option marks start of files
            fileArgStart = i;
            break;
        }
    }
    
    // Validate we have at least one input file
    if (fileArgStart >= argc) {
        std::cerr << "Error: At least one input file is required\n";
        return 1;
    }
    
    std::cerr << "Loop count: " << loopCount << "\n";
    
    std::vector<FileInfo> files;
    
    // Load and analyze all files
    for (int i = fileArgStart; i < argc; i++) {
        FileInfo file;
        file.path = argv[i];
        
        std::cerr << "\n=== Processing " << file.path << " ===\n";
        
        if (!loadTSFile(file.path, file.packets)) {
            return 1;
        }
        
        if (!analyzeStream(file.packets, file.streamInfo)) {
            return 1;
        }
        
        if (!findFirstIDR(file.packets, file.streamInfo, file.firstIDRIndex)) {
            return 1;
        }
        
        if (!getTimestampBases(file.packets, file.streamInfo, file.firstIDRIndex,
                               file.ptsBase, file.pcrBase)) {
            std::cerr << "Warning: Could not extract timestamp bases, using 0\n";
            file.ptsBase = 0;
            file.pcrBase = 0;
        }
        
        // Extract SPS and PPS NAL units
        if (!extractSPSPPS(file.packets, file.streamInfo, file.spsData, file.ppsData)) {
            std::cerr << "Warning: Could not extract SPS/PPS from " << file.path << "\n";
        }
        
        files.push_back(file);
    }
    
    // Use first file's stream info for output
    StreamInfo outputInfo = files[0].streamInfo;
    
    // Write PAT and PMT
    ContinuityManager ccManager;
    
    ts::TSPacket patPkt = createPAT(outputInfo.programNumber, NORM_PMT_PID);
    patPkt.setCC(ccManager.getNext(ts::PID_PAT));
    std::cout.write(reinterpret_cast<char*>(patPkt.b), ts::PKT_SIZE);
    
    ts::TSPacket pmtPkt = createPMT(outputInfo.programNumber, outputInfo);
    pmtPkt.setCC(ccManager.getNext(NORM_PMT_PID));
    std::cout.write(reinterpret_cast<char*>(pmtPkt.b), ts::PKT_SIZE);
    
    std::cerr << "\n=== Splicing streams ===\n";
    
    // Process each iteration
    uint64_t globalPTSOffset = 0;
    uint64_t globalPCROffset = 0;
    
    for (int loopIter = 0; loopIter < loopCount; loopIter++) {
        if (loopCount > 1) {
            std::cerr << "\n--- Loop iteration " << (loopIter + 1) << " of " << loopCount << " ---\n";
        }
        
        // Process all files
        for (size_t fileIdx = 0; fileIdx < files.size(); fileIdx++) {
            auto& file = files[fileIdx];
            
            std::cerr << "Splicing " << file.path << " (offset PTS=" << globalPTSOffset
                      << ", PCR=" << globalPCROffset << ")\n";
            
            // PCR offset for relative rebasing (passed to rebasePacket which does the subtraction)
            uint64_t filePCROffset = globalPCROffset;
            uint64_t maxPTS = 0;
            uint64_t maxPCR = 0;
            uint64_t minPTS = UINT64_MAX;
            bool firstPTSFound = false;
            
            // Inject SPS/PPS packets
            if (!file.spsData.empty() && !file.ppsData.empty()) {
                std::cerr << "  Injecting SPS/PPS packets before IDR frame\n";
                uint64_t idrPTS = globalPTSOffset;
                
                auto sppPackets = createSPSPPSPackets(file.spsData, file.ppsData, idrPTS);
                for (auto& pkt : sppPackets) {
                    if (pkt.hasPayload()) {
                        pkt.setCC(ccManager.getNext(pkt.getPID()));
                    }
                    std::cout.write(reinterpret_cast<char*>(pkt.b), ts::PKT_SIZE);
                }
            }
            
            // Write packets from first IDR
            for (size_t i = file.firstIDRIndex; i < file.packets.size(); i++) {
                ts::TSPacket pkt = file.packets[i];
                ts::PID origPID = pkt.getPID();
                
                if (origPID == ts::PID_PAT || origPID == file.streamInfo.pmtPID) {
                    continue;
                }
                
                rebasePacket(pkt, file.streamInfo, globalPTSOffset, filePCROffset, file.ptsBase, file.pcrBase);
                
                if (pkt.hasPayload()) {
                    pkt.setCC(ccManager.getNext(pkt.getPID()));
                }
                
                if (pkt.hasPCR()) {
                    maxPCR = std::max(maxPCR, pkt.getPCR());
                }
                
                // Track PTS
                if (pkt.getPUSI() && pkt.hasPayload()) {
                    ts::PID normalizedPID = pkt.getPID();
                    if (normalizedPID == NORM_VIDEO_PID || normalizedPID == NORM_AUDIO_PID) {
                        size_t header_size = pkt.getHeaderSize();
                        const uint8_t* payload = pkt.b + header_size;
                        size_t payload_size = ts::PKT_SIZE - header_size;
                        
                        if (payload_size >= 14 && payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
                            uint64_t pts;
                            if (extractPTS(payload, payload_size, pts)) {
                                if (!firstPTSFound) {
                                    firstPTSFound = true;
                                    minPTS = pts;
                                    std::cerr << "  First PTS: " << pts << "\n";
                                }
                                maxPTS = std::max(maxPTS, pts);
                            }
                        }
                    }
                }
                
                std::cout.write(reinterpret_cast<char*>(pkt.b), ts::PKT_SIZE);
            }
            
            std::cerr << "  File PTS range: " << minPTS << " -> " << maxPTS << " (duration: " << ((maxPTS - minPTS) / 90000.0) << "s)\n";
            
            // Update offsets
            if (fileIdx < files.size() - 1 || loopIter < loopCount - 1) {
                globalPTSOffset = maxPTS > 0 ? maxPTS : globalPTSOffset + 90000;
                globalPCROffset = maxPCR > 0 ? maxPCR : globalPCROffset + 27000000;
            }
        }
    }
    
    std::cerr << "\n=== Splicing complete ===\n";
    std::cerr << "Final PTS offset: " << globalPTSOffset << " (approx " << (globalPTSOffset / 90000.0) << "s)\n";
    std::cerr << "Final PCR offset: " << globalPCROffset << " (approx " << (globalPCROffset / 27000000.0) << "s)\n";
    
    return 0;
}