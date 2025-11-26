#include "RTMPOutput.h"
#include <iostream>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <thread>
#include <fcntl.h>
#include <sstream>

RTMPOutput::RTMPOutput(const std::string& rtmp_url)
    : rtmp_url_(rtmp_url),
      stdin_pipe_{-1, -1},
      stderr_pipe_{-1, -1},
      ffmpeg_pid_(-1),
      running_(false),
      should_stop_monitor_(false),
      should_stop_reconnect_(false),
      connection_state_(ConnectionState::DISCONNECTED),
      packets_written_(0),
      packets_dropped_(0),
      reconnection_attempts_(0),
      total_disconnections_(0),
      successful_reconnections_(0),
      disconnect_time_(std::chrono::steady_clock::now()),
      last_reconnect_attempt_(std::chrono::steady_clock::now()),
      last_write_time_(std::chrono::steady_clock::now()),
      last_write_wallclock_(std::chrono::steady_clock::now()) {
}

RTMPOutput::~RTMPOutput() {
    stop();
}

bool RTMPOutput::start() {
    if (running_.load()) {
        std::cerr << "[RTMPOutput] Already running" << std::endl;
        return false;
    }
    
    running_ = true;
    should_stop_reconnect_ = false;
    
    // Start reconnection thread
    reconnection_thread_ = std::thread(&RTMPOutput::reconnectionLoop, this);
    
    // Attempt initial connection
    if (!spawnFFmpeg()) {
        std::cerr << "[RTMPOutput] Initial connection failed, will retry..." << std::endl;
        connection_state_ = ConnectionState::DISCONNECTED;
        return true; // Don't fail - reconnection thread will retry
    }
    
    return true;
}

void RTMPOutput::stop() {
    if (!running_.load()) {
        return;
    }
    
    std::cout << "[RTMPOutput] Stopping FFmpeg..." << std::endl;
    running_ = false;
    should_stop_monitor_ = true;
    should_stop_reconnect_ = true;
    
    // Wait for reconnection thread to finish
    if (reconnection_thread_.joinable()) {
        reconnection_thread_.join();
    }
    
    // Wait for monitor thread to finish
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    
    closeFFmpeg();
    
    std::cout << "[RTMPOutput] Stopped. Statistics:" << std::endl;
    std::cout << "  Total packets written: " << packets_written_.load() << std::endl;
    std::cout << "  Total packets dropped: " << packets_dropped_.load() << std::endl;
    std::cout << "  Total disconnections: " << total_disconnections_.load() << std::endl;
    std::cout << "  Successful reconnections: " << successful_reconnections_.load() << std::endl;
}

bool RTMPOutput::writePacket(const ts::TSPacket& packet) {
    if (!running_.load()) {
        return false;
    }
    
    // Check connection state - drop packets if disconnected or reconnecting
    ConnectionState state = connection_state_.load();
    if (state != ConnectionState::CONNECTED && state != ConnectionState::CONNECTING) {
        packets_dropped_++;
        return false;
    }
    
    // Verify pipe is valid
    if (stdin_pipe_[1] < 0) {
        handleDisconnection();
        packets_dropped_++;
        return false;
    }
    
    // Write 188-byte TS packet to FFmpeg stdin
    ssize_t written = write(stdin_pipe_[1], packet.b, ts::PKT_SIZE);
    
    if (written != ts::PKT_SIZE) {
        // Pipe broken or write error - handle disconnection
        handleDisconnection();
        packets_dropped_++;
        return false;
    }
    
    packets_written_++;
    last_write_time_ = std::chrono::steady_clock::now();
    
    // Simple pacing: Sleep a bit to avoid overwhelming FFmpeg
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    
    return true;
}

