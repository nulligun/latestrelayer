#pragma once

#include <tsduck.h>
#include "TSStreamReassembler.h"
#include "TSAnalyzer.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <string>
#include <cstdint>
#include <chrono>

/**
 * TCPReceiver - TCP Client for Receiving MPEG-TS from FFmpeg
 * 
 * Connects to FFmpeg TCP server (listen mode) and receives MPEG-TS stream.
 * Features:
 * - TCP client with automatic reconnection
 * - Background thread for continuous buffering
 * - TSStreamReassembler for packet boundary handling
 * - PAT/PMT discovery for stream info
 * - IDR frame detection for clean switching
 * - Rolling buffer (~3 seconds / 1500 packets)
 * - Thread-safe access to buffered packets
 */
class TCPReceiver {
public:
    /**
     * Constructor
     * @param name Stream name for logging
     * @param host TCP server hostname/IP
     * @param port TCP server port
     */
    TCPReceiver(const std::string& name, const std::string& host, uint16_t port);
    
    ~TCPReceiver();
    
    // Start TCP connection and background thread
    bool start();
    
    // Stop TCP connection and join background thread
    void stop();
    
    // Check if receiver is running
    bool isRunning() const { return running_.load(); }
    
    // Check if TCP connected
    bool isConnected() const { return connected_.load(); }
    
    // Check if stream is ready (PAT/PMT discovered AND IDR detected)
    bool isStreamReady() const { return pids_ready_.load() && idr_ready_.load(); }
    
    // Wait for stream info (PAT/PMT) discovery - blocks until ready
    void waitForStreamInfo();
    
    // Wait for IDR frame detection - blocks until ready
    void waitForIDR();
    
    // Get discovered stream information
    StreamInfo getStreamInfo() const { return discovered_info_; }
    
    // Get all buffered packets from IDR frame onwards
    std::vector<ts::TSPacket> getBufferedPacketsFromIDR();
    
    // Get SPS/PPS data (if available)
    std::vector<uint8_t> getSPSData() const { return sps_data_; }
    std::vector<uint8_t> getPPSData() const { return pps_data_; }
    
    // Get timestamp bases extracted from stream
    uint64_t getPTSBase() const { return pts_base_; }
    uint64_t getAudioPTSBase() const { return audio_pts_base_; }
    uint64_t getPCRBase() const { return pcr_base_; }
    int64_t getPCRPTSAlignmentOffset() const { return pcr_pts_alignment_offset_; }
    
    // Receive packets from stream (non-blocking, returns what's available)
    std::vector<ts::TSPacket> receivePackets(size_t maxPackets, int timeoutMs);
    
    // Initialize consumption from specific index in rolling buffer
    void initConsumptionFromIndex(size_t index);
    
    // Initialize consumption from current buffer position
    void initConsumptionFromCurrentPosition();
    
    // Get last snapshot end index (for preventing packet loss during switch)
    size_t getLastSnapshotEnd() const { return last_snapshot_end_; }
    
    // Reset for new loop iteration - triggers new IDR detection
    void resetForNewLoop();
    
    // Extract timestamp bases from buffered packets
    bool extractTimestampBases();
    
    // Get statistics
    uint64_t getPacketsReceived() const { return total_packets_received_.load(); }
    
private:
    // Thread function for TCP connection and buffering
    void backgroundThreadFunc();
    
    // Attempt TCP connection to server
    bool attemptConnection();
    
    // Process TCP stream (receive and buffer packets)
    void processTCPStream();
    
    // Close TCP socket
    void closeSocket();
    
    std::string name_;
    std::string host_;
    uint16_t port_;
    int sockfd_;
    
    std::thread bg_thread_;
    std::atomic<bool> stop_thread_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> pids_ready_{false};
    std::atomic<bool> idr_ready_{false};
    std::atomic<bool> audio_ready_{false};
    std::atomic<bool> first_packet_received_{false};
    
    // Thread synchronization
    std::mutex buffer_mutex_;
    std::condition_variable cv_;
    
    // Rolling buffer and state
    std::vector<ts::TSPacket> rolling_buffer_;
    size_t idr_index_ = 0;
    size_t consume_index_ = 0;
    size_t last_snapshot_end_ = 0;
    size_t max_buffer_packets_ = 1500;  // ~3 seconds at 2Mbps
    
    // Stream discovery
    StreamInfo discovered_info_;
    std::vector<uint8_t> sps_data_;
    std::vector<uint8_t> pps_data_;
    
    // Timestamp tracking
    uint64_t pts_base_ = 0;
    uint64_t audio_pts_base_ = 0;
    uint64_t pcr_base_ = 0;
    int64_t pcr_pts_alignment_offset_ = 0;
    
    // Statistics
    std::atomic<uint64_t> total_packets_received_{0};
    std::chrono::steady_clock::time_point last_progress_report_;
    
    // TCP reconnection configuration
    static constexpr int TCP_RECONNECT_DELAY_MS = 2000;  // 2 seconds
};