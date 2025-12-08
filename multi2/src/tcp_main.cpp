/*
 * TCP MPEG-TS Stream Splicer
 * 
 * This program reads MPEG-TS streams from FFmpeg over TCP and splices them together.
 * 
 * TRANSPORT: TCP (SOCK_STREAM)
 * - FFmpeg acts as TCP server (listen mode)
 * - Application connects as TCP client
 * - Reliable delivery (no packet loss like UDP)
 * - Simpler reassembly (continuous byte stream, no datagram boundaries)
 * 
 * FFmpeg command:
 *   ffmpeg -re -i input.ts -f mpegts tcp://127.0.0.1:9000?listen=1
 * 
 * The TSStreamReassembler is still used to handle MPEG-TS packet boundaries
 * within the TCP byte stream, as packets may span across recv() calls.
 */

#include <iostream>
#include <iomanip>
#include <vector>
#include <map>
#include <string>
#include <cstdint>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <tsduck.h>
#include "ts_stream_reassembler.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// Global DuckContext for TSDuck operations
static ts::DuckContext duck;

// Normalized PID values for output
constexpr ts::PID NORM_VIDEO_PID = 256;
constexpr ts::PID NORM_AUDIO_PID = 257;
constexpr ts::PID NORM_PMT_PID = 4096;

// TCP reconnection configuration
constexpr int TCP_RECONNECT_DELAY_MS = 2000;  // 2 seconds between reconnection attempts

// Stream information extracted from a TS stream
struct StreamInfo {
    ts::PID videoPID = ts::PID_NULL;
    ts::PID audioPID = ts::PID_NULL;
    ts::PID pcrPID = ts::PID_NULL;
    ts::PID pmtPID = ts::PID_NULL;
    uint16_t programNumber = 0;
    uint8_t videoStreamType = 0;
    uint8_t audioStreamType = 0;
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

// Extract SPS and PPS NAL units from packets
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
        
        if (pes_buffer.size() > 14) {
            size_t pes_header_len = 9;
            if (pes_buffer[0] == 0x00 && pes_buffer[1] == 0x00 && pes_buffer[2] == 0x01) {
                pes_header_len = 9 + pes_buffer[8];
            }
            
            for (size_t i = pes_header_len; i < pes_buffer.size() - 4; i++) {
                if (pes_buffer[i] == 0x00 && pes_buffer[i+1] == 0x00 &&
                    (pes_buffer[i+2] == 0x01 || (pes_buffer[i+2] == 0x00 && pes_buffer[i+3] == 0x01))) {
                    
                    size_t nal_start = (pes_buffer[i+2] == 0x01) ? i : i + 1;
                    size_t nal_header = nal_start + 3;
                    
                    if (nal_header >= pes_buffer.size()) continue;
                    
                    uint8_t nal_type = pes_buffer[nal_header] & 0x1F;
                    
                    size_t nal_end = pes_buffer.size();
                    for (size_t j = nal_header + 1; j < pes_buffer.size() - 3; j++) {
                        if (pes_buffer[j] == 0x00 && pes_buffer[j+1] == 0x00 &&
                            (pes_buffer[j+2] == 0x01 || (pes_buffer[j+2] == 0x00 && pes_buffer[j+3] == 0x01))) {
                            nal_end = j;
                            break;
                        }
                    }
                    
                    if (nal_type == 7 && !foundSPS) {
                        spsData.assign(pes_buffer.begin() + nal_start, pes_buffer.begin() + nal_end);
                        foundSPS = true;
                    } else if (nal_type == 8 && !foundPPS) {
                        ppsData.assign(pes_buffer.begin() + nal_start, pes_buffer.begin() + nal_end);
                        foundPPS = true;
                    }
                    
                    if (foundSPS && foundPPS) {
                        return true;
                    }
                }
            }
        }
        
        if (foundSPS && foundPPS) {
            break;
        }
    }
    
    return foundSPS && foundPPS;
}