bool RTMPOutput::spawnFFmpeg() {
    std::lock_guard<std::mutex> lock(reconnection_mutex_);
    
    connection_state_ = ConnectionState::CONNECTING;
    
    std::cout << "[RTMPOutput] ========================================" << std::endl;
    std::cout << "[RTMPOutput] Starting RTMP Output" << std::endl;
    std::cout << "[RTMPOutput] ========================================" << std::endl;
    std::cout << "[RTMPOutput] Target URL: " << rtmp_url_ << std::endl;
    std::cout << "[RTMPOutput] Initializing FFmpeg process..." << std::endl;
    
    // Create pipes for stdin and stderr
    if (pipe(stdin_pipe_) < 0) {
        std::cerr << "[RTMPOutput] ✗ Failed to create stdin pipe: " << strerror(errno) << std::endl;
        connection_state_ = ConnectionState::DISCONNECTED;
        return false;
    }
    
    if (pipe(stderr_pipe_) < 0) {
        std::cerr << "[RTMPOutput] ✗ Failed to create stderr pipe: " << strerror(errno) << std::endl;
        close(stdin_pipe_[0]);
        close(stdin_pipe_[1]);
        connection_state_ = ConnectionState::DISCONNECTED;
        return false;
    }
    
    // Fork the process
    ffmpeg_pid_ = fork();
    
    if (ffmpeg_pid_ < 0) {
        std::cerr << "[RTMPOutput] ✗ Failed to fork: " << strerror(errno) << std::endl;
        close(stdin_pipe_[0]);
        close(stdin_pipe_[1]);
        close(stderr_pipe_[0]);
        close(stderr_pipe_[1]);
        connection_state_ = ConnectionState::DISCONNECTED;
        return false;
    }
    
    if (ffmpeg_pid_ == 0) {
        // Child process - execute FFmpeg
        
        // Redirect stdin to read from pipe
        dup2(stdin_pipe_[0], STDIN_FILENO);
        close(stdin_pipe_[0]);
        close(stdin_pipe_[1]);
        
        // Redirect stderr to write to pipe
        dup2(stderr_pipe_[1], STDERR_FILENO);
        close(stderr_pipe_[0]);
        close(stderr_pipe_[1]);
        
        // Execute FFmpeg with timestamp handling compatible with -c copy
        execlp("ffmpeg",
               "ffmpeg",
               "-i", "-",                          // Input from stdin
               "-c", "copy",                       // Copy codec (no re-encoding)
               "-fflags", "+genpts+igndts",        // Generate PTS if missing, ignore DTS issues
               "-avoid_negative_ts", "make_zero",  // Shift timestamps to avoid negatives
               "-max_interleave_delta", "0",       // Don't reorder packets
               "-f", "flv",                        // Output format FLV
               rtmp_url_.c_str(),                  // RTMP URL
               nullptr);
        
        // If we get here, exec failed
        std::cerr << "[RTMPOutput] ✗ Failed to execute ffmpeg: " << strerror(errno) << std::endl;
        exit(1);
    }
    
    // Parent process
    // Close unused ends of pipes
    close(stdin_pipe_[0]);  // Close read end of stdin pipe
    close(stderr_pipe_[1]); // Close write end of stderr pipe
    
    std::cout << "[RTMPOutput] FFmpeg process started (PID: " << ffmpeg_pid_ << ")" << std::endl;
    std::cout << "[RTMPOutput] Monitoring RTMP connection status..." << std::endl;
    
    // Start monitoring thread for FFmpeg stderr
    should_stop_monitor_ = false;
    monitor_thread_ = std::thread(&RTMPOutput::monitorFFmpegOutput, this);
    
    // Give FFmpeg 2 seconds to report connection, then assume success
    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        
        if (connection_state_.load() == ConnectionState::CONNECTING) {
            std::cout << "[RTMPOutput] Grace period elapsed - assuming connection succeeded" << std::endl;
            connection_state_ = ConnectionState::CONNECTED;
            reconnection_attempts_ = 0;
        }
    }).detach();
    
    return true;
}

