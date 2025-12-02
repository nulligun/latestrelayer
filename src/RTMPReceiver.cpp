#include "RTMPReceiver.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>

RTMPReceiver::RTMPReceiver(const std::string& name, const std::string& rtmp_url, TSPacketQueue& queue)
    : name_(name),
      rtmp_url_(rtmp_url),
      queue_(queue),
      running_(false),
      should_stop_(false),
      connected_(false),
      ffmpeg_pid_(-1),
      ffmpeg_stdout_fd_(-1),
      packets_received_(0),
      packets_dropped_(0),
      bytes_received_(0) {
}

RTMPReceiver::~RTMPReceiver() {
    stop();
}

bool RTMPReceiver::start() {
    if (running_.load()) {
        std::cerr << "[" << name_ << "] Already running" << std::endl;
        return false;
    }
    
    if (!startFFmpeg()) {
        return false;
    }
    
    should_stop_ = false;
    receiver_thread_ = std::thread(&RTMPReceiver::receiveLoop, this);
    
    std::cout << "[" << name_ << "] Started RTMP receiver for URL: " << rtmp_url_ << std::endl;
    return true;
}

void RTMPReceiver::stop() {
    if (!running_.load() && !receiver_thread_.joinable()) {
        return;
    }
    
    auto shutdown_start = std::chrono::steady_clock::now();
    std::cout << "[" << name_ << "] Stopping..." << std::endl;
    
    // Set stop flag first
    should_stop_ = true;
    
    // Stop FFmpeg process
    stopFFmpeg();
    
    // Wait for thread to finish
    if (receiver_thread_.joinable()) {
        receiver_thread_.join();
        auto shutdown_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - shutdown_start
        );
        std::cout << "[" << name_ << "] Thread joined (" << shutdown_duration.count() << "ms)" << std::endl;
    }
    
    std::cout << "[" << name_ << "] Stopped. Bytes: "
              << bytes_received_.load() << ", TS packets: "
              << packets_received_.load() << ", dropped: "
              << packets_dropped_.load() << std::endl;
}

bool RTMPReceiver::startFFmpeg() {
    // Create pipe for FFmpeg stdout
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        std::cerr << "[" << name_ << "] Failed to create pipe: " << strerror(errno) << std::endl;
        return false;
    }
    
    ffmpeg_pid_ = fork();
    
    if (ffmpeg_pid_ < 0) {
        std::cerr << "[" << name_ << "] Failed to fork: " << strerror(errno) << std::endl;
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    
    if (ffmpeg_pid_ == 0) {
        // Child process - exec FFmpeg
        
        // Close read end of pipe
        close(pipefd[0]);
        
        // Redirect stdout to pipe
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        
        // Redirect stderr to /dev/null to avoid noise (or could redirect to a log)
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        
        // Build FFmpeg command
        // FFmpeg pulls from RTMP and outputs MPEG-TS to stdout
        // -re: Read input at native frame rate (important for live streams)
        // -i: Input URL
        // -c copy: Stream copy (no re-encoding)
        // -f mpegts: Output format MPEG-TS
        // -: Output to stdout
        execlp("ffmpeg", "ffmpeg",
               "-loglevel", "warning",
               "-i", rtmp_url_.c_str(),
               "-c", "copy",
               "-f", "mpegts",
               "-",
               nullptr);
        
        // If exec fails
        std::cerr << "[" << name_ << "] Failed to exec ffmpeg: " << strerror(errno) << std::endl;
        _exit(1);
    }
    
    // Parent process
    
    // Close write end of pipe
    close(pipefd[1]);
    
    // Store read end
    ffmpeg_stdout_fd_ = pipefd[0];
    
    // Set non-blocking mode for easier shutdown
    int flags = fcntl(ffmpeg_stdout_fd_, F_GETFL, 0);
    fcntl(ffmpeg_stdout_fd_, F_SETFL, flags | O_NONBLOCK);
    
    std::cout << "[" << name_ << "] FFmpeg started with PID " << ffmpeg_pid_ << std::endl;
    return true;
}

void RTMPReceiver::stopFFmpeg() {
    if (ffmpeg_pid_ > 0) {
        std::cout << "[" << name_ << "] Stopping FFmpeg process (PID " << ffmpeg_pid_ << ")..." << std::endl;
        
        // Send SIGTERM first
        kill(ffmpeg_pid_, SIGTERM);
        
        // Wait up to 2 seconds for graceful shutdown
        int status;
        int wait_ms = 0;
        while (wait_ms < 2000) {
            pid_t result = waitpid(ffmpeg_pid_, &status, WNOHANG);
            if (result == ffmpeg_pid_) {
                std::cout << "[" << name_ << "] FFmpeg exited gracefully" << std::endl;
                ffmpeg_pid_ = -1;
                break;
            }
            usleep(100000); // 100ms
            wait_ms += 100;
        }
        
        // If still running, force kill
        if (ffmpeg_pid_ > 0) {
            std::cout << "[" << name_ << "] Force killing FFmpeg..." << std::endl;
            kill(ffmpeg_pid_, SIGKILL);
            waitpid(ffmpeg_pid_, &status, 0);
            ffmpeg_pid_ = -1;
        }
    }
    
    // Close pipe
    if (ffmpeg_stdout_fd_ >= 0) {
        close(ffmpeg_stdout_fd_);
        ffmpeg_stdout_fd_ = -1;
    }
    
    connected_ = false;
}

