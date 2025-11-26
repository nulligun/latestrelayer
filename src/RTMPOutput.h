#pragma once

#include <tsduck.h>
#include <string>
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <mutex>

class RTMPOutput {
public:
    explicit RTMPOutput(const std::string& rtmp_url);
    ~RTMPOutput();
    
    // Start FFmpeg process
    bool start();
    
    // Stop FFmpeg process
    void stop();
    
    // Write a TS packet to FFmpeg stdin
    bool writePacket(const ts::TSPacket& packet);
    
    // Check if FFmpeg is running
    bool isRunning() const { return running_.load(); }
    
    // Get packets written count
    uint64_t getPacketsWritten() const { return packets_written_.load(); }
    
    // Connection states
    enum class ConnectionState {
        DISCONNECTED,
        CONNECTING,
        CONNECTED,
        RECONNECTING
    };
    
private:
    // Spawn FFmpeg process with pipes for stdin and stderr
    bool spawnFFmpeg();
    
    // Close FFmpeg process
    void closeFFmpeg();
    
    // Check FFmpeg process health
    bool checkProcessHealth();
    
    // Monitor FFmpeg stderr output for RTMP connection status
    void monitorFFmpegOutput();
    
    // Parse FFmpeg output line for RTMP events
    void parseFFmpegOutput(const std::string& line);
    
    // Reconnection logic
    void reconnectionLoop();
    void handleDisconnection();
    void attemptReconnection();
    
    std::string rtmp_url_;
    
    int stdin_pipe_[2];   // Pipe to FFmpeg stdin
    int stderr_pipe_[2];  // Pipe from FFmpeg stderr
    pid_t ffmpeg_pid_;
    
    std::atomic<bool> running_;
    std::atomic<bool> should_stop_monitor_;
    std::atomic<bool> should_stop_reconnect_;
    std::atomic<ConnectionState> connection_state_;
    std::atomic<uint64_t> packets_written_;
    std::atomic<uint64_t> packets_dropped_;
    
    std::thread monitor_thread_;
    std::thread reconnection_thread_;
    std::mutex reconnection_mutex_;
    
    // Reconnection state
    std::atomic<uint32_t> reconnection_attempts_;
    std::atomic<uint32_t> total_disconnections_;
    std::atomic<uint32_t> successful_reconnections_;
    std::chrono::steady_clock::time_point disconnect_time_;
    std::chrono::steady_clock::time_point last_reconnect_attempt_;
    
    // Exponential backoff configuration
    static constexpr uint32_t INITIAL_BACKOFF_MS = 1000;      // 1 second
    static constexpr uint32_t MAX_BACKOFF_MS = 30000;         // 30 seconds
    static constexpr uint32_t BACKOFF_MULTIPLIER = 2;
    
    // Last write time for health check
    std::chrono::steady_clock::time_point last_write_time_;
    
    // Pacing state
    std::optional<uint64_t> last_pts_;
    std::chrono::steady_clock::time_point last_write_wallclock_;
};