#pragma once

#include <tsduck.h>
#include "TSPacketQueue.h"
#include <thread>
#include <atomic>
#include <cstdint>
#include <string>
#include <sys/types.h>

/**
 * RTMPReceiver - Pulls RTMP stream using FFmpeg subprocess and outputs TS packets to a queue.
 *
 * This class spawns an FFmpeg process that connects to an RTMP URL and outputs
 * MPEG-TS data to stdout. The receiver reads this data and extracts TS packets
 * into the provided queue.
 *
 * The receiver includes auto-reconnect logic with exponential backoff. When the
 * RTMP source disconnects (FFmpeg exits or gets EOF), the receiver will automatically
 * attempt to reconnect, similar to how UDPReceiver keeps listening for packets.
 *
 * Usage:
 *   RTMPReceiver receiver("Drone", "rtmp://nginx-rtmp:1935/publish/drone", queue);
 *   receiver.start();
 *   // ... packets are pushed to queue ...
 *   receiver.stop();
 */
class RTMPReceiver {
public:
    /**
     * Constructor with default reconnect parameters
     * @param name Display name for logging
     * @param rtmp_url RTMP URL to pull from (e.g., "rtmp://nginx-rtmp:1935/publish/drone")
     * @param queue Reference to TSPacketQueue to push packets into
     * @param initial_reconnect_delay_ms Initial delay between reconnection attempts (default 1000ms)
     * @param max_reconnect_delay_ms Maximum delay between reconnection attempts (default 30000ms)
     * @param backoff_multiplier Multiplier for exponential backoff (default 2.0)
     */
    RTMPReceiver(const std::string& name, const std::string& rtmp_url, TSPacketQueue& queue,
                 uint32_t initial_reconnect_delay_ms = 1000,
                 uint32_t max_reconnect_delay_ms = 30000,
                 double backoff_multiplier = 2.0);
    ~RTMPReceiver();
    
    // Prevent copying
    RTMPReceiver(const RTMPReceiver&) = delete;
    RTMPReceiver& operator=(const RTMPReceiver&) = delete;
    
    /**
     * Start the FFmpeg subprocess and receiver thread
     * @return true if started successfully, false on error
     */
    bool start();
    
    /**
     * Stop the receiver and FFmpeg subprocess
     */
    void stop();
    
    /**
     * Check if receiver is currently running
     */
    bool isRunning() const { return running_.load(); }
    
    /**
     * Check if FFmpeg process is connected to RTMP stream
     */
    bool isConnected() const { return connected_.load(); }
    
    // Statistics
    uint64_t getPacketsReceived() const { return packets_received_.load(); }
    uint64_t getPacketsDropped() const { return packets_dropped_.load(); }
    uint64_t getBytesReceived() const { return bytes_received_.load(); }
    
private:
    /**
     * Thread function for receiving packets from FFmpeg stdout
     */
    void receiveLoop();
    
    /**
     * Start FFmpeg subprocess
     * @return true if process started, false on error
     */
    bool startFFmpeg();
    
    /**
     * Stop FFmpeg subprocess gracefully
     */
    void stopFFmpeg();
    
    std::string name_;
    std::string rtmp_url_;
    TSPacketQueue& queue_;
    
    std::thread receiver_thread_;
    std::atomic<bool> running_;
    std::atomic<bool> should_stop_;
    std::atomic<bool> connected_;  // True when FFmpeg is receiving RTMP data
    
    // FFmpeg process
    pid_t ffmpeg_pid_;
    int ffmpeg_stdout_fd_;  // File descriptor for reading FFmpeg stdout
    
    // Statistics
    std::atomic<uint64_t> packets_received_;
    std::atomic<uint64_t> packets_dropped_;
    std::atomic<uint64_t> bytes_received_;
    std::atomic<uint64_t> reconnect_attempts_;
    
    // Reconnect configuration
    uint32_t initial_reconnect_delay_ms_;
    uint32_t max_reconnect_delay_ms_;
    double backoff_multiplier_;
};