void RTMPOutput::closeFFmpeg() {
    auto shutdown_start = std::chrono::steady_clock::now();
    std::cout << "[RTMPOutput] Beginning graceful shutdown..." << std::endl;
    
    // Close pipes
    if (stdin_pipe_[1] >= 0) {
        close(stdin_pipe_[1]);
        stdin_pipe_[1] = -1;
        std::cout << "[RTMPOutput] Closed stdin pipe" << std::endl;
    }
    
    if (stderr_pipe_[0] >= 0) {
        close(stderr_pipe_[0]);
        stderr_pipe_[0] = -1;
        std::cout << "[RTMPOutput] Closed stderr pipe" << std::endl;
    }
    
    // Terminate FFmpeg process with timeout and fallback
    if (ffmpeg_pid_ > 0) {
        std::cout << "[RTMPOutput] Sending SIGTERM to FFmpeg (PID: " << ffmpeg_pid_ << ")" << std::endl;
        kill(ffmpeg_pid_, SIGTERM);
        
        // Wait for process to exit with 1 second timeout
        int status;
        const int timeout_ms = 1000;
        const int poll_interval_ms = 50;
        int elapsed_ms = 0;
        
        while (elapsed_ms < timeout_ms) {
            pid_t result = waitpid(ffmpeg_pid_, &status, WNOHANG);
            
            if (result == ffmpeg_pid_) {
                // Process has exited
                std::cout << "[RTMPOutput] FFmpeg exited gracefully";
                if (WIFEXITED(status)) {
                    std::cout << " (exit code: " << WEXITSTATUS(status) << ")";
                }
                std::cout << std::endl;
                ffmpeg_pid_ = -1;
                break;
            } else if (result == -1) {
                // Error or no such process
                std::cout << "[RTMPOutput] FFmpeg process already terminated" << std::endl;
                ffmpeg_pid_ = -1;
                break;
            }
            
            // Still running, wait a bit
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
            elapsed_ms += poll_interval_ms;
        }
        
        // If still running after timeout, force kill
        if (ffmpeg_pid_ > 0) {
            std::cout << "[RTMPOutput] FFmpeg did not exit within " << timeout_ms
                      << "ms, sending SIGKILL" << std::endl;
            kill(ffmpeg_pid_, SIGKILL);
            
            // Wait for force kill to complete (should be immediate)
            waitpid(ffmpeg_pid_, &status, 0);
            std::cout << "[RTMPOutput] FFmpeg force-killed" << std::endl;
            ffmpeg_pid_ = -1;
        }
    }
    
    auto shutdown_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - shutdown_start
    );
    std::cout << "[RTMPOutput] Shutdown complete (" << shutdown_duration.count() << "ms)" << std::endl;
}

bool RTMPOutput::checkProcessHealth() {
    if (ffmpeg_pid_ <= 0) {
        return false;
    }
    
    // Check if process is still running
    int status;
    pid_t result = waitpid(ffmpeg_pid_, &status, WNOHANG);
    
    if (result == ffmpeg_pid_) {
        // Process has exited
        std::cerr << "[RTMPOutput] ✗ FFmpeg process exited unexpectedly" << std::endl;
        if (WIFEXITED(status)) {
            std::cerr << "[RTMPOutput] Exit code: " << WEXITSTATUS(status) << std::endl;
        }
        return false;
    }
    
    return true;
}

void RTMPOutput::monitorFFmpegOutput() {
    std::cout << "[RTMPOutput] FFmpeg output monitor thread started" << std::endl;
    
    // Set stderr pipe to non-blocking
    int flags = fcntl(stderr_pipe_[0], F_GETFL, 0);
    fcntl(stderr_pipe_[0], F_SETFL, flags | O_NONBLOCK);
    
    char buffer[4096];
    std::string line_buffer;
    
    while (!should_stop_monitor_.load()) {
        ssize_t bytes_read = read(stderr_pipe_[0], buffer, sizeof(buffer) - 1);
        
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            line_buffer += buffer;
            
            // Process complete lines
            size_t pos;
            while ((pos = line_buffer.find('\n')) != std::string::npos) {
                std::string line = line_buffer.substr(0, pos);
                line_buffer.erase(0, pos + 1);
                
                if (!line.empty()) {
                    parseFFmpegOutput(line);
                }
            }
        } else if (bytes_read == 0) {
            // EOF - FFmpeg closed stderr
            std::cerr << "[RTMPOutput] FFmpeg closed stderr (process may have exited)" << std::endl;
            break;
        } else {
            // No data available or error
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "[RTMPOutput] Error reading FFmpeg output: " << strerror(errno) << std::endl;
                break;
            }
            
            // No data, sleep briefly
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    std::cout << "[RTMPOutput] FFmpeg output monitor thread stopped" << std::endl;
}

