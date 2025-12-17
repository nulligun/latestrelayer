#include "FIFOInput.h"
#include "TSStreamReassembler.h"
#include "NALParser.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

// Helper function to extract PTS from PES header
static bool extractPTS(const uint8_t* pes, size_t size, uint64_t& pts) {
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

// Helper function to extract PCR from adaptation field
static bool extractPCR(const ts::TSPacket& pkt, uint64_t& pcr) {
    if (!pkt.hasPCR()) return false;
    pcr = pkt.getPCR();
    return true;
}

// Helper function to find IDR in PES payload
static bool findIDRInPES(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size - 4; i++) {
        if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x01) {
            uint8_t nal_type = data[i+3] & 0x1F;
            if (nal_type == 5) {  // IDR NAL unit
                return true;
            }
        }
    }
    return false;
}

FIFOInput::FIFOInput(const std::string& name, const std::string& pipe_path)
    : name_(name),
      pipe_path_(pipe_path),
      fd_(-1),
      stop_thread_(false),
      running_(false),
      connected_(false),
      pids_ready_(false),
      idr_ready_(false),
      audio_ready_(false),
      audio_sync_ready_(false),
      first_packet_received_(false),
      idr_index_(0),
      latest_idr_index_(0),
      audio_sync_index_(0),
      consume_index_(0),
      last_snapshot_end_(0),
      max_buffer_packets_(MAX_BUFFER_PACKETS),
      pts_base_(0),
      audio_pts_base_(0),
      pcr_base_(0),
      pcr_pts_alignment_offset_(0),
      total_packets_received_(0),
      last_progress_report_(std::chrono::steady_clock::now()) {
}

FIFOInput::~FIFOInput() {
    stop();
}

bool FIFOInput::start() {
    if (running_.load()) {
        std::cerr << "[" << name_ << "] Already running" << std::endl;
        return false;
    }
    
    stop_thread_ = false;
    bg_thread_ = std::thread(&FIFOInput::backgroundThreadFunc, this);
    
    std::cout << "[" << name_ << "] Started FIFO reader for " << pipe_path_ << std::endl;
    return true;
}

void FIFOInput::stop() {
    if (!running_.load() && !bg_thread_.joinable()) {
        return;
    }
    
    std::cout << "[" << name_ << "] Stopping..." << std::endl;
    stop_thread_ = true;
    
    closePipe();
    
    if (bg_thread_.joinable()) {
        bg_thread_.join();
    }
    
    std::cout << "[" << name_ << "] Stopped. Total packets: " << total_packets_received_.load() << std::endl;
}

bool FIFOInput::openPipe() {
    if (fd_ >= 0) {
        std::cout << "[" << name_ << "] Pipe already open" << std::endl;
        return true;
    }
    
    std::cout << "[" << name_ << "] Opening named pipe: " << pipe_path_ << std::endl;
    
    // Verify pipe exists
    struct stat st;
    if (stat(pipe_path_.c_str(), &st) != 0) {
        std::cerr << "[" << name_ << "] Pipe does not exist: " << pipe_path_ 
                  << " (" << strerror(errno) << ")" << std::endl;
        return false;
    }
    
    if (!S_ISFIFO(st.st_mode)) {
        std::cerr << "[" << name_ << "] Path exists but is not a FIFO: " << pipe_path_ << std::endl;
        return false;
    }
    
    // Open the FIFO for reading (blocks until a writer opens it)
    std::cout << "[" << name_ << "] Waiting for FFmpeg to open pipe for writing..." << std::endl;
    fd_ = ::open(pipe_path_.c_str(), O_RDONLY);
    
    if (fd_ < 0) {
        std::cerr << "[" << name_ << "] Failed to open pipe: " << strerror(errno) << std::endl;
        return false;
    }
    
    std::cout << "[" << name_ << "] Pipe opened successfully (fd=" << fd_ << ")" << std::endl;
    
    // Try to increase pipe buffer size for better performance
#ifdef F_SETPIPE_SZ
    int result = fcntl(fd_, F_SETPIPE_SZ, PIPE_BUFFER_SIZE);
    if (result < 0) {
        std::cerr << "[" << name_ << "] Warning: Failed to set pipe buffer size: " 
                  << strerror(errno) << std::endl;
        std::cout << "[" << name_ << "] Using default system pipe buffer" << std::endl;
    } else {
        std::cout << "[" << name_ << "] Pipe buffer size set to " << result << " bytes" << std::endl;
    }
#else
    std::cout << "[" << name_ << "] F_SETPIPE_SZ not available, using default pipe buffer" << std::endl;
#endif
    
    connected_ = true;
    return true;
}

