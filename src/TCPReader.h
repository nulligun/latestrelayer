#ifndef TCP_READER_H
#define TCP_READER_H

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <deque>
#include <tsduck.h>
#include "StreamHealthMetrics.h"

// Stream information extracted from TS stream
struct StreamInfo {
    ts::PID video_pid = ts::PID_NULL;
    ts::PID audio_pid = ts::PID_NULL;
    ts::PID pcr_pid = ts::PID_NULL;
    ts::PID pmt_pid = ts::PID_NULL;
    uint16_t program_number = 0;
    uint8_t video_stream_type = 0;
    uint8_t audio_stream_type = 0;
    bool initialized = false;
};

/**
 * TCPReader - TCP client for receiving MPEG-TS streams
 * 
 * Based on multi2/src/tcp_main.cpp TCPReader pattern:
 * - Background thread for continuous buffering
 * - Automatic PAT/PMT discovery
 * - IDR frame detection
 * - Audio sync point detection (first audio PUSI after IDR)
 * - Rolling buffer with trim logic
 * - Infinite reconnection on disconnect
 */
class TCPReader {
public:
    TCPReader(const std::string& name, const std::string& host, uint16_t port);
    ~TCPReader();
    
    // Start background thread
    bool start();
    
    // Stop and cleanup
    void stop();
    
    // Wait for stream info (PAT/PMT) to be discovered
    void waitForStreamInfo();
    
    // Wait for IDR frame to be detected
    void waitForIDR();
    
    // Wait for audio sync point (first audio PUSI after IDR)
    void waitForAudioSync();
    
    // Reset for new loop - triggers fresh IDR and audio detection
    void resetForNewLoop();
    
    // Get stream information
    StreamInfo getStreamInfo() const { return discovered_info_; }
    
    // Extract timestamp bases from buffered packets
    bool extractTimestampBases();
    
    // Get buffered packets from IDR to end
    std::vector<ts::TSPacket> getBufferedPacketsFromIDR();
    
    // Get buffered packets from audio sync point (includes IDR)
    std::vector<ts::TSPacket> getBufferedPacketsFromAudioSync();
    
    // Receive packets from current position
    std::vector<ts::TSPacket> receivePackets(size_t maxPackets, int timeoutMs);
    
    // Initialize consumption from specific index
    void initConsumptionFromIndex(size_t index);
    
    // Initialize consumption from current buffer position
    void initConsumptionFromCurrentPosition();
    
    // Get last snapshot end index
    size_t getLastSnapshotEnd() const { return last_snapshot_end_; }
    
    // Timestamp bases
    uint64_t getPTSBase() const { return pts_base_; }
    uint64_t getAudioPTSBase() const { return audio_pts_base_; }
    uint64_t getPCRBase() const { return pcr_base_; }
    int64_t getPCRPTSAlignmentOffset() const { return pcr_pts_alignment_offset_; }
    
    // SPS/PPS data for splice injection
    std::vector<uint8_t> getSPSData() const { return sps_data_; }
    std::vector<uint8_t> getPPSData() const { return pps_data_; }
    
    // Validate first audio packet has valid ADTS header
    bool validateFirstAudioADTS(const std::vector<ts::TSPacket>& packets) const;
    
    // Connection status
    bool isConnected() const { return connected_.load(); }
    bool isStreamReady() const { return pids_ready_.load() && idr_ready_.load(); }
    
    // Health checking methods
    bool isHealthy() const { return connected_.load() && health_metrics_.isHealthy(); }
    bool isDataFresh(int64_t maxAgeMs = 0) const {
        if (maxAgeMs > 0) {
            return health_metrics_.getMsSinceLastData() < maxAgeMs;
        }
        return health_metrics_.isDataFresh();
    }
    int64_t getMsSinceLastData() const { return health_metrics_.getMsSinceLastData(); }
    uint64_t getCurrentBitrateBps() const { return health_metrics_.getCurrentBitrateBps(); }
    void configureHealthThresholds(const StreamHealthConfig& config) {
        health_metrics_.configure(config);
    }
    
    // Statistics
    uint64_t getPacketsReceived() const { return total_packets_received_.load(); }
    
private:
    // Connection management
    bool attemptConnection();
    void closeSocket();
    void backgroundThreadFunc();
    void processTCPStream();
    
    // Configuration
    std::string name_;
    std::string host_;
    uint16_t port_;
    
    // Socket
    int sockfd_;
    
    // Threading
    std::thread bg_thread_;
    std::atomic<bool> stop_thread_;
    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    
    // Stream discovery state
    std::atomic<bool> pids_ready_;
    std::atomic<bool> idr_ready_;
    std::atomic<bool> audio_ready_;
    std::atomic<bool> audio_sync_ready_;
    std::atomic<bool> first_packet_received_;
    
    // Buffer management
    std::mutex buffer_mutex_;
    std::condition_variable cv_;
    std::vector<ts::TSPacket> rolling_buffer_;
    size_t idr_index_;              // Initial IDR index for first connection
    size_t latest_idr_index_;       // Most recent IDR index (continuously updated)
    size_t audio_sync_index_;
    size_t consume_index_;
    size_t last_snapshot_end_;
    size_t max_buffer_packets_;
    
    // Discovered stream info
    StreamInfo discovered_info_;
    
    // Timestamp bases
    uint64_t pts_base_;
    uint64_t audio_pts_base_;
    uint64_t pcr_base_;
    int64_t pcr_pts_alignment_offset_;
    
    // SPS/PPS data
    std::vector<uint8_t> sps_data_;
    std::vector<uint8_t> pps_data_;
    
    // Statistics
    std::atomic<uint64_t> total_packets_received_;
    std::chrono::steady_clock::time_point last_progress_report_;
    std::chrono::steady_clock::time_point connection_start_time_;
    
    // Health monitoring
    StreamHealthMetrics health_metrics_;
    
    // Constants
    static constexpr int TCP_RECONNECT_DELAY_MS = 2000;
    static constexpr size_t MAX_BUFFER_PACKETS = 1500;  // ~3 seconds at 2Mbps
};

#endif // TCP_READER_H