void RTMPOutput::parseFFmpegOutput(const std::string& line) {
    // Log FFmpeg output for debugging (skip noisy DTS warnings)
    if (line.find("Non-monotonous DTS in output") == std::string::npos) {
        std::cout << "[RTMPOutput] FFmpeg: " << line << std::endl;
    }
    
    // Convert to lowercase for easier matching
    std::string lower_line = line;
    std::transform(lower_line.begin(), lower_line.end(), lower_line.begin(), ::tolower);
    
    // Check for RTMP connection establishment
    if (lower_line.find("opening") != std::string::npos &&
        lower_line.find("rtmp") != std::string::npos) {
        std::cout << "[RTMPOutput] Attempting RTMP connection..." << std::endl;
    }
    
    // Check for successful connection
    if ((lower_line.find("stream #0") != std::string::npos ||
         lower_line.find("output stream") != std::string::npos ||
         lower_line.find("writing") != std::string::npos) &&
        connection_state_.load() != ConnectionState::CONNECTED) {
        
        connection_state_ = ConnectionState::CONNECTED;
        
        std::cout << "[RTMPOutput] ========================================" << std::endl;
        std::cout << "[RTMPOutput] ✓ RTMP CONNECTION ESTABLISHED" << std::endl;
        std::cout << "[RTMPOutput] ========================================" << std::endl;
        std::cout << "[RTMPOutput] Streaming to nginx-rtmp server" << std::endl;
        
        // Reset reconnection attempts on successful connection
        reconnection_attempts_ = 0;
    }
    
    // Check for errors
    if (lower_line.find("error") != std::string::npos ||
        lower_line.find("failed") != std::string::npos ||
        lower_line.find("refused") != std::string::npos ||
        lower_line.find("timeout") != std::string::npos ||
        lower_line.find("could not") != std::string::npos) {
        
        std::cerr << "[RTMPOutput] ========================================" << std::endl;
        std::cerr << "[RTMPOutput] ✗ RTMP ERROR DETECTED" << std::endl;
        std::cerr << "[RTMPOutput] ========================================" << std::endl;
        std::cerr << "[RTMPOutput] Error details: " << line << std::endl;
        
        // Provide specific diagnostics
        if (lower_line.find("connection refused") != std::string::npos) {
            std::cerr << "[RTMPOutput] Diagnosis: nginx-rtmp server refused connection" << std::endl;
            std::cerr << "[RTMPOutput] Possible causes:" << std::endl;
            std::cerr << "[RTMPOutput]   - nginx-rtmp container not running" << std::endl;
            std::cerr << "[RTMPOutput]   - Check: docker ps | grep nginx-rtmp" << std::endl;
            std::cerr << "[RTMPOutput]   - Network issue between containers" << std::endl;
        } else if (lower_line.find("timeout") != std::string::npos) {
            std::cerr << "[RTMPOutput] Diagnosis: Connection timeout" << std::endl;
            std::cerr << "[RTMPOutput] Possible causes:" << std::endl;
            std::cerr << "[RTMPOutput]   - nginx-rtmp server not responding" << std::endl;
            std::cerr << "[RTMPOutput]   - Network connectivity issue" << std::endl;
            std::cerr << "[RTMPOutput]   - Firewall blocking connection" << std::endl;
        } else if (lower_line.find("not found") != std::string::npos ||
                   lower_line.find("unknown") != std::string::npos) {
            std::cerr << "[RTMPOutput] Diagnosis: Host not found" << std::endl;
            std::cerr << "[RTMPOutput] Possible causes:" << std::endl;
            std::cerr << "[RTMPOutput]   - Incorrect hostname in RTMP URL" << std::endl;
            std::cerr << "[RTMPOutput]   - DNS resolution failure" << std::endl;
            std::cerr << "[RTMPOutput]   - Check config.yaml rtmp_url setting" << std::endl;
        }
        
        std::cerr << "[RTMPOutput] ========================================" << std::endl;
        
        connection_state_ = ConnectionState::DISCONNECTED;
    }
}