void RTMPReceiver::receiveLoop() {
    running_ = true;
    
    // Buffer for reading from FFmpeg stdout
    // We read in larger chunks and then extract TS packets
    const size_t READ_BUFFER_SIZE = 8192;  // 8KB read buffer
    uint8_t read_buffer[READ_BUFFER_SIZE];
    
    // Accumulator buffer for partial TS packets
    const size_t PACKET_BUFFER_SIZE = ts::PKT_SIZE * 100;  // Room for 100 packets
    uint8_t packet_buffer[PACKET_BUFFER_SIZE];
    size_t packet_buffer_len = 0;
    
    std::cout << "[" << name_ << "] Receive loop started" << std::endl;
    
    // Stats tracking
    uint64_t stats_interval = 10000;  // Log stats every 10000 packets
    auto last_data_time = std::chrono::steady_clock::now();
    bool first_data_received = false;
    
    while (!should_stop_.load()) {
        // Use poll to wait for data with timeout
        struct pollfd pfd;
        pfd.fd = ffmpeg_stdout_fd_;
        pfd.events = POLLIN;
        
        int ret = poll(&pfd, 1, 500);  // 500ms timeout
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[" << name_ << "] Poll error: " << strerror(errno) << std::endl;
            break;
        }
        
        if (ret == 0) {
            // Timeout - check if FFmpeg is still running
            if (ffmpeg_pid_ > 0) {
                int status;
                pid_t result = waitpid(ffmpeg_pid_, &status, WNOHANG);
                if (result == ffmpeg_pid_) {
                    std::cout << "[" << name_ << "] FFmpeg process exited" << std::endl;
                    ffmpeg_pid_ = -1;
                    connected_ = false;
                    break;
                }
            }
            
            // Log if we haven't received data in a while
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_data_time).count();
            if (first_data_received && elapsed > 5) {
                std::cout << "[" << name_ << "] No data received for " << elapsed << " seconds" << std::endl;
                last_data_time = now;  // Reset to avoid spam
            }
            
            continue;
        }
        
        // Data available - read from FFmpeg
        ssize_t bytes_read = read(ffmpeg_stdout_fd_, read_buffer, READ_BUFFER_SIZE);
        
        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            std::cerr << "[" << name_ << "] Read error: " << strerror(errno) << std::endl;
            break;
        }
        
        if (bytes_read == 0) {
            // EOF - FFmpeg closed stdout
            std::cout << "[" << name_ << "] FFmpeg closed stdout (EOF)" << std::endl;
            connected_ = false;
            break;
        }
        
        // Update stats
        bytes_received_ += bytes_read;
        last_data_time = std::chrono::steady_clock::now();
        
        if (!first_data_received) {
            first_data_received = true;
            connected_ = true;
            std::cout << "[" << name_ << "] First data received - RTMP stream connected!" << std::endl;
        }
        
        // Append to packet buffer
        size_t bytes_to_copy = std::min(static_cast<size_t>(bytes_read), 
                                        PACKET_BUFFER_SIZE - packet_buffer_len);
        memcpy(packet_buffer + packet_buffer_len, read_buffer, bytes_to_copy);
        packet_buffer_len += bytes_to_copy;
        
        if (bytes_to_copy < static_cast<size_t>(bytes_read)) {
            std::cerr << "[" << name_ << "] Warning: Packet buffer overflow, discarding "
                      << (bytes_read - bytes_to_copy) << " bytes" << std::endl;
        }
        
        // Extract complete TS packets from buffer
        size_t offset = 0;
        while (offset + ts::PKT_SIZE <= packet_buffer_len) {
            // Find sync byte
            while (offset < packet_buffer_len && packet_buffer[offset] != ts::SYNC_BYTE) {
                offset++;
            }
            
            if (offset + ts::PKT_SIZE > packet_buffer_len) {
                // Not enough data for a complete packet
                break;
            }
            
            // Validate this is a real packet by checking sync byte of next packet (if available)
            bool valid_packet = true;
            if (offset + ts::PKT_SIZE + 1 <= packet_buffer_len) {
                if (packet_buffer[offset + ts::PKT_SIZE] != ts::SYNC_BYTE) {
                    // Next byte is not a sync byte - this might be corrupted data
                    // Skip this byte and try again
                    offset++;
                    continue;
                }
            }
            
            if (valid_packet) {
                // Create TS packet and push to queue
                ts::TSPacket packet;
                memcpy(packet.b, packet_buffer + offset, ts::PKT_SIZE);
                
                if (!queue_.push(packet)) {
                    packets_dropped_++;
                } else {
                    packets_received_++;
                }
                
                offset += ts::PKT_SIZE;
            }
        }
        
        // Move remaining data to beginning of buffer
        if (offset > 0 && offset < packet_buffer_len) {
            memmove(packet_buffer, packet_buffer + offset, packet_buffer_len - offset);
            packet_buffer_len -= offset;
        } else if (offset >= packet_buffer_len) {
            packet_buffer_len = 0;
        }
        
        // Periodic statistics logging
        if (packets_received_ > 0 && packets_received_ % stats_interval == 0) {
            std::cout << "[" << name_ << "] Stats: Bytes: " << bytes_received_.load()
                      << ", TS packets: " << packets_received_.load()
                      << ", Dropped: " << packets_dropped_.load() << std::endl;
        }
    }
    
    running_ = false;
    connected_ = false;
    std::cout << "[" << name_ << "] Receive loop ended" << std::endl;
}