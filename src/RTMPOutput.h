#pragma once

#include <tsduck.h>
#include <string>
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

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
    
    std::string rtmp_url_;
    
    int stdin_pipe_[2];   // Pipe to FFmpeg stdin
    int stderr_pipe_[2];  // Pipe from FFmpeg stderr
    pid_t ffmpeg_pid_;
    
    std::atomic<bool> running_;
    std::atomic<bool> should_stop_monitor_;
    std::atomic<bool> rtmp_connected_;
    std::atomic<uint64_t> packets_written_;
    
    std::thread monitor_thread_;
    
    // Last write time for health check
    std::chrono::steady_clock::time_point last_write_time_;
    
    // Pacing state
    std::optional<uint64_t> last_pts_;
    std::chrono::steady_clock::time_point last_write_wallclock_;
};