void FIFOInput::closePipe() {
    if (fd_ >= 0) {
        std::cout << "[" << name_ << "] Closing pipe..." << std::endl;
        ::close(fd_);
        fd_ = -1;
    }
    connected_ = false;
}

void FIFOInput::backgroundThreadFunc() {
    std::cout << "[" << name_ << "] Background thread started" << std::endl;
    running_ = true;
    
    while (!stop_thread_.load()) {
        // Attempt to open pipe with retry
        while (!connected_.load() && !stop_thread_.load()) {
            if (openPipe()) {
                break;
            }
            std::cout << "[" << name_ << "] Retrying pipe open in " << (PIPE_RECONNECT_DELAY_MS / 1000.0) << "s..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(PIPE_RECONNECT_DELAY_MS));
        }
        
        if (stop_thread_.load()) break;
        
        // Reset state for new connection
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            rolling_buffer_.clear();
            idr_index_ = 0;
            latest_idr_index_ = 0;
            audio_sync_index_ = 0;
            consume_index_ = 0;
        }
        pids_ready_ = false;
        idr_ready_ = false;
        audio_ready_ = false;
        audio_sync_ready_ = false;
        first_packet_received_ = false;
        
        // Reset health metrics for new connection
        health_metrics_.reset();
        
        // Process FIFO stream
        processFIFOStream();
        
        // Connection lost
        std::cout << "[" << name_ << "] FIFO connection closed" << std::endl;
        connected_ = false;
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }
    
    running_ = false;
    std::cout << "[" << name_ << "] Background thread stopped" << std::endl;
}

