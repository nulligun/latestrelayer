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
      rtmp_connected_(false),
      packets_written_(0),
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
    
    if (!spawnFFmpeg()) {
        return false;
    }
    
    running_ = true;
    return true;
}

void RTMPOutput::stop() {
    if (!running_.load()) {
        return;
    }
    
    std::cout << "[RTMPOutput] Stopping FFmpeg..." << std::endl;
    running_ = false;
    should_stop_monitor_ = true;
    
    // Wait for monitor thread to finish
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    
    closeFFmpeg();
    std::cout << "[RTMPOutput] Stopped. Total packets written: " 
              << packets_written_.load() << std::endl;
}

bool RTMPOutput::writePacket(const ts::TSPacket& packet) {
    if (!running_.load() || stdin_pipe_[1] < 0) {
        return false;
    }
    
    // Write 188-byte TS packet to FFmpeg stdin
    ssize_t written = write(stdin_pipe_[1], packet.b, ts::PKT_SIZE);
    
    if (written != ts::PKT_SIZE) {
        if (written < 0) {
            std::cerr << "[RTMPOutput] ✗ Write error: " << strerror(errno) << std::endl;
            std::cerr << "[RTMPOutput] RTMP connection may have been lost" << std::endl;
        } else {
            std::cerr << "[RTMPOutput] ✗ Partial write: " << written << "/" << ts::PKT_SIZE << " bytes" << std::endl;
        }
        
        // Connection likely lost
        if (rtmp_connected_.load()) {
            rtmp_connected_ = false;
            std::cerr << "[RTMPOutput] ========================================" << std::endl;
            std::cerr << "[RTMPOutput] RTMP DISCONNECTED" << std::endl;
            std::cerr << "[RTMPOutput] ========================================" << std::endl;
        }
        
        return false;
    }
    
    packets_written_++;
    last_write_time_ = std::chrono::steady_clock::now();
    
    // Simple pacing: Sleep a bit to avoid overwhelming FFmpeg
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    
    return true;
}

bool RTMPOutput::spawnFFmpeg() {
    std::cout << "[RTMPOutput] ========================================" << std::endl;
    std::cout << "[RTMPOutput] Starting RTMP Output" << std::endl;
    std::cout << "[RTMPOutput] ========================================" << std::endl;
    std::cout << "[RTMPOutput] Target URL: " << rtmp_url_ << std::endl;
    std::cout << "[RTMPOutput] Initializing FFmpeg process..." << std::endl;
    
    // Create pipes for stdin and stderr
    if (pipe(stdin_pipe_) < 0) {
        std::cerr << "[RTMPOutput] ✗ Failed to create stdin pipe: " << strerror(errno) << std::endl;
        return false;
    }
    
    if (pipe(stderr_pipe_) < 0) {
        std::cerr << "[RTMPOutput] ✗ Failed to create stderr pipe: " << strerror(errno) << std::endl;
        close(stdin_pipe_[0]);
        close(stdin_pipe_[1]);
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
    
    return true;
}

void RTMPOutput::closeFFmpeg() {
    // Close pipes
    if (stdin_pipe_[1] >= 0) {
        close(stdin_pipe_[1]);
        stdin_pipe_[1] = -1;
    }
    
    if (stderr_pipe_[0] >= 0) {
        close(stderr_pipe_[0]);
        stderr_pipe_[0] = -1;
    }
    
    // Kill FFmpeg process
    if (ffmpeg_pid_ > 0) {
        kill(ffmpeg_pid_, SIGTERM);
        
        // Wait for process to exit
        int status;
        waitpid(ffmpeg_pid_, &status, 0);
        
        ffmpeg_pid_ = -1;
    }
    
    rtmp_connected_ = false;
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
        !rtmp_connected_.load()) {
        rtmp_connected_ = true;
        std::cout << "[RTMPOutput] ========================================" << std::endl;
        std::cout << "[RTMPOutput] ✓ RTMP CONNECTION ESTABLISHED" << std::endl;
        std::cout << "[RTMPOutput] ========================================" << std::endl;
        std::cout << "[RTMPOutput] Streaming to nginx-rtmp server" << std::endl;
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
        std::cerr << "[RTMPOutput] FFmpeg: " << line << std::endl;
        
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
        
        rtmp_connected_ = false;
    }
    
    // Log other diagnostic information (but not every progress line)
    if (lower_line.find("frame=") == std::string::npos &&  // Skip frame progress
        lower_line.find("size=") == std::string::npos) {     // Skip size progress
        // Log configuration and important messages
        if (lower_line.find("input") != std::string::npos ||
            lower_line.find("output") != std::string::npos ||
            lower_line.find("stream") != std::string::npos ||
            lower_line.find("duration") != std::string::npos) {
            std::cout << "[RTMPOutput] FFmpeg: " << line << std::endl;
        }
    }
}