void RTMPOutput::handleDisconnection() {
    ConnectionState current_state = connection_state_.load();
    
    // Only log and trigger reconnection if we were connected
    if (current_state == ConnectionState::CONNECTED) {
        connection_state_ = ConnectionState::DISCONNECTED;
        total_disconnections_++;
        disconnect_time_ = std::chrono::steady_clock::now();
        
        std::cerr << "[RTMPOutput] ========================================" << std::endl;
        std::cerr << "[RTMPOutput] ✗ RTMP CONNECTION LOST" << std::endl;
        std::cerr << "[RTMPOutput] ========================================" << std::endl;
        std::cerr << "[RTMPOutput] Write error detected - entering reconnection mode" << std::endl;
        std::cerr << "[RTMPOutput] Packets will be dropped until reconnection succeeds" << std::endl;
        
        // Close the broken FFmpeg process
        std::lock_guard<std::mutex> lock(reconnection_mutex_);
        closeFFmpeg();
    }
}

void RTMPOutput::reconnectionLoop() {
    std::cout << "[RTMPOutput] Reconnection thread started" << std::endl;
    
    while (!should_stop_reconnect_.load()) {
        ConnectionState current_state = connection_state_.load();
        
        // Only attempt reconnection if we're disconnected
        if (current_state == ConnectionState::DISCONNECTED) {
            attemptReconnection();
        }
        
        // Sleep briefly to avoid busy-waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    std::cout << "[RTMPOutput] Reconnection thread stopped" << std::endl;
}

void RTMPOutput::attemptReconnection() {
    // Calculate backoff delay
    uint32_t attempt = reconnection_attempts_.load();
    uint32_t backoff_ms = INITIAL_BACKOFF_MS * (1 << std::min(attempt, 5u)); // Cap at 2^5 = 32x
    backoff_ms = std::min(backoff_ms, MAX_BACKOFF_MS);
    
    // Check if enough time has passed since last attempt
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_reconnect_attempt_
    );
    
    if (elapsed.count() < backoff_ms) {
        return; // Not time to retry yet
    }
    
    // Mark as reconnecting
    connection_state_ = ConnectionState::RECONNECTING;
    reconnection_attempts_++;
    last_reconnect_attempt_ = now;
    
    // Rate-limited logging - log every attempt for first 5, then every 5th attempt
    bool should_log = (attempt < 5) || (attempt % 5 == 0);
    
    if (should_log) {
        auto disconnected_duration = std::chrono::duration_cast<std::chrono::seconds>(
            now - disconnect_time_
        );
        
        std::cout << "[RTMPOutput] ========================================" << std::endl;
        std::cout << "[RTMPOutput] Reconnection attempt #" << attempt + 1 << std::endl;
        std::cout << "[RTMPOutput] Disconnected for: " << disconnected_duration.count() << "s" << std::endl;
        std::cout << "[RTMPOutput] Next retry delay: " << backoff_ms / 1000.0 << "s" << std::endl;
        std::cout << "[RTMPOutput] ========================================" << std::endl;
    }
    
    // Attempt to spawn FFmpeg
    if (spawnFFmpeg()) {
        // Success - wait a bit to verify connection
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        if (connection_state_.load() == ConnectionState::CONNECTED) {
            successful_reconnections_++;
            
            auto reconnected_duration = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - disconnect_time_
            );
            
            std::cout << "[RTMPOutput] ========================================" << std::endl;
            std::cout << "[RTMPOutput] ✓ RTMP RECONNECTED SUCCESSFULLY" << std::endl;
            std::cout << "[RTMPOutput] ========================================" << std::endl;
            std::cout << "[RTMPOutput] Reconnection attempt: " << attempt + 1 << std::endl;
            std::cout << "[RTMPOutput] Downtime: " << reconnected_duration.count() << "s" << std::endl;
            std::cout << "[RTMPOutput] Resuming packet processing..." << std::endl;
            
            // Reset reconnection attempts
            reconnection_attempts_ = 0;
        }
    } else {
        // Spawn failed - state should already be DISCONNECTED
        connection_state_ = ConnectionState::DISCONNECTED;
    }
}