void FIFOInput::processFIFOStream() {
    ts::DuckContext duck;
    ts::SectionDemux demux(duck);
    std::vector<uint8_t> pes_buffer;
    bool foundPAT = false;
    bool foundPMT = false;
    last_progress_report_ = std::chrono::steady_clock::now();
    connection_start_time_ = std::chrono::steady_clock::now();
    size_t total_packets_in_connection = 0;
    size_t pes_start_index = 0;
    
    // PAT/PMT handler (same as TCPReader)
    class StreamAnalyzer : public ts::TableHandlerInterface {
    public:
        StreamInfo& info;
        bool& foundPAT;
        bool& foundPMT;
        ts::DuckContext& duck;
        
        StreamAnalyzer(StreamInfo& si, bool& pat, bool& pmt, ts::DuckContext& d)
            : info(si), foundPAT(pat), foundPMT(pmt), duck(d) {}
        
        virtual void handleTable(ts::SectionDemux&, const ts::BinaryTable& table) override {
            if (table.tableId() == ts::TID_PAT && !foundPAT) {
                ts::PAT pat(duck, table);
                if (pat.isValid() && !pat.pmts.empty()) {
                    auto it = pat.pmts.begin();
                    info.program_number = it->first;
                    info.pmt_pid = it->second;
                    foundPAT = true;
                }
            }
            else if (table.tableId() == ts::TID_PMT && !foundPMT) {
                ts::PMT pmt(duck, table);
                if (pmt.isValid()) {
                    info.pcr_pid = pmt.pcr_pid;
                    for (const auto& stream : pmt.streams) {
                        if (stream.second.stream_type == 0x1B || stream.second.stream_type == 0x24) {
                            info.video_pid = stream.first;
                            info.video_stream_type = stream.second.stream_type;
                        } else if (stream.second.stream_type == 0x0F ||
                                 stream.second.stream_type == 0x03 ||
                                 stream.second.stream_type == 0x81) {
                            info.audio_pid = stream.first;
                            info.audio_stream_type = stream.second.stream_type;
                        }
                    }
                    foundPMT = true;
                }
            }
        }
    };
    
    StreamAnalyzer analyzer(discovered_info_, foundPAT, foundPMT, duck);
    demux.setTableHandler(&analyzer);
    demux.addPID(ts::PID_PAT);
    
    // TSStreamReassembler handles TS packet boundaries in byte stream
    TSStreamReassembler reassembler;
    
    // FIFO read buffer
    constexpr size_t FIFO_BUFFER_SIZE = 64 * 1024;  // 64 KB
    uint8_t fifo_buffer[FIFO_BUFFER_SIZE];
    
    std::cout << "[" << name_ << "] Starting FIFO read loop" << std::endl;
    
    while (!stop_thread_.load() && connected_.load()) {
        // Blocking read from pipe
        ssize_t n = read(fd_, fifo_buffer, sizeof(fifo_buffer));
        
        if (n < 0) {
            std::cerr << "[" << name_ << "] FIFO read error: " << strerror(errno) << std::endl;
            break;
        }
        
        if (n == 0) {
            // EOF - writer closed the pipe
            std::cout << "[" << name_ << "] FIFO EOF (writer disconnected)" << std::endl;
            break;
        }
        
        // Record data received for health monitoring
        health_metrics_.recordDataReceived(n);
        
        // Feed data to reassembler
        reassembler.addData(fifo_buffer, n);
        
        // Get reassembled TS packets
        auto packets = reassembler.getPackets();
        
        for (auto& pkt : packets) {
            total_packets_in_connection++;
            
            if (!first_packet_received_.load()) {
                first_packet_received_ = true;
                std::cout << "[" << name_ << "] Receiving FIFO data..." << std::endl;
            }
            
            // Periodic progress reporting
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_progress_report_).count();
            if (elapsed >= 5) {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                std::string status;
                if (!pids_ready_.load()) {
                    if (!foundPAT) status = "searching for PAT/PMT...";
                    else if (!foundPMT) status = "PAT found, searching for PMT...";
                    else status = "PMT found, waiting for signal...";
                } else if (!idr_ready_.load()) {
                    status = "waiting for IDR frame...";
                } else {
                    status = "ready";
                }
                std::cout << "[" << name_ << "] Progress: " << rolling_buffer_.size()
                          << " packets buffered, " << status << std::endl;
                last_progress_report_ = now;
            }
            
            // Phase 1: Parse PAT/PMT (same logic as TCPReader)
            if (!pids_ready_.load()) {
                if (foundPAT && !demux.hasPID(discovered_info_.pmt_pid)) {
                    demux.addPID(discovered_info_.pmt_pid);
                }
                
                demux.feedPacket(pkt);
                
                if (foundPAT && foundPMT) {
                    discovered_info_.initialized = true;
                    std::cout << "[" << name_ << "] PAT/PMT discovery complete!" << std::endl;
                    std::cout << "[" << name_ << "] Video PID=" << discovered_info_.video_pid
                              << ", Audio PID=" << discovered_info_.audio_pid
                              << ", PCR PID=" << discovered_info_.pcr_pid << std::endl;
                    pids_ready_ = true;
                    cv_.notify_all();
                }
            }
            
            // Phase 2: Detect IDR (same logic as TCPReader)
            if (pids_ready_.load()) {
                if (pkt.getPID() == discovered_info_.video_pid && pkt.hasPayload()) {
                    if (pkt.getPUSI()) {
                        if (!pes_buffer.empty() && findIDRInPES(pes_buffer.data(), pes_buffer.size())) {
                            std::lock_guard<std::mutex> lock(buffer_mutex_);
                            
                            latest_idr_index_ = pes_start_index;
                            
                            if (!idr_ready_.load()) {
                                idr_index_ = pes_start_index;
                                std::cout << "[" << name_ << "] Initial IDR frame detected at index " << idr_index_ << std::endl;
                                
                                if (discovered_info_.audio_pid == ts::PID_NULL) {
                                    std::cout << "[" << name_ << "] No audio stream, marking ready" << std::endl;
                                    idr_ready_ = true;
                                    cv_.notify_all();
                                } else {
                                    std::cout << "[" << name_ << "] Waiting for first audio PES..." << std::endl;
                                }
                            }
                        }
                        pes_buffer.clear();
                        pes_start_index = rolling_buffer_.size();
                    }
                    
                    size_t header_size = pkt.getHeaderSize();
                    const uint8_t* payload = pkt.b + header_size;
                    size_t payload_size = ts::PKT_SIZE - header_size;
                    
                    if (payload_size > 0) {
                        pes_buffer.insert(pes_buffer.end(), payload, payload + payload_size);
                    }
                }
            }
            
            // Phase 3: Wait for first audio PES after IDR (same logic as TCPReader)
            if (pids_ready_.load() && idr_index_ > 0 && !idr_ready_.load() &&
                discovered_info_.audio_pid != ts::PID_NULL && !audio_ready_.load()) {
                if (pkt.getPID() == discovered_info_.audio_pid && pkt.getPUSI() && pkt.hasPayload()) {
                    std::lock_guard<std::mutex> lock(buffer_mutex_);
                    audio_sync_index_ = rolling_buffer_.size();
                    std::cout << "[" << name_ << "] First audio PES at index " << audio_sync_index_ << std::endl;
                    audio_ready_ = true;
                    audio_sync_ready_ = true;
                    idr_ready_ = true;
                    cv_.notify_all();
                }
            }
            
            // Phase 3b: Continue tracking audio sync
            if (pids_ready_.load() && idr_ready_.load() && !audio_sync_ready_.load() &&
                discovered_info_.audio_pid != ts::PID_NULL) {
                if (pkt.getPID() == discovered_info_.audio_pid && pkt.getPUSI() && pkt.hasPayload()) {
                    std::lock_guard<std::mutex> lock(buffer_mutex_);
                    audio_sync_index_ = rolling_buffer_.size();
                    audio_sync_ready_ = true;
                    std::cout << "[" << name_ << "] Audio sync updated at index " << audio_sync_index_ << std::endl;
                    cv_.notify_all();
                }
            }
            
            // Always buffer
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                rolling_buffer_.push_back(pkt);
                
                // Trim buffer if too large
                if (rolling_buffer_.size() > max_buffer_packets_ && idr_ready_.load()) {
                    size_t to_remove = rolling_buffer_.size() - max_buffer_packets_;
                    rolling_buffer_.erase(rolling_buffer_.begin(), rolling_buffer_.begin() + to_remove);
                    if (idr_index_ >= to_remove) idr_index_ -= to_remove;
                    else idr_index_ = 0;
                    if (latest_idr_index_ >= to_remove) latest_idr_index_ -= to_remove;
                    else latest_idr_index_ = 0;
                    if (consume_index_ >= to_remove) consume_index_ -= to_remove;
                    else consume_index_ = 0;
                    if (last_snapshot_end_ >= to_remove) last_snapshot_end_ -= to_remove;
                    else last_snapshot_end_ = 0;
                    if (audio_sync_index_ >= to_remove) audio_sync_index_ -= to_remove;
                    else audio_sync_index_ = 0;
                }
            }
            
            total_packets_received_++;
        }
        
        cv_.notify_all();
    }
    
    std::cout << "[" << name_ << "] FIFO stream processing ended" << std::endl;
    std::cout << "[" << name_ << "] Total packets in connection: " << total_packets_in_connection << std::endl;
}