// Create TS packets containing SPS+PPS in a PES packet
std::vector<ts::TSPacket> createSPSPPSPackets(const std::vector<uint8_t>& spsData,
                                               const std::vector<uint8_t>& ppsData,
                                               uint64_t pts) {
    std::vector<ts::TSPacket> packets;
    
    // Build elementary stream data (SPS + PPS with their start codes)
    std::vector<uint8_t> es_data;
    es_data.insert(es_data.end(), spsData.begin(), spsData.end());
    es_data.insert(es_data.end(), ppsData.begin(), ppsData.end());
    
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
    
    size_t offset = 0;
    bool first = true;
    constexpr size_t TS_HEADER_SIZE = 4;
    constexpr size_t MAX_PAYLOAD = ts::PKT_SIZE - TS_HEADER_SIZE;  // 184 bytes
    
    while (offset < pes_packet.size()) {
        ts::TSPacket pkt;
        std::memset(pkt.b, 0xFF, ts::PKT_SIZE);  // Pre-fill with stuffing
        
        pkt.b[0] = 0x47;  // Sync byte
        pkt.b[1] = (NORM_VIDEO_PID >> 8) & 0x1F;
        if (first) {
            pkt.b[1] |= 0x40;  // Set PUSI bit
            first = false;
        }
        pkt.b[2] = NORM_VIDEO_PID & 0xFF;
        
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
    
    return packets.empty() ? ts::TSPacket() : packets[0];
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
    
    return packets.empty() ? ts::TSPacket() : packets[0];
}

// Rebase timestamps in packet
void rebasePacket(ts::TSPacket& pkt, const StreamInfo& origInfo,
                  uint64_t ptsOffset, uint64_t pcrOffset, uint64_t ptsBase, uint64_t pcrBase) {
    ts::PID origPID = pkt.getPID();
    if (origPID == ts::PID_PAT || origPID == origInfo.pmtPID) {
        return;
    }
    
    if (origPID == origInfo.videoPID) {
        pkt.setPID(NORM_VIDEO_PID);
    } else if (origPID == origInfo.audioPID) {
        pkt.setPID(NORM_AUDIO_PID);
    }
    
    if (pkt.hasPCR() && origPID == origInfo.pcrPID) {
        uint64_t pcr = pkt.getPCR();
        uint64_t rebasedPCR = (pcr - pcrBase) + pcrOffset;
        pkt.setPCR(rebasedPCR);
    }
    
    if (pkt.getPUSI() && pkt.hasPayload()) {
        size_t header_size = pkt.getHeaderSize();
        uint8_t* payload = pkt.b + header_size;
        size_t payload_size = ts::PKT_SIZE - header_size;
        
        if (payload_size >= 14 && payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
            uint8_t pts_dts_flags = (payload[7] >> 6) & 0x03;
            
            if (pts_dts_flags == 0x02 || pts_dts_flags == 0x03) {
                uint64_t pts = ((uint64_t)(payload[9] & 0x0E) << 29) |
                              ((uint64_t)(payload[10]) << 22) |
                              ((uint64_t)(payload[11] & 0xFE) << 14) |
                              ((uint64_t)(payload[12]) << 7) |
                              ((uint64_t)(payload[13] >> 1));
                pts = (pts - ptsBase) + ptsOffset;
                pts &= 0x1FFFFFFFF;
                
                payload[9] = (payload[9] & 0xF1) | ((pts >> 29) & 0x0E);
                payload[10] = (pts >> 22) & 0xFF;
                payload[11] = (payload[11] & 0x01) | ((pts >> 14) & 0xFE);
                payload[12] = (pts >> 7) & 0xFF;
                payload[13] = (payload[13] & 0x01) | ((pts << 1) & 0xFE);
            }
            
            if (pts_dts_flags == 0x03) {
                uint64_t dts = ((uint64_t)(payload[14] & 0x0E) << 29) |
                              ((uint64_t)(payload[15]) << 22) |
                              ((uint64_t)(payload[16] & 0xFE) << 14) |
                              ((uint64_t)(payload[17]) << 7) |
                              ((uint64_t)(payload[18] >> 1));
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

/*
 * TCPReader Class
 * 
 * Connects to FFmpeg TCP server and reads MPEG-TS stream.
 * - TCP client (connects to FFmpeg server)
 * - Blocking socket with continuous recv() loop
 * - TSStreamReassembler handles TS packet boundaries in byte stream
 * - Infinite reconnection on disconnect
 * - Background thread for continuous buffering
 */
class TCPReader {
private:
    std::string url;
    std::string streamName;
    std::string host;
    int port;
    int sockfd = -1;
    std::thread bgThread;
    std::atomic<bool> stopThread{false};
    std::atomic<bool> pidsReady{false};
    std::atomic<bool> idrReady{false};
    std::atomic<bool> audioReady{false};  // Track if audio has arrived after IDR
    std::atomic<bool> firstPacketReceived{false};
    std::atomic<bool> connected{false};
    
    std::mutex bufferMutex;
    std::condition_variable cv;
    std::vector<ts::TSPacket> rollingBuffer;
    size_t idrIndex = 0;
    size_t consumeIndex = 0;
    size_t lastSnapshotEnd = 0;  // Track where last snapshot ended to prevent packet loss
    size_t maxBufferPackets = 1500; // ~3 seconds at 2Mbps
    
    StreamInfo discoveredInfo;
    std::vector<uint8_t> spsData;
    std::vector<uint8_t> ppsData;
    uint64_t ptsBase = 0;
    uint64_t audioPtsBase = 0;  // Track audio PTS base separately
    uint64_t pcrBase = 0;
    int64_t pcrPtsAlignmentOffset = 0;  // Offset to maintain PCR/PTS sync
    std::chrono::steady_clock::time_point lastProgressReport;
    
    // Attempt TCP connection to FFmpeg server
    bool attemptConnection() {
        std::cerr << "[" << streamName << "] Attempting TCP connection to " << host << ":" << port << "...\n";
        
        // Create TCP socket
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            std::cerr << "[" << streamName << "] Failed to create TCP socket: " << strerror(errno) << "\n";
            return false;
        }
        
        // Set receive buffer size
        int bufsize = 2 * 1024 * 1024; // 2MB
        setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
        
        // Set TCP_NODELAY to reduce latency
        int flag = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        
        // Connect to FFmpeg server
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
            std::cerr << "[" << streamName << "] Invalid address: " << host << "\n";
            close(sockfd);
            sockfd = -1;
            return false;
        }
        
        if (::connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "[" << streamName << "] Connection failed: " << strerror(errno) << "\n";
            close(sockfd);
            sockfd = -1;
            return false;
        }
        
        std::cerr << "[" << streamName << "] TCP connection established to " << host << ":" << port << "\n";
        connected.store(true);
        return true;
    }
    
    void backgroundThreadFunc() {
        std::cerr << "[" << streamName << "] Background thread started\n";
        
        while (!stopThread.load()) {
            // Attempt connection with infinite retry
            while (!connected.load() && !stopThread.load()) {
                if (attemptConnection()) {
                    break;
                }
                std::cerr << "[" << streamName << "] Reconnecting in " << (TCP_RECONNECT_DELAY_MS / 1000.0) << " seconds...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(TCP_RECONNECT_DELAY_MS));
            }
            
            if (stopThread.load()) break;
            
            // Reset state for new connection
            {
                std::lock_guard<std::mutex> lock(bufferMutex);
                rollingBuffer.clear();
                idrIndex = 0;
                consumeIndex = 0;
            }
            pidsReady.store(false);
            idrReady.store(false);
            audioReady.store(false);
            firstPacketReceived.store(false);
            
            // Process TCP stream
            processTCPStream();
            
            // Connection lost
            std::cerr << "[" << streamName << "] TCP connection closed\n";
            connected.store(false);
            if (sockfd >= 0) {
                close(sockfd);
                sockfd = -1;
            }
        }
        
        std::cerr << "[" << streamName << "] Background thread stopped\n";
    }
    
    void processTCPStream() {
        ts::SectionDemux demux(duck);
        std::vector<uint8_t> pes_buffer;
        bool foundPAT = false;
        bool foundPMT = false;
        lastProgressReport = std::chrono::steady_clock::now();
        size_t totalPacketsReceived = 0;
        size_t pesStartIndex = 0;
        
        // PAT/PMT handler
        class StreamAnalyzer : public ts::TableHandlerInterface {
        public:
            StreamInfo& info;
            bool& foundPAT;
            bool& foundPMT;
            
            StreamAnalyzer(StreamInfo& si, bool& pat, bool& pmt) 
                : info(si), foundPAT(pat), foundPMT(pmt) {}
            
            virtual void handleTable(ts::SectionDemux&, const ts::BinaryTable& table) override {
                if (table.tableId() == ts::TID_PAT && !foundPAT) {
                    ts::PAT pat(duck, table);
                    if (pat.isValid() && !pat.pmts.empty()) {
                        auto it = pat.pmts.begin();
                        info.programNumber = it->first;
                        info.pmtPID = it->second;
                        foundPAT = true;
                    }
                }
                else if (table.tableId() == ts::TID_PMT && !foundPMT) {
                    ts::PMT pmt(duck, table);
                    if (pmt.isValid()) {
                        info.pcrPID = pmt.pcr_pid;
                        for (const auto& stream : pmt.streams) {
                            if (stream.second.stream_type == 0x1B || stream.second.stream_type == 0x24) {
                                info.videoPID = stream.first;
                                info.videoStreamType = stream.second.stream_type;
                            } else if (stream.second.stream_type == 0x0F ||
                                     stream.second.stream_type == 0x03 ||
                                     stream.second.stream_type == 0x81) {
                                info.audioPID = stream.first;
                                info.audioStreamType = stream.second.stream_type;
                            }
                        }
                        foundPMT = true;
                    }
                }
            }
        };
        
        StreamAnalyzer analyzer(discoveredInfo, foundPAT, foundPMT);
        demux.setTableHandler(&analyzer);
        demux.addPID(ts::PID_PAT);
        
        // TSStreamReassembler handles TS packet boundaries in TCP byte stream
        TSStreamReassembler reassembler;
        
        // TCP read buffer (32-64 KB for efficient TCP reads)
        constexpr size_t TCP_BUFFER_SIZE = 64 * 1024;  // 64 KB
        uint8_t tcpBuffer[TCP_BUFFER_SIZE];
        
        size_t totalBytesReceived = 0;
        size_t audioPacketsInBuffer = 0;
        size_t videoPacketsInBuffer = 0;
        
        std::cerr << "[" << streamName << "] Starting TCP recv loop (buffer size: " << TCP_BUFFER_SIZE << " bytes)\n";
        
        while (!stopThread.load() && connected.load()) {
            // Blocking TCP read
            ssize_t n = recv(sockfd, tcpBuffer, sizeof(tcpBuffer), 0);
            
            if (n < 0) {
                std::cerr << "[" << streamName << "] TCP recv error: " << strerror(errno) << "\n";
                break;  // Connection error, will trigger reconnect
            }
            
            if (n == 0) {
                std::cerr << "[" << streamName << "] TCP connection closed by peer (EOF)\n";
                break;  // Clean disconnect, will trigger reconnect
            }
            
            totalBytesReceived += n;
            
            // Feed data to reassembler
            reassembler.addData(tcpBuffer, n);
            
            // Get reassembled TS packets
            auto packets = reassembler.getPackets();
            
            for (auto& pkt : packets) {
                totalPacketsReceived++;
            
                if (!firstPacketReceived.load()) {
                    firstPacketReceived.store(true);
                    std::cerr << "[" << streamName << "] Receiving TCP data...\n";
                }
                
                // Periodic progress reporting (every 5 seconds)
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastProgressReport).count();
                if (elapsed >= 5) {
                    std::lock_guard<std::mutex> lock(bufferMutex);
                    std::string status;
                    if (!pidsReady.load()) {
                        if (!foundPAT) {
                            status = "searching for PAT/PMT...";
                        } else if (!foundPMT) {
                            status = "PAT found, searching for PMT...";
                        } else {
                            status = "PMT found, waiting for signal...";
                        }
                    } else if (!idrReady.load()) {
                        status = "waiting for IDR frame...";
                    } else {
                        status = "ready";
                    }
                    std::cerr << "[" << streamName << "] Progress: " << rollingBuffer.size()
                              << " packets buffered, " << status << "\n";
                    lastProgressReport = now;
                }
                
                // Phase 1: Parse PAT/PMT
                if (!pidsReady.load()) {
                    if (foundPAT && !demux.hasPID(discoveredInfo.pmtPID)) {
                        demux.addPID(discoveredInfo.pmtPID);
                    }
                    
                    demux.feedPacket(pkt);
                    
                    if (foundPAT && foundPMT) {
                        std::cerr << "[" << streamName << "] PAT/PMT discovery complete! Setting pidsReady=true\n";
                        std::cerr << "[" << streamName << "] Discovered: Video PID=" << discoveredInfo.videoPID
                                  << ", Audio PID=" << discoveredInfo.audioPID
                                  << ", PCR PID=" << discoveredInfo.pcrPID << "\n";
                        pidsReady.store(true);
                        cv.notify_all();
                    }
                }
                
                // Phase 2: Detect IDR and wait for audio
                if (pidsReady.load() && !idrReady.load()) {
                    if (pkt.getPID() == discoveredInfo.videoPID && pkt.hasPayload()) {
                        if (pkt.getPUSI()) {
                            if (!pes_buffer.empty() && findIDRInPES(pes_buffer.data(), pes_buffer.size())) {
                                std::lock_guard<std::mutex> lock(bufferMutex);
                                idrIndex = pesStartIndex;
                                std::cerr << "[" << streamName << "] IDR frame detected at index " << idrIndex << "\n";
                                
                                // If no audio stream, mark ready immediately
                                if (discoveredInfo.audioPID == ts::PID_NULL) {
                                    std::cerr << "[" << streamName << "] No audio stream, marking ready\n";
                                    idrReady.store(true);
                                    cv.notify_all();
                                } else {
                                    std::cerr << "[" << streamName << "] Waiting for first audio PES packet...\n";
                                    // Don't mark idrReady yet - wait for audio
                                }
                            }
                            pes_buffer.clear();
                            pesStartIndex = rollingBuffer.size();
                        }
                        
                        size_t header_size = pkt.getHeaderSize();
                        const uint8_t* payload = pkt.b + header_size;
                        size_t payload_size = ts::PKT_SIZE - header_size;
                        
                        if (payload_size > 0) {
                            pes_buffer.insert(pes_buffer.end(), payload, payload + payload_size);
                        }
                    }
                }
                
                // Phase 3: Wait for first audio PES after IDR (if audio exists)
                if (pidsReady.load() && idrIndex > 0 && !idrReady.load() &&
                    discoveredInfo.audioPID != ts::PID_NULL && !audioReady.load()) {
                    if (pkt.getPID() == discoveredInfo.audioPID && pkt.getPUSI() && pkt.hasPayload()) {
                        size_t audioPUSIIndex = rollingBuffer.size();  // Current position (before this packet added)
                        std::cerr << "[" << streamName << "] First audio PES packet detected at index "
                                  << audioPUSIIndex << "\n";
                        std::cerr << "[AUDIO-DIAG] IDR index=" << idrIndex << ", Audio PUSI index=" << audioPUSIIndex
                                  << " (audio starts " << (audioPUSIIndex - idrIndex) << " packets after IDR)\n";
                        
                        // Count how many audio continuation packets would be between idrIndex and here
                        size_t audioContBetween = 0;
                        for (size_t i = idrIndex; i < rollingBuffer.size(); i++) {
                            if (rollingBuffer[i].getPID() == discoveredInfo.audioPID && !rollingBuffer[i].getPUSI()) {
                                audioContBetween++;
                            }
                        }
                        if (audioContBetween > 0) {
                            std::cerr << "[AUDIO-DIAG] WARNING: " << audioContBetween
                                      << " orphan audio continuation packets between IDR and first audio PUSI!\n"
                                      << "  These packets are part of a PES that started BEFORE the IDR.\n"
                                      << "  They will be output without a PES header, causing decoder gaps.\n";
                        }
                        
                        audioReady.store(true);
                        idrReady.store(true);  // Now we're ready with both video and audio
                        cv.notify_all();
                    }
                }
                
                // Track packet types
                if (pidsReady.load()) {
                    if (pkt.getPID() == discoveredInfo.videoPID) {
                        videoPacketsInBuffer++;
                    } else if (pkt.getPID() == discoveredInfo.audioPID) {
                        audioPacketsInBuffer++;
                    }
                }
                
                // Always buffer
                {
                    std::lock_guard<std::mutex> lock(bufferMutex);
                    rollingBuffer.push_back(pkt);
                    
                    // Trim buffer if too large
                    if (rollingBuffer.size() > maxBufferPackets && idrReady.load()) {
                        size_t toRemove = rollingBuffer.size() - maxBufferPackets;
                        rollingBuffer.erase(rollingBuffer.begin(), rollingBuffer.begin() + toRemove);
                        if (idrIndex >= toRemove) {
                            idrIndex -= toRemove;
                        } else {
                            idrIndex = 0;
                        }
                        if (consumeIndex >= toRemove) {
                            consumeIndex -= toRemove;
                        } else {
                            consumeIndex = 0;
                        }
                        // Also adjust lastSnapshotEnd to prevent stale index references
                        if (lastSnapshotEnd >= toRemove) {
                            lastSnapshotEnd -= toRemove;
                        } else {
                            lastSnapshotEnd = 0;
                        }
                    }
                }
            }
            
            cv.notify_all();
        }
        
        std::cerr << "[" << streamName << "] TCP stream processing ended\n";
        std::cerr << "[" << streamName << "] Total bytes received: " << totalBytesReceived << "\n";
        std::cerr << "[" << streamName << "] Total packets: " << totalPacketsReceived << "\n";
    }
    
public:
    TCPReader(const std::string& tcpUrl, const std::string& name)
        : url(tcpUrl), streamName(name) {}
    
    ~TCPReader() {
        disconnect();
    }
    
    bool connect() {
        // Parse URL: tcp://host:port
        if (url.substr(0, 6) != "tcp://") {
            std::cerr << "[" << streamName << "] Invalid TCP URL format: " << url << "\n";
            return false;
        }
        
        std::string hostPort = url.substr(6);
        size_t colonPos = hostPort.find(':');
        if (colonPos == std::string::npos) {
            std::cerr << "[" << streamName << "] Invalid TCP URL, missing port: " << url << "\n";
            return false;
        }
        
        host = hostPort.substr(0, colonPos);
        port = std::stoi(hostPort.substr(colonPos + 1));
        
        // Start background thread (it will handle connection attempts)
        stopThread.store(false);
        bgThread = std::thread(&TCPReader::backgroundThreadFunc, this);
        
        return true;
    }
    
    void disconnect() {
        stopThread.store(true);
        if (sockfd >= 0) {
            shutdown(sockfd, SHUT_RDWR);
            close(sockfd);
            sockfd = -1;
        }
        if (bgThread.joinable()) {
            bgThread.join();
        }
    }
    
    void waitForStreamInfo() {
        std::cerr << "[" << streamName << "] Main thread waiting for stream info...\n";
        std::unique_lock<std::mutex> lock(bufferMutex);
        cv.wait(lock, [this]{ return pidsReady.load(); });
        std::cerr << "[" << streamName << "] Stream info ready\n";
    }
    
    void waitForIDR() {
        std::unique_lock<std::mutex> lock(bufferMutex);
        cv.wait(lock, [this]{ return idrReady.load(); });
    }
    
    StreamInfo getStreamInfo() const {
        return discoveredInfo;
    }
    
    std::vector<ts::TSPacket> getBufferedPacketsFromIDR() {
        std::lock_guard<std::mutex> lock(bufferMutex);
        if (idrIndex < rollingBuffer.size()) {
            // Record where this snapshot ends to prevent race condition
            lastSnapshotEnd = rollingBuffer.size();
            return std::vector<ts::TSPacket>(rollingBuffer.begin() + idrIndex,
                                            rollingBuffer.end());
        }
        return {};
    }
    
    size_t getLastSnapshotEnd() const {
        return lastSnapshotEnd;
    }
    
    bool extractTimestampBases() {
        auto packets = getBufferedPacketsFromIDR();
        if (packets.empty()) return false;
        
        extractSPSPPS(packets, discoveredInfo, spsData, ppsData);
        
        // Find video and audio PTS bases
        std::vector<uint8_t> video_pes_buffer;
        std::vector<uint8_t> audio_pes_buffer;
        uint64_t videoPtsBase = UINT64_MAX;
        uint64_t audioPtsBaseTemp = UINT64_MAX;
        bool foundVideoPTS = false;
        bool foundAudioPTS = false;
        
        for (const auto& pkt : packets) {
            // Extract video PTS base
            if (!foundVideoPTS && pkt.getPID() == discoveredInfo.videoPID && pkt.hasPayload()) {
                if (pkt.getPUSI()) {
                    video_pes_buffer.clear();
                }
                
                size_t header_size = pkt.getHeaderSize();
                const uint8_t* payload = pkt.b + header_size;
                size_t payload_size = ts::PKT_SIZE - header_size;
                
                if (payload_size > 0) {
                    video_pes_buffer.insert(video_pes_buffer.end(), payload, payload + payload_size);
                    if (video_pes_buffer.size() >= 14 && extractPTS(video_pes_buffer.data(), video_pes_buffer.size(), videoPtsBase)) {
                        foundVideoPTS = true;
                        std::cerr << "[" << streamName << "] Video PTS base: " << videoPtsBase << "\n";
                    }
                }
            }
            
            // Extract audio PTS base
            if (!foundAudioPTS && discoveredInfo.audioPID != ts::PID_NULL &&
                pkt.getPID() == discoveredInfo.audioPID && pkt.hasPayload()) {
                if (pkt.getPUSI()) {
                    audio_pes_buffer.clear();
                }
                
                size_t header_size = pkt.getHeaderSize();
                const uint8_t* payload = pkt.b + header_size;
                size_t payload_size = ts::PKT_SIZE - header_size;
                
                if (payload_size > 0) {
                    audio_pes_buffer.insert(audio_pes_buffer.end(), payload, payload + payload_size);
                    if (audio_pes_buffer.size() >= 14 && extractPTS(audio_pes_buffer.data(), audio_pes_buffer.size(), audioPtsBaseTemp)) {
                        foundAudioPTS = true;
                        std::cerr << "[" << streamName << "] Audio PTS base: " << audioPtsBaseTemp << "\n";
                    }
                }
            }
            
            if (foundVideoPTS && (foundAudioPTS || discoveredInfo.audioPID == ts::PID_NULL)) {
                break;
            }
        }
        
        // Use minimum of video and audio PTS as the base to prevent underflow
        if (foundVideoPTS && foundAudioPTS) {
            ptsBase = std::min(videoPtsBase, audioPtsBaseTemp);
            audioPtsBase = audioPtsBaseTemp;
            std::cerr << "[" << streamName << "] Using minimum PTS base: " << ptsBase
                      << " (video=" << videoPtsBase << ", audio=" << audioPtsBaseTemp << ")\n";
        } else if (foundVideoPTS) {
            ptsBase = videoPtsBase;
            audioPtsBase = videoPtsBase;
            std::cerr << "[" << streamName << "] Using video PTS base: " << ptsBase << " (no audio PTS found)\n";
        } else if (foundAudioPTS) {
            ptsBase = audioPtsBaseTemp;
            audioPtsBase = audioPtsBaseTemp;
            std::cerr << "[" << streamName << "] Using audio PTS base: " << ptsBase << " (no video PTS found)\n";
        } else {
            std::cerr << "[" << streamName << "] Warning: No PTS found!\n";
            return false;
        }
        
        // Extract the actual first PCR from the stream for reference
        uint64_t actualFirstPCR = 0;
        for (const auto& pkt : packets) {
            if (pkt.getPID() == discoveredInfo.pcrPID && extractPCR(pkt, actualFirstPCR)) {
                break;
            }
        }
        
        // Use actual PCR base to avoid underflow, but we'll add an alignment offset
        // to maintain proper PCR/PTS synchronization for the decoder.
        pcrBase = actualFirstPCR;
        
        // Calculate the PCR/PTS alignment offset
        // This is the difference between where PCR "should be" (based on PTS timing)
        // and where it actually is in the source stream.
        // Adding this offset to rebased PCR values ensures decoder clock sync with PTS.
        uint64_t expectedPCRFromPTS = ptsBase * 300;  // What PCR would be if aligned with PTS
        
        // Calculate and store alignment offset for later use
        // This offset ensures PCR and PTS maintain their original timing relationship
        if (actualFirstPCR > 0) {
            pcrPtsAlignmentOffset = (int64_t)expectedPCRFromPTS - (int64_t)actualFirstPCR;
            std::cerr << "[" << streamName << "] PCR base: " << pcrBase << "\n";
            std::cerr << "[" << streamName << "] PCR/PTS alignment offset: " << pcrPtsAlignmentOffset
                      << " (" << (pcrPtsAlignmentOffset / 27000.0) << "ms)\n";
            std::cerr << "[" << streamName << "] (expected PCR from PTS: " << expectedPCRFromPTS
                      << ", actual first PCR: " << actualFirstPCR << ")\n";
        } else {
            // Fallback: derive PCR from PTS if no PCR found
            pcrBase = ptsBase * 300;
            std::cerr << "[" << streamName << "] PCR not found, using PTS-derived PCR base: " << pcrBase << "\n";
        }
        
        return ptsBase > 0;
    }
    
    std::vector<uint8_t> getSPSData() const { return spsData; }
    std::vector<uint8_t> getPPSData() const { return ppsData; }
    uint64_t getPTSBase() const { return ptsBase; }
    uint64_t getAudioPTSBase() const { return audioPtsBase; }
    uint64_t getPCRBase() const { return pcrBase; }
    int64_t getPCRPTSAlignmentOffset() const { return pcrPtsAlignmentOffset; }
    std::string getURL() const { return url; }
    
    std::vector<ts::TSPacket> receivePackets(size_t maxPackets, int timeoutMs) {
        std::vector<ts::TSPacket> result;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        
        while (result.size() < maxPackets) {
            {
                std::unique_lock<std::mutex> lock(bufferMutex);
                if (consumeIndex < rollingBuffer.size()) {
                    size_t available = rollingBuffer.size() - consumeIndex;
                    size_t toCopy = std::min(maxPackets - result.size(), available);
                    
                    result.insert(result.end(),
                                rollingBuffer.begin() + consumeIndex,
                                rollingBuffer.begin() + consumeIndex + toCopy);
                    consumeIndex += toCopy;
                    
                    // Periodically trim consumed packets
                    if (consumeIndex > maxBufferPackets / 2) {
                        rollingBuffer.erase(rollingBuffer.begin(), rollingBuffer.begin() + consumeIndex);
                        if (idrIndex >= consumeIndex) {
                            idrIndex -= consumeIndex;
                        } else {
                            idrIndex = 0;
                        }
                        consumeIndex = 0;
                    }
                }
            }
            
            if (result.size() >= maxPackets) break;
            
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        return result;
    }
    
    void initConsumptionFromIndex(size_t index) {
        std::lock_guard<std::mutex> lock(bufferMutex);
        consumeIndex = index;
        std::cerr << "[" << streamName << "] Consumption started at index " << consumeIndex
                  << " (buffer size: " << rollingBuffer.size() << ")\n";
    }
    
    void initConsumptionFromCurrentPosition() {
        std::lock_guard<std::mutex> lock(bufferMutex);
        consumeIndex = rollingBuffer.size();
        std::cerr << "[" << streamName << "] Consumption started at current position " << consumeIndex << "\n";
    }
    
    // Reset for new loop iteration - triggers new IDR detection
    void resetForNewLoop() {
        std::lock_guard<std::mutex> lock(bufferMutex);
        idrReady.store(false);
        audioReady.store(false);
        idrIndex = 0;
        std::cerr << "[" << streamName << "] Reset for new loop - waiting for next IDR\n";
    }
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " -duration SECONDS [-loop N] <tcp://host1:port1> [tcp://host2:port2]\n";
        std::cerr << "\nOptions:\n";
        std::cerr << "  -duration SECONDS  Duration in seconds to pull from each stream\n";
        std::cerr << "  -loop N           Number of times to loop (default: 1)\n";
        std::cerr << "                    - With 1 source: loop N times from same source\n";
        std::cerr << "                    - With 2 sources: alternate between sources N times\n";
        std::cerr << "\nOutput is written to stdout in binary format\n";
        std::cerr << "\nExamples:\n";
        std::cerr << "  # Single source: 30 seconds from one stream (10s x 3 loops)\n";
        std::cerr << "  " << argv[0] << " -duration 10 -loop 3 tcp://127.0.0.1:9000 > output.ts\n";
        std::cerr << "\n  # Two sources: alternate between streams (10s each x 3 loops = 60s)\n";
        std::cerr << "  " << argv[0] << " -duration 10 -loop 3 tcp://127.0.0.1:9000 tcp://127.0.0.1:9001 > output.ts\n";
        std::cerr << "\nFFmpeg server command:\n";
        std::cerr << "  ffmpeg -re -i input.ts -f mpegts tcp://127.0.0.1:9000?listen=1\n";
        return 1;
    }
    
    int duration = 0;
    int loopCount = 1;
    std::string tcp1, tcp2;
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "-duration" && i + 1 < argc) {
            duration = std::atoi(argv[i + 1]);
            i++;
        } else if (arg == "-loop" && i + 1 < argc) {
            loopCount = std::atoi(argv[i + 1]);
            i++;
        } else if (arg.substr(0, 6) == "tcp://") {
            if (tcp1.empty()) {
                tcp1 = arg;
            } else if (tcp2.empty()) {
                tcp2 = arg;
            }
        }
    }
    
    if (duration <= 0) {
        std::cerr << "Error: -duration must be specified and > 0\n";
        return 1;
    }
    
    if (tcp1.empty()) {
        std::cerr << "Error: At least one TCP URL required\n";
        return 1;
    }
    
    bool dualSource = !tcp2.empty();
    
    std::cerr << "TCP Splicer Configuration:\n";
    std::cerr << "  Duration per stream: " << duration << " seconds\n";
    std::cerr << "  Loop count: " << loopCount << "\n";
    std::cerr << "  Mode: " << (dualSource ? "Dual source (alternating)" : "Single source") << "\n";
    std::cerr << "  TCP Stream 1: " << tcp1 << "\n";
    if (dualSource) {
        std::cerr << "  TCP Stream 2: " << tcp2 << "\n";
    }
    std::cerr << "\n";
    
    // Create TCP readers
    std::vector<std::unique_ptr<TCPReader>> readers;
    std::vector<StreamInfo> streamInfos;
    
    readers.push_back(std::make_unique<TCPReader>(tcp1, "TCP1"));
    if (dualSource) {
        readers.push_back(std::make_unique<TCPReader>(tcp2, "TCP2"));
    }
    
    // Connect to all streams
    for (size_t i = 0; i < readers.size(); i++) {
        std::string name = "TCP" + std::to_string(i + 1);
        std::cerr << "[" << name << "] Connecting to " << readers[i]->getURL() << "...\n";
        if (!readers[i]->connect()) {
            std::cerr << "[" << name << "] Failed to start connection\n";
            return 1;
        }
    }
    
    // Wait for stream discovery on all streams
    for (size_t i = 0; i < readers.size(); i++) {
        std::string name = "TCP" + std::to_string(i + 1);
        std::cerr << "[" << name << "] Waiting for stream info...\n";
        readers[i]->waitForStreamInfo();
        StreamInfo info = readers[i]->getStreamInfo();
        streamInfos.push_back(info);
        std::cerr << "[" << name << "] Stream discovered: Video PID=" << info.videoPID
                  << ", Audio PID=" << info.audioPID << ", PCR PID=" << info.pcrPID << "\n";
    }
    
    // Wait for IDR frames on all streams
    for (size_t i = 0; i < readers.size(); i++) {
        std::string name = "TCP" + std::to_string(i + 1);
        std::cerr << "[" << name << "] Waiting for IDR frame...\n";
        readers[i]->waitForIDR();
        std::cerr << "[" << name << "] IDR frame detected, buffer ready\n";
    }
    
    // Extract timestamp bases from all streams
    for (size_t i = 0; i < readers.size(); i++) {
        std::string name = "TCP" + std::to_string(i + 1);
        if (!readers[i]->extractTimestampBases()) {
            std::cerr << "[" << name << "] Warning: Could not extract timestamp bases\n";
        }
        std::cerr << "[" << name << "] PTS base=" << readers[i]->getPTSBase()
                  << ", PCR base=" << readers[i]->getPCRBase() << "\n";
    }
    
    // Write PAT and PMT (use first stream's info)
    ContinuityManager ccManager;
    StreamInfo& primaryInfo = streamInfos[0];
    
    ts::TSPacket patPkt = createPAT(primaryInfo.programNumber > 0 ? primaryInfo.programNumber : 1, NORM_PMT_PID);
    patPkt.setCC(ccManager.getNext(ts::PID_PAT));
    std::cout.write(reinterpret_cast<char*>(patPkt.b), ts::PKT_SIZE);
    
    ts::TSPacket pmtPkt = createPMT(primaryInfo.programNumber > 0 ? primaryInfo.programNumber : 1, primaryInfo);
    pmtPkt.setCC(ccManager.getNext(NORM_PMT_PID));
    std::cout.write(reinterpret_cast<char*>(pmtPkt.b), ts::PKT_SIZE);
    
    std::cerr << "\n=== Starting splice loop ===\n\n";
    
    // Get the PCR/PTS alignment offset from the first stream
    // In MPEG-TS, PTS is typically ahead of PCR (to allow decode buffer time)
    // We need to maintain this relationship after rebasing
    int64_t alignmentOffset = readers[0]->getPCRPTSAlignmentOffset();
    
    // The alignment offset is in 27MHz PCR units. Convert to 90kHz PTS units.
    // We ADD this to PTS (not PCR) because PTS should be AHEAD of PCR.
    // If alignmentOffset > 0, it means original PTS was ahead of original PCR.
    // We want rebased PTS to also be ahead of rebased PCR by the same amount.
    uint64_t ptsAlignmentIn90kHz = (alignmentOffset > 0) ? (uint64_t)(alignmentOffset / 300) : 0;
    uint64_t globalPTSOffset = ptsAlignmentIn90kHz;
    uint64_t globalPCROffset = 0;
    
    std::cerr << "[TIMING] PCR base (from stream): " << readers[0]->getPCRBase() << "\n";
    std::cerr << "[TIMING] PCR/PTS alignment offset: " << alignmentOffset
              << " (27MHz) = " << ptsAlignmentIn90kHz << " (90kHz) = "
              << (ptsAlignmentIn90kHz / 90.0) << "ms\n";
    std::cerr << "[TIMING] Initial PTS offset: " << globalPTSOffset
              << ", Initial PCR offset: " << globalPCROffset << "\n";
    uint64_t targetDurationPTS = duration * 90000;
    
    // Debug counters for audio analysis
    size_t totalAudioPacketsOutput = 0;
    size_t audioPacketsWithPayload = 0;
    size_t audioPacketsAdaptationOnly = 0;
    size_t nonEssentialPacketsOutput = 0;
    std::map<ts::PID, size_t> unexpectedPidCounts;
    std::map<ts::PID, uint8_t> lastCCValues;  // Track CC for discontinuity detection
    size_t audioCCDiscontinuities = 0;
    size_t videoCCDiscontinuities = 0;
    
    // Audio PTS tracking for timing analysis
    uint64_t firstAudioPTS = 0;
    uint64_t lastAudioPTS = 0;
    uint64_t firstVideoPTS = 0;
    size_t audioPTSCount = 0;
    size_t audioPTSGaps = 0;  // Count gaps > expected frame duration
    constexpr uint64_t EXPECTED_AUDIO_FRAME_PTS = 1920;  // ~21.3ms for 48kHz AAC (1024 samples)
    constexpr uint64_t AUDIO_GAP_THRESHOLD = EXPECTED_AUDIO_FRAME_PTS * 2;  // Gap > 2 frames
    
    // Detailed audio PUSI tracking
    size_t audioPUSICount = 0;
    size_t audioPUSIWithValidPES = 0;
    size_t audioPUSIWithPTS = 0;
    size_t audioPUSINoPTS = 0;
    size_t audioPUSIBadPESHeader = 0;
    size_t audioPUSITooSmall = 0;
    
    // Audio PES content analysis
    size_t totalAudioPESBytes = 0;
    size_t totalADTSFramesDetected = 0;
    
    // NEW: Track audio packet sequence to detect drops
    size_t expectedAudioPacketsPerPES = 0;
    size_t currentPESAudioPackets = 0;
    size_t incompleteAudioPES = 0;
    size_t lastAudioPUSIOutputIndex = 0;
    size_t totalOutputIndex = 0;
    
    // Helper to count ADTS frames in PES payload
    auto countADTSFrames = [](const uint8_t* data, size_t size) -> size_t {
        size_t count = 0;
        size_t pos = 0;
        
        // Skip PES header
        if (size >= 9 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) {
            size_t pesHeaderLen = 9 + data[8];  // 9 bytes + PES header data length
            if (pesHeaderLen < size) {
                pos = pesHeaderLen;
            }
        }
        
        // Scan for ADTS sync words (0xFFF)
        while (pos + 7 < size) {
            // ADTS sync: 12 bits = 0xFFF (byte0 = 0xFF, byte1 high nibble = 0xF)
            if (data[pos] == 0xFF && (data[pos + 1] & 0xF0) == 0xF0) {
                count++;
                // Get frame length from ADTS header (13 bits at bytes 3-5)
                size_t frameLen = ((data[pos + 3] & 0x03) << 11) |
                                  (data[pos + 4] << 3) |
                                  ((data[pos + 5] & 0xE0) >> 5);
                if (frameLen > 0 && pos + frameLen <= size) {
                    pos += frameLen;
                } else {
                    pos++;  // Invalid frame length, skip byte
                }
            } else {
                pos++;
            }
        }
        return count;
    };
    
    for (int loop = 0; loop < loopCount; loop++) {
        std::cerr << "=== Loop " << (loop + 1) << "/" << loopCount << " ===\n\n";
        
        for (size_t streamIdx = 0; streamIdx < readers.size(); streamIdx++) {
            // Reset IDR and re-extract bases before each stream (except the very first one)
            bool isFirstStream = (loop == 0 && streamIdx == 0);
            if (!isFirstStream) {
                TCPReader* reader = readers[streamIdx].get();
                std::string name = "TCP" + std::to_string(streamIdx + 1);
                
                // Reset IDR detection to wait for next IDR frame
                reader->resetForNewLoop();
                
                // Wait for new IDR
                std::cerr << "[" << name << "] Waiting for fresh IDR frame...\n";
                reader->waitForIDR();
                std::cerr << "[" << name << "] Fresh IDR frame detected\n";
                
                // Re-extract timestamp bases from current stream position
                if (!reader->extractTimestampBases()) {
                    std::cerr << "[" << name << "] Warning: Could not extract timestamp bases\n";
                }
                std::cerr << "[" << name << "] Updated bases: PTS=" << reader->getPTSBase()
                          << ", PCR=" << reader->getPCRBase() << "\n";
            }
            
            TCPReader* reader = readers[streamIdx].get();
            std::string name = "TCP" + std::to_string(streamIdx + 1);
            const StreamInfo& streamInfo = streamInfos[streamIdx];
            
            std::cerr << "[ACTIVE: " << name << "] Splicing from " << reader->getURL()
                      << " (PTS offset=" << globalPTSOffset << ", PCR offset=" << globalPCROffset << ")\n";
            
            // Get initial buffered packets
            auto buffered = reader->getBufferedPacketsFromIDR();
            std::cerr << "[" << name << "] Processing " << buffered.size() << " buffered packets from IDR\n";
            
            // DIAGNOSTIC: Check if first audio packets are PES continuations (missing headers)
            bool foundFirstAudioPUSI = false;
            size_t audioPacketsBeforeFirstPUSI = 0;
            size_t firstAudioPUSIIndex = 0;
            for (size_t i = 0; i < buffered.size(); i++) {
                const auto& pkt = buffered[i];
                if (pkt.getPID() == streamInfo.audioPID) {
                    if (!foundFirstAudioPUSI) {
                        if (pkt.getPUSI()) {
                            foundFirstAudioPUSI = true;
                            firstAudioPUSIIndex = i;
                            std::cerr << "[AUDIO-DIAG] First audio PUSI found at buffered index " << i
                                      << " (after " << audioPacketsBeforeFirstPUSI << " continuation packets)\n";
                        } else {
                            audioPacketsBeforeFirstPUSI++;
                        }
                    }
                }
            }
            if (!foundFirstAudioPUSI) {
                std::cerr << "[AUDIO-DIAG] WARNING: No audio PUSI found in buffered packets! "
                          << "All " << audioPacketsBeforeFirstPUSI << " audio packets are continuations!\n";
            } else if (audioPacketsBeforeFirstPUSI > 0) {
                std::cerr << "[AUDIO-DIAG] WARNING: " << audioPacketsBeforeFirstPUSI
                          << " audio continuation packets at segment start - these will cause decoder issues!\n";
            }
            
            // DIAGNOSTIC: Show PTS bases and offset for this segment
            std::cerr << "[AUDIO-DIAG] Segment PTS configuration:\n"
                      << "  - PTS base (used for rebasing): " << reader->getPTSBase() << "\n"
                      << "  - Audio PTS base: " << reader->getAudioPTSBase() << "\n"
                      << "  - Global PTS offset: " << globalPTSOffset << "\n"
                      << "  - PCR base: " << reader->getPCRBase() << "\n"
                      << "  - Global PCR offset: " << globalPCROffset << "\n";
            
            // If audio PTS base differs from PTS base, this could cause issues
            if (reader->getAudioPTSBase() != reader->getPTSBase()) {
                int64_t diff = (int64_t)reader->getAudioPTSBase() - (int64_t)reader->getPTSBase();
                std::cerr << "[AUDIO-DIAG] NOTE: Audio PTS base differs from video PTS base by "
                          << diff << " ticks (" << (diff / 90.0) << "ms)\n";
            }
            
            // DIAGNOSTIC: Check first audio PES's raw PTS and what it becomes after rebasing
            if (foundFirstAudioPUSI && firstAudioPUSIIndex < buffered.size()) {
                const auto& firstAudioPkt = buffered[firstAudioPUSIIndex];
                size_t hdr_sz = firstAudioPkt.getHeaderSize();
                const uint8_t* pl = firstAudioPkt.b + hdr_sz;
                size_t pl_sz = ts::PKT_SIZE - hdr_sz;
                
                if (pl_sz >= 14 && pl[0] == 0x00 && pl[1] == 0x00 && pl[2] == 0x01) {
                    uint8_t flags = (pl[7] >> 6) & 0x03;
                    if (flags == 0x02 || flags == 0x03) {
                        uint64_t rawPts = ((uint64_t)(pl[9] & 0x0E) << 29) |
                                          ((uint64_t)(pl[10]) << 22) |
                                          ((uint64_t)(pl[11] & 0xFE) << 14) |
                                          ((uint64_t)(pl[12]) << 7) |
                                          ((uint64_t)(pl[13] >> 1));
                        uint64_t rebasedPts = (rawPts - reader->getPTSBase()) + globalPTSOffset;
                        std::cerr << "[AUDIO-DIAG] First audio PTS: raw=" << rawPts
                                  << ", rebased=" << rebasedPts
                                  << " (should be near globalPTSOffset=" << globalPTSOffset << ")\n";
                        
                        if (rawPts < reader->getPTSBase()) {
                            std::cerr << "[AUDIO-DIAG] CRITICAL: Audio PTS UNDERFLOW! "
                                      << "Raw PTS " << rawPts << " < base " << reader->getPTSBase() << "\n";
                        }
                    }
                }
            }
            
            uint64_t startPTS = globalPTSOffset;
            uint64_t maxPTS = 0;
            uint64_t maxPCR = 0;
            size_t packetCount = 0;
            size_t videoPacketCount = 0;
            size_t audioPacketCount = 0;
            size_t skippedPacketCount = 0;
            
            // DEBUG: Count audio packets in buffered section BEFORE any processing
            size_t audioInBufferedSection = 0;
            size_t audioPUSIInBuffered = 0;
            for (const auto& p : buffered) {
                if (p.getPID() == streamInfo.audioPID) {
                    audioInBufferedSection++;
                    if (p.getPUSI()) audioPUSIInBuffered++;
                }
            }
            std::cerr << "[AUDIO-TRACE] Buffered section: " << audioInBufferedSection
                      << " audio packets, " << audioPUSIInBuffered << " with PUSI\n";
            
            // Process buffered packets
            for (auto& pkt : buffered) {
                ts::PID origPID = pkt.getPID();
                if (origPID == ts::PID_PAT || origPID == streamInfo.pmtPID) {
                    skippedPacketCount++;
                    continue;
                }
                
                bool isVideo = (origPID == streamInfo.videoPID);
                bool isAudio = (origPID == streamInfo.audioPID);
                
                // DEBUG: Check for PTS underflow before rebasing
                if (isAudio && pkt.getPUSI() && pkt.hasPayload()) {
                    size_t hdr = pkt.getHeaderSize();
                    const uint8_t* pl = pkt.b + hdr;
                    size_t pl_size = ts::PKT_SIZE - hdr;
                    if (pl_size >= 14 && pl[0] == 0x00 && pl[1] == 0x00 && pl[2] == 0x01) {
                        uint8_t flags = (pl[7] >> 6) & 0x03;
                        if (flags == 0x02 || flags == 0x03) {
                            uint64_t origPts = ((uint64_t)(pl[9] & 0x0E) << 29) |
                                               ((uint64_t)(pl[10]) << 22) |
                                               ((uint64_t)(pl[11] & 0xFE) << 14) |
                                               ((uint64_t)(pl[12]) << 7) |
                                               ((uint64_t)(pl[13] >> 1));
                            if (origPts < reader->getPTSBase()) {
                                std::cerr << "[AUDIO-TRACE] WARNING: Audio PTS " << origPts
                                          << " is BEFORE video PTS base " << reader->getPTSBase()
                                          << " - will underflow!\n";
                            }
                        }
                    }
                }
                
                // DEBUG: Track non-essential packets (not video, not audio)
                if (!isVideo && !isAudio) {
                    nonEssentialPacketsOutput++;
                    unexpectedPidCounts[origPID]++;
                    // Skip non-essential packets - they may cause decoder issues
                    skippedPacketCount++;
                    continue;
                }
                
                rebasePacket(pkt, streamInfo, globalPTSOffset, globalPCROffset,
                           reader->getPTSBase(), reader->getPCRBase());
                
                // DEBUG: Track audio packet types
                if (isAudio) {
                    totalAudioPacketsOutput++;
                    currentPESAudioPackets++;
                    
                    if (pkt.hasPayload()) {
                        audioPacketsWithPayload++;
                    } else {
                        audioPacketsAdaptationOnly++;
                        std::cerr << "[AUDIO-DEBUG] Adaptation-only audio packet detected (no payload), CC="
                                  << (int)pkt.getCC() << "\n";
                    }
                    
                    // Track PES boundaries
                    if (pkt.getPUSI()) {
                        // New PES starting - check if previous was complete
                        if (expectedAudioPacketsPerPES > 0 && currentPESAudioPackets > 1) {
                            if (currentPESAudioPackets - 1 != expectedAudioPacketsPerPES) {
                                incompleteAudioPES++;
                                std::cerr << "[AUDIO-PES-TRACK] Incomplete PES: expected "
                                          << expectedAudioPacketsPerPES << " packets, got "
                                          << (currentPESAudioPackets - 1) << "\n";
                            }
                        }
                        // Set expectation based on first complete PES
                        if (expectedAudioPacketsPerPES == 0 && currentPESAudioPackets > 1) {
                            expectedAudioPacketsPerPES = currentPESAudioPackets - 1;
                            std::cerr << "[AUDIO-PES-TRACK] First PES had " << expectedAudioPacketsPerPES
                                      << " packets - using as baseline\n";
                        }
                        
                        // Log gap between PUSI packets (should be consistent)
                        if (lastAudioPUSIOutputIndex > 0) {
                            size_t packetGap = totalOutputIndex - lastAudioPUSIOutputIndex;
                            if (audioPUSICount <= 5 || audioPUSICount % 10 == 0) {
                                std::cerr << "[AUDIO-PES-TRACK] PUSI #" << audioPUSICount
                                          << " at output index " << totalOutputIndex
                                          << " (gap of " << packetGap << " total packets since last PUSI)\n";
                            }
                        }
                        lastAudioPUSIOutputIndex = totalOutputIndex;
                        currentPESAudioPackets = 1;  // Reset for new PES, counting this packet
                    }
                    
                    // Detailed PUSI analysis for audio
                    if (pkt.getPUSI()) {
                        audioPUSICount++;
                        size_t hdr_size = pkt.getHeaderSize();
                        const uint8_t* payload = pkt.b + hdr_size;
                        size_t payload_size = ts::PKT_SIZE - hdr_size;
                        
                        if (payload_size < 14) {
                            audioPUSITooSmall++;
                            if (audioPUSICount <= 5) {
                                std::cerr << "[AUDIO-PUSI] #" << audioPUSICount << ": payload too small ("
                                          << payload_size << " bytes)\n";
                            }
                        } else if (payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01) {
                            audioPUSIBadPESHeader++;
                            if (audioPUSICount <= 5) {
                                std::cerr << "[AUDIO-PUSI] #" << audioPUSICount << ": bad PES header: "
                                          << std::hex << (int)payload[0] << " " << (int)payload[1] << " "
                                          << (int)payload[2] << " " << (int)payload[3] << std::dec << "\n";
                            }
                        } else {
                            audioPUSIWithValidPES++;
                            uint8_t pts_dts_flags = (payload[7] >> 6) & 0x03;
                            if (pts_dts_flags == 0x02 || pts_dts_flags == 0x03) {
                                audioPUSIWithPTS++;
                                
                                // Count ADTS frames in this PES
                                size_t adtsCount = countADTSFrames(payload, payload_size);
                                totalADTSFramesDetected += adtsCount;
                                totalAudioPESBytes += payload_size;
                                
                                // Log first few PES details
                                if (audioPUSIWithPTS <= 5) {
                                    std::cerr << "[AUDIO-PES] #" << audioPUSIWithPTS
                                              << ": payload=" << payload_size << " bytes"
                                              << ", ADTS frames in first TS packet=" << adtsCount << "\n";
                                }
                            } else {
                                audioPUSINoPTS++;
                                if (audioPUSINoPTS <= 5) {
                                    std::cerr << "[AUDIO-PUSI] #" << audioPUSICount
                                              << ": valid PES but no PTS (pts_dts_flags=" << (int)pts_dts_flags
                                              << ", stream_id=0x" << std::hex << (int)payload[3] << std::dec << ")\n";
                                }
                            }
                        }
                    }
                }
                
                if (pkt.hasPayload()) {
                    pkt.setCC(ccManager.getNext(pkt.getPID()));
                }
                
                // DEBUG: Check for CC discontinuities
                ts::PID finalPID = pkt.getPID();
                if (lastCCValues.find(finalPID) != lastCCValues.end()) {
                    uint8_t expectedCC = (lastCCValues[finalPID] + 1) & 0x0F;
                    if (pkt.hasPayload() && pkt.getCC() != expectedCC) {
                        // This shouldn't happen since we're generating CC sequentially
                        // But let's log if it does
                        if (finalPID == NORM_AUDIO_PID) {
                            audioCCDiscontinuities++;
                            std::cerr << "[AUDIO-DEBUG] CC discontinuity: expected=" << (int)expectedCC
                                      << ", got=" << (int)pkt.getCC() << "\n";
                        } else if (finalPID == NORM_VIDEO_PID) {
                            videoCCDiscontinuities++;
                        }
                    }
                }
                if (pkt.hasPayload()) {
                    lastCCValues[finalPID] = pkt.getCC();
                }
                
                if (isVideo) videoPacketCount++;
                else if (isAudio) audioPacketCount++;
                
                if (pkt.hasPCR()) {
                    maxPCR = std::max(maxPCR, pkt.getPCR());
                }
                
                if (pkt.getPUSI() && pkt.hasPayload()) {
                    size_t header_size = pkt.getHeaderSize();
                    const uint8_t* payload = pkt.b + header_size;
                    size_t payload_size = ts::PKT_SIZE - header_size;
                    
                    uint64_t pts;
                    if (payload_size >= 14 && extractPTS(payload, payload_size, pts)) {
                        if (pts <= 0x0FFFFFFFF) {
                            maxPTS = std::max(maxPTS, pts);
                            
                            // DEBUG: Track audio PTS timing
                            if (isAudio) {
                                audioPTSCount++;
                                if (firstAudioPTS == 0) {
                                    firstAudioPTS = pts;
                                    std::cerr << "[AUDIO-PTS] First audio PTS: " << pts
                                              << " (rebased), original was ~" << (pts + reader->getPTSBase()) << "\n";
                                }
                                
                                // Check for gaps in audio PTS
                                if (lastAudioPTS > 0 && pts > lastAudioPTS) {
                                    uint64_t gap = pts - lastAudioPTS;
                                    if (gap > AUDIO_GAP_THRESHOLD) {
                                        audioPTSGaps++;
                                        std::cerr << "[AUDIO-PTS] GAP DETECTED: " << gap << " ticks ("
                                                  << (gap / 90.0) << "ms) at audio frame #" << audioPTSCount << "\n";
                                    }
                                }
                                lastAudioPTS = pts;
                                
                                // Log first 5 audio PTS values
                                if (audioPTSCount <= 5) {
                                    std::cerr << "[AUDIO-PTS] Frame #" << audioPTSCount << ": PTS=" << pts << "\n";
                                }
                            } else if (isVideo && firstVideoPTS == 0) {
                                firstVideoPTS = pts;
                                std::cerr << "[VIDEO-PTS] First video PTS: " << pts << "\n";
                            }
                        }
                    }
                }
                
                std::cout.write(reinterpret_cast<char*>(pkt.b), ts::PKT_SIZE);
                packetCount++;
                totalOutputIndex++;
                
                if (maxPTS > 0 && (maxPTS - startPTS) >= targetDurationPTS) {
                    break;
                }
            }
            
            // Use snapshot end index to prevent race condition packet loss
            reader->initConsumptionFromIndex(reader->getLastSnapshotEnd());
            
            // DEBUG: Track audio output from buffered section
            std::cerr << "[AUDIO-TRACE] After buffered: output " << audioPacketCount << " audio packets\n";
            
            size_t audioFromLive = 0;
            
            // Continue with live packets if needed
            while (maxPTS == 0 || (maxPTS - startPTS) < targetDurationPTS) {
                auto packets = reader->receivePackets(100, 1000);
                if (packets.empty()) {
                    continue;
                }
                
                for (auto& pkt : packets) {
                    ts::PID origPID = pkt.getPID();
                    if (origPID == ts::PID_PAT || origPID == streamInfo.pmtPID) {
                        skippedPacketCount++;
                        continue;
                    }
                    
                    bool isVideo = (origPID == streamInfo.videoPID);
                    bool isAudio = (origPID == streamInfo.audioPID);
                    
                    // DEBUG: Track non-essential packets (not video, not audio)
                    if (!isVideo && !isAudio) {
                        nonEssentialPacketsOutput++;
                        unexpectedPidCounts[origPID]++;
                        // Skip non-essential packets - they may cause decoder issues
                        skippedPacketCount++;
                        continue;
                    }
                    
                    rebasePacket(pkt, streamInfo, globalPTSOffset, globalPCROffset,
                               reader->getPTSBase(), reader->getPCRBase());
                    
                    // DEBUG: Track audio packet types
                    if (isAudio) {
                        totalAudioPacketsOutput++;
                        currentPESAudioPackets++;
                        
                        if (pkt.hasPayload()) {
                            audioPacketsWithPayload++;
                        } else {
                            audioPacketsAdaptationOnly++;
                            std::cerr << "[AUDIO-DEBUG] Adaptation-only audio packet detected (no payload), CC="
                                      << (int)pkt.getCC() << "\n";
                        }
                        
                        // Track PES boundaries (live)
                        if (pkt.getPUSI()) {
                            // Check previous PES completeness
                            if (expectedAudioPacketsPerPES > 0 && currentPESAudioPackets > 1) {
                                if (currentPESAudioPackets - 1 != expectedAudioPacketsPerPES) {
                                    incompleteAudioPES++;
                                    std::cerr << "[AUDIO-PES-TRACK] Incomplete PES in live: expected "
                                              << expectedAudioPacketsPerPES << " packets, got "
                                              << (currentPESAudioPackets - 1) << "\n";
                                }
                            }
                            if (expectedAudioPacketsPerPES == 0 && currentPESAudioPackets > 1) {
                                expectedAudioPacketsPerPES = currentPESAudioPackets - 1;
                                std::cerr << "[AUDIO-PES-TRACK] First PES had " << expectedAudioPacketsPerPES
                                          << " packets - using as baseline\n";
                            }
                            
                            // Log gap between PUSI packets
                            if (lastAudioPUSIOutputIndex > 0) {
                                size_t packetGap = totalOutputIndex - lastAudioPUSIOutputIndex;
                                if (audioPUSICount <= 5 || audioPUSICount % 10 == 0) {
                                    std::cerr << "[AUDIO-PES-TRACK] PUSI #" << audioPUSICount
                                              << " at output index " << totalOutputIndex
                                              << " (gap of " << packetGap << " total packets since last PUSI)\n";
                                }
                            }
                            lastAudioPUSIOutputIndex = totalOutputIndex;
                            currentPESAudioPackets = 1;
                        }
                        
                        // Detailed PUSI analysis for audio (live packets)
                        if (pkt.getPUSI()) {
                            audioFromLive++;  // Track audio PUSI from live section
                            audioPUSICount++;
                            size_t hdr_size = pkt.getHeaderSize();
                            const uint8_t* payload = pkt.b + hdr_size;
                            size_t payload_size = ts::PKT_SIZE - hdr_size;
                            
                            if (payload_size < 14) {
                                audioPUSITooSmall++;
                            } else if (payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01) {
                                audioPUSIBadPESHeader++;
                            } else {
                                audioPUSIWithValidPES++;
                                uint8_t pts_dts_flags = (payload[7] >> 6) & 0x03;
                                if (pts_dts_flags == 0x02 || pts_dts_flags == 0x03) {
                                    audioPUSIWithPTS++;
                                } else {
                                    audioPUSINoPTS++;
                                }
                            }
                        }
                    }
                    
                    if (pkt.hasPayload()) {
                        pkt.setCC(ccManager.getNext(pkt.getPID()));
                    }
                    
                    // DEBUG: Check for CC discontinuities
                    ts::PID finalPID = pkt.getPID();
                    if (lastCCValues.find(finalPID) != lastCCValues.end()) {
                        uint8_t expectedCC = (lastCCValues[finalPID] + 1) & 0x0F;
                        if (pkt.hasPayload() && pkt.getCC() != expectedCC) {
                            if (finalPID == NORM_AUDIO_PID) {
                                audioCCDiscontinuities++;
                                std::cerr << "[AUDIO-DEBUG] CC discontinuity: expected=" << (int)expectedCC
                                          << ", got=" << (int)pkt.getCC() << "\n";
                            } else if (finalPID == NORM_VIDEO_PID) {
                                videoCCDiscontinuities++;
                            }
                        }
                    }
                    if (pkt.hasPayload()) {
                        lastCCValues[finalPID] = pkt.getCC();
                    }
                    
                    if (isVideo) videoPacketCount++;
                    else if (isAudio) audioPacketCount++;
                    
                    if (pkt.hasPCR()) {
                        maxPCR = std::max(maxPCR, pkt.getPCR());
                    }
                    
                    if (pkt.getPUSI() && pkt.hasPayload()) {
                        size_t header_size = pkt.getHeaderSize();
                        const uint8_t* payload = pkt.b + header_size;
                        size_t payload_size = ts::PKT_SIZE - header_size;
                        
                        uint64_t pts;
                        if (payload_size >= 14 && extractPTS(payload, payload_size, pts)) {
                            if (pts <= 0x0FFFFFFFF) {
                                maxPTS = std::max(maxPTS, pts);
                                
                                // DEBUG: Track audio PTS timing (live packets)
                                if (isAudio) {
                                    audioPTSCount++;
                                    if (firstAudioPTS == 0) {
                                        firstAudioPTS = pts;
                                        std::cerr << "[AUDIO-PTS] First audio PTS: " << pts
                                                  << " (rebased), original was ~" << (pts + reader->getPTSBase()) << "\n";
                                    }
                                    
                                    // Check for gaps in audio PTS
                                    if (lastAudioPTS > 0 && pts > lastAudioPTS) {
                                        uint64_t gap = pts - lastAudioPTS;
                                        if (gap > AUDIO_GAP_THRESHOLD) {
                                            audioPTSGaps++;
                                            std::cerr << "[AUDIO-PTS] GAP DETECTED: " << gap << " ticks ("
                                                      << (gap / 90.0) << "ms) at audio frame #" << audioPTSCount << "\n";
                                        }
                                    }
                                    lastAudioPTS = pts;
                                }
                            }
                        }
                    }
                    
                    std::cout.write(reinterpret_cast<char*>(pkt.b), ts::PKT_SIZE);
                    packetCount++;
                    totalOutputIndex++;
                    
                    if (maxPTS > 0 && (maxPTS - startPTS) >= targetDurationPTS) {
                        break;
                    }
                }
                
                if (maxPTS > 0 && (maxPTS - startPTS) >= targetDurationPTS) {
                    break;
                }
            }
            
            double actualDuration = (maxPTS - startPTS) / 90000.0;
            std::cerr << "[" << name << "] Processed " << packetCount << " packets ("
                      << std::fixed << std::setprecision(2) << actualDuration << "s)\n";
            std::cerr << "[" << name << "] Packet breakdown: video=" << videoPacketCount
                      << ", audio=" << audioPacketCount << ", skipped=" << skippedPacketCount << "\n";
            std::cerr << "[AUDIO-TRACE] Audio from live section: " << audioFromLive << " PUSI packets\n";
            std::cerr << "[AUDIO-TRACE] Total audio PUSI output: " << audioPUSICount << "\n";
            
            bool isLastSegment = (streamIdx == readers.size() - 1) && (loop == loopCount - 1);
            if (!isLastSegment) {
                globalPTSOffset = maxPTS > 0 ? maxPTS : globalPTSOffset + targetDurationPTS;
                globalPCROffset = maxPCR > 0 ? maxPCR : globalPCROffset + (targetDurationPTS * 300);
            }
            
            std::cerr << "\n";
        }
    }
    
    std::cerr << "=== Splicing complete ===\n";
    std::cerr << "Final PTS offset: " << globalPTSOffset << " (~"
              << (globalPTSOffset / 90000.0) << "s)\n";
    std::cerr << "Final PCR offset: " << globalPCROffset << " (~"
              << (globalPCROffset / 27000000.0) << "s)\n";
    
    // DEBUG: Audio analysis summary
    std::cerr << "\n=== Audio Debug Summary ===\n";
    std::cerr << "Total audio packets output: " << totalAudioPacketsOutput << "\n";
    std::cerr << "  - With payload: " << audioPacketsWithPayload << "\n";
    std::cerr << "  - Adaptation-only (no payload): " << audioPacketsAdaptationOnly << "\n";
    std::cerr << "Audio CC discontinuities detected: " << audioCCDiscontinuities << "\n";
    std::cerr << "Video CC discontinuities detected: " << videoCCDiscontinuities << "\n";
    std::cerr << "Non-essential packets filtered: " << nonEssentialPacketsOutput << "\n";
    if (!unexpectedPidCounts.empty()) {
        std::cerr << "Unexpected PIDs encountered:\n";
        for (const auto& pair : unexpectedPidCounts) {
            std::cerr << "  PID " << pair.first << " (0x" << std::hex << pair.first << std::dec
                      << "): " << pair.second << " packets\n";
        }
    }
    
    // Audio PUSI detailed breakdown
    std::cerr << "\n=== Audio PUSI Analysis ===\n";
    std::cerr << "Total audio packets: " << totalAudioPacketsOutput << "\n";
    std::cerr << "Audio packets with PUSI: " << audioPUSICount << "\n";
    std::cerr << "  - Valid PES header: " << audioPUSIWithValidPES << "\n";
    std::cerr << "    - With PTS: " << audioPUSIWithPTS << "\n";
    std::cerr << "    - Without PTS: " << audioPUSINoPTS << "\n";
    std::cerr << "  - Bad PES header (not 00 00 01): " << audioPUSIBadPESHeader << "\n";
    std::cerr << "  - Payload too small (<14 bytes): " << audioPUSITooSmall << "\n";
    
    // ADTS frame analysis
    std::cerr << "\n=== Audio PES Completeness ===\n";
    std::cerr << "Expected packets per PES: " << expectedAudioPacketsPerPES << "\n";
    std::cerr << "Incomplete PES detected: " << incompleteAudioPES << "\n";
    if (incompleteAudioPES > 0) {
        std::cerr << "  WARNING: Incomplete PES packets cause audio gaps!\n";
    }
    
    std::cerr << "\n=== Audio Content Analysis ===\n";
    std::cerr << "Total audio PES payload bytes (first TS packet only): " << totalAudioPESBytes << "\n";
    std::cerr << "ADTS frames detected in PUSI packets: " << totalADTSFramesDetected << "\n";
    if (audioPUSIWithPTS > 0) {
        std::cerr << "Average ADTS frames per PES (first TS only): "
                  << (double)totalADTSFramesDetected / audioPUSIWithPTS << "\n";
    }
    std::cerr << "Expected AAC frames for " << duration << "s @ 48kHz: ~" << (duration * 46.875) << "\n";
    if (totalADTSFramesDetected > 0) {
        std::cerr << "ADTS frame coverage: " << (100.0 * totalADTSFramesDetected / (duration * 46.875)) << "%\n";
    }
    
    // Audio PTS timing analysis
    std::cerr << "\n=== Audio PTS Timing Analysis ===\n";
    std::cerr << "First video PTS (rebased): " << firstVideoPTS << "\n";
    std::cerr << "First audio PTS (rebased): " << firstAudioPTS << "\n";
    if (firstVideoPTS > 0 && firstAudioPTS > 0) {
        int64_t avDiff = (int64_t)firstAudioPTS - (int64_t)firstVideoPTS;
        std::cerr << "Audio-Video PTS offset: " << avDiff << " ticks (" << (avDiff / 90.0) << "ms)\n";
        if (avDiff < -9000 || avDiff > 9000) {
            std::cerr << "  WARNING: A/V offset > 100ms! This could cause sync issues.\n";
        }
    }
    std::cerr << "Total audio PTS frames: " << audioPTSCount << "\n";
    std::cerr << "Audio PTS gaps detected (>" << (AUDIO_GAP_THRESHOLD / 90.0) << "ms): " << audioPTSGaps << "\n";
    if (lastAudioPTS > firstAudioPTS && audioPTSCount > 1) {
        double avgFrameDuration = (double)(lastAudioPTS - firstAudioPTS) / (audioPTSCount - 1);
        std::cerr << "Average audio frame duration: " << (avgFrameDuration / 90.0) << "ms\n";
        std::cerr << "Expected AAC frame duration: " << (EXPECTED_AUDIO_FRAME_PTS / 90.0) << "ms\n";
    }
    
    return 0;
}