// All remaining methods are identical to TCPReader
void FIFOInput::waitForStreamInfo() {
    std::cout << "[" << name_ << "] Waiting for stream info..." << std::endl;
    std::unique_lock<std::mutex> lock(buffer_mutex_);
    cv_.wait(lock, [this]{ return pids_ready_.load(); });
    std::cout << "[" << name_ << "] Stream info ready" << std::endl;
}

void FIFOInput::waitForIDR() {
    std::unique_lock<std::mutex> lock(buffer_mutex_);
    cv_.wait(lock, [this]{ return idr_ready_.load(); });
}

void FIFOInput::waitForAudioSync() {
    std::cout << "[" << name_ << "] Waiting for audio sync point..." << std::endl;
    std::unique_lock<std::mutex> lock(buffer_mutex_);
    
    auto timeout = std::chrono::seconds(5);
    bool found = cv_.wait_for(lock, timeout, [this]{ return audio_sync_ready_.load(); });
    
    if (found) {
        std::cout << "[" << name_ << "] Audio sync ready at index " << audio_sync_index_ << std::endl;
    } else {
        std::cerr << "[" << name_ << "] Warning: Audio sync timeout - using IDR as fallback" << std::endl;
        audio_sync_index_ = idr_index_;
        audio_sync_ready_ = true;
    }
}

void FIFOInput::resetForNewLoop() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    idr_ready_ = false;
    audio_ready_ = false;
    audio_sync_ready_ = false;
    idr_index_ = 0;
    audio_sync_index_ = 0;
    std::cout << "[" << name_ << "] Reset for new loop - waiting for next IDR and audio sync" << std::endl;
}

std::vector<ts::TSPacket> FIFOInput::getBufferedPacketsFromIDR() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (idr_index_ < rolling_buffer_.size()) {
        last_snapshot_end_ = rolling_buffer_.size();
        return std::vector<ts::TSPacket>(rolling_buffer_.begin() + idr_index_, rolling_buffer_.end());
    }
    return {};
}

std::vector<ts::TSPacket> FIFOInput::getBufferedPacketsFromAudioSync() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    size_t start_index = idr_index_;
    
    if (audio_sync_ready_.load()) {
        std::cout << "[" << name_ << "] IDR at " << idr_index_ << ", audio sync at " << audio_sync_index_ << std::endl;
    }
    
    if (start_index < rolling_buffer_.size()) {
        last_snapshot_end_ = rolling_buffer_.size();
        return std::vector<ts::TSPacket>(rolling_buffer_.begin() + start_index, rolling_buffer_.end());
    }
    return {};
}

std::vector<ts::TSPacket> FIFOInput::receivePackets(size_t maxPackets, int timeoutMs) {
    std::vector<ts::TSPacket> result;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    
    while (result.size() < maxPackets) {
        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            if (consume_index_ < rolling_buffer_.size()) {
                size_t available = rolling_buffer_.size() - consume_index_;
                size_t to_copy = std::min(maxPackets - result.size(), available);
                
                result.insert(result.end(),
                            rolling_buffer_.begin() + consume_index_,
                            rolling_buffer_.begin() + consume_index_ + to_copy);
                consume_index_ += to_copy;
                
                // Periodically trim consumed packets
                if (consume_index_ > max_buffer_packets_ / 2) {
                    rolling_buffer_.erase(rolling_buffer_.begin(), rolling_buffer_.begin() + consume_index_);
                    if (idr_index_ >= consume_index_) idr_index_ -= consume_index_;
                    else idr_index_ = 0;
                    if (latest_idr_index_ >= consume_index_) latest_idr_index_ -= consume_index_;
                    else latest_idr_index_ = 0;
                    consume_index_ = 0;
                }
            }
        }
        
        if (result.size() >= maxPackets) break;
        if (std::chrono::steady_clock::now() >= deadline) break;
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    return result;
}

void FIFOInput::initConsumptionFromIndex(size_t index) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    consume_index_ = index;
    std::cout << "[" << name_ << "] Consumption started at index " << consume_index_ << std::endl;
}

void FIFOInput::initConsumptionFromCurrentPosition() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    consume_index_ = rolling_buffer_.size();
    std::cout << "[" << name_ << "] Consumption started at current position " << consume_index_ << std::endl;
}

bool FIFOInput::extractTimestampBases() {
    auto packets = getBufferedPacketsFromIDR();
    if (packets.empty()) return false;
    
    // Extract SPS/PPS using NALParser
    NALParser nal_parser;
    for (const auto& pkt : packets) {
        if (pkt.getPID() == discovered_info_.video_pid && pkt.hasPayload()) {
            size_t header_size = pkt.getHeaderSize();
            const uint8_t* payload = pkt.b + header_size;
            size_t payload_size = ts::PKT_SIZE - header_size;
            
            if (payload_size > 0) {
                FrameInfo frame_info = nal_parser.parseVideoPayload(payload, payload_size);
                if (nal_parser.hasParameterSets()) {
                    sps_data_ = nal_parser.getLastSPS();
                    pps_data_ = nal_parser.getLastPPS();
                    break;
                }
            }
        }
    }
    
    // Find video and audio PTS bases
    std::vector<uint8_t> video_pes_buffer;
    std::vector<uint8_t> audio_pes_buffer;
    uint64_t video_pts_base = UINT64_MAX;
    uint64_t audio_pts_base_temp = UINT64_MAX;
    bool found_video_pts = false;
    bool found_audio_pts = false;
    
    for (const auto& pkt : packets) {
        // Extract video PTS base
        if (!found_video_pts && pkt.getPID() == discovered_info_.video_pid && pkt.hasPayload()) {
            if (pkt.getPUSI()) video_pes_buffer.clear();
            
            size_t header_size = pkt.getHeaderSize();
            const uint8_t* payload = pkt.b + header_size;
            size_t payload_size = ts::PKT_SIZE - header_size;
            
            if (payload_size > 0) {
                video_pes_buffer.insert(video_pes_buffer.end(), payload, payload + payload_size);
                if (video_pes_buffer.size() >= 14 && extractPTS(video_pes_buffer.data(), video_pes_buffer.size(), video_pts_base)) {
                    found_video_pts = true;
                    std::cout << "[" << name_ << "] Video PTS base: " << video_pts_base << std::endl;
                }
            }
        }
        
        // Extract audio PTS base
        if (!found_audio_pts && discovered_info_.audio_pid != ts::PID_NULL &&
            pkt.getPID() == discovered_info_.audio_pid && pkt.hasPayload()) {
            if (pkt.getPUSI()) audio_pes_buffer.clear();
            
            size_t header_size = pkt.getHeaderSize();
            const uint8_t* payload = pkt.b + header_size;
            size_t payload_size = ts::PKT_SIZE - header_size;
            
            if (payload_size > 0) {
                audio_pes_buffer.insert(audio_pes_buffer.end(), payload, payload + payload_size);
                if (audio_pes_buffer.size() >= 14 && extractPTS(audio_pes_buffer.data(), audio_pes_buffer.size(), audio_pts_base_temp)) {
                    found_audio_pts = true;
                    std::cout << "[" << name_ << "] Audio PTS base: " << audio_pts_base_temp << std::endl;
                }
            }
        }
        
        if (found_video_pts && (found_audio_pts || discovered_info_.audio_pid == ts::PID_NULL)) {
            break;
        }
    }
    
    // Use minimum of video and audio PTS as base
    if (found_video_pts && found_audio_pts) {
        pts_base_ = std::min(video_pts_base, audio_pts_base_temp);
        audio_pts_base_ = audio_pts_base_temp;
        std::cout << "[" << name_ << "] Using minimum PTS base: " << pts_base_ << std::endl;
    } else if (found_video_pts) {
        pts_base_ = video_pts_base;
        audio_pts_base_ = video_pts_base;
    } else if (found_audio_pts) {
        pts_base_ = audio_pts_base_temp;
        audio_pts_base_ = audio_pts_base_temp;
    } else {
        std::cerr << "[" << name_ << "] Warning: No PTS found!" << std::endl;
        return false;
    }
    
    // Extract first PCR
    uint64_t actual_first_pcr = 0;
    for (const auto& pkt : packets) {
        if (pkt.getPID() == discovered_info_.pcr_pid && extractPCR(pkt, actual_first_pcr)) {
            break;
        }
    }
    
    pcr_base_ = actual_first_pcr;
    
    // Calculate PCR/PTS alignment offset
    uint64_t expected_pcr_from_pts = pts_base_ * 300;
    if (actual_first_pcr > 0) {
        pcr_pts_alignment_offset_ = (int64_t)expected_pcr_from_pts - (int64_t)actual_first_pcr;
        std::cout << "[" << name_ << "] PCR base: " << pcr_base_ << std::endl;
        std::cout << "[" << name_ << "] PCR/PTS alignment offset: " << pcr_pts_alignment_offset_ << std::endl;
    } else {
        pcr_base_ = pts_base_ * 300;
        std::cout << "[" << name_ << "] PCR not found, using PTS-derived: " << pcr_base_ << std::endl;
    }
    
    return pts_base_ > 0;
}

bool FIFOInput::validateFirstAudioADTS(const std::vector<ts::TSPacket>& packets) const {
    // Find first audio packet with PUSI
    for (const auto& pkt : packets) {
        if (pkt.getPID() == discovered_info_.audio_pid && pkt.getPUSI() && pkt.hasPayload()) {
            size_t header_size = pkt.getHeaderSize();
            const uint8_t* payload = pkt.b + header_size;
            size_t payload_size = ts::PKT_SIZE - header_size;
            
            if (payload_size < 9) return false;
            
            // Check PES start code
            if (payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01) return false;
            
            // Get PES header length
            uint8_t pes_header_data_length = payload[8];
            size_t audio_data_start = 9 + pes_header_data_length;
            
            if (audio_data_start >= payload_size) {
                // ADTS in continuation packets - OK
                return true;
            }
            
            // Look for ADTS sync word (0xFFF)
            size_t remaining = payload_size - audio_data_start;
            const uint8_t* audio_data = payload + audio_data_start;
            
            for (size_t i = 0; i + 1 < remaining; i++) {
                if (audio_data[i] == 0xFF && (audio_data[i+1] & 0xF0) == 0xF0) {
                    std::cout << "[" << name_ << "] Valid ADTS sync at offset " << (audio_data_start + i) << std::endl;
                    return true;
                }
            }
            
            std::cout << "[" << name_ << "] ADTS not in first packet - trusting PES boundary" << std::endl;
            return true;
        }
    }
    
    // No audio or no audio PID
    if (discovered_info_.audio_pid == ts::PID_NULL) return true;
    
    std::cerr << "[" << name_ << "] No audio PUSI packet found" << std::endl;
    return false;
}
