#include "FIFOOutput.h"
#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <thread>
#include <chrono>

FIFOOutput::FIFOOutput(const std::string& pipe_path, const std::atomic<bool>& running)
    : pipe_path_(pipe_path),
      fd_(-1),
      running_(running),
      packets_written_(0),
      bytes_written_(0) {
}

FIFOOutput::~FIFOOutput() {
    close();
}

bool FIFOOutput::open() {
    if (fd_ >= 0) {
        std::cout << "[FIFOOutput] Pipe already open" << std::endl;
        return true;
    }
    
    std::cout << "[FIFOOutput] Opening named pipe: " << pipe_path_ << std::endl;
    
    // Verify pipe exists
    struct stat st;
    if (stat(pipe_path_.c_str(), &st) != 0) {
        std::cerr << "[FIFOOutput] Pipe does not exist: " << pipe_path_ 
                  << " (" << strerror(errno) << ")" << std::endl;
        return false;
    }
    
    if (!S_ISFIFO(st.st_mode)) {
        std::cerr << "[FIFOOutput] Path exists but is not a FIFO: " << pipe_path_ << std::endl;
        return false;
    }
    
    // Open the FIFO for writing (blocks until a reader opens it)
    std::cout << "[FIFOOutput] Waiting for FFmpeg to open pipe for reading..." << std::endl;
    fd_ = ::open(pipe_path_.c_str(), O_WRONLY);
    
    if (fd_ < 0) {
        std::cerr << "[FIFOOutput] Failed to open pipe: " << strerror(errno) << std::endl;
        return false;
    }
    
    std::cout << "[FIFOOutput] Pipe opened successfully (fd=" << fd_ << ")" << std::endl;
    
    // Try to increase pipe buffer size for better performance
    // This reduces the chance of blocking during high bitrate bursts
#ifdef F_SETPIPE_SZ
    int result = fcntl(fd_, F_SETPIPE_SZ, PIPE_BUFFER_SIZE);
    if (result < 0) {
        std::cerr << "[FIFOOutput] Warning: Failed to set pipe buffer size: " 
                  << strerror(errno) << std::endl;
        std::cout << "[FIFOOutput] Using default system pipe buffer" << std::endl;
    } else {
        std::cout << "[FIFOOutput] Pipe buffer size set to " << result << " bytes" << std::endl;
    }
#else
    std::cout << "[FIFOOutput] F_SETPIPE_SZ not available, using default pipe buffer" << std::endl;
#endif
    
    return true;
}

void FIFOOutput::close() {
    if (fd_ >= 0) {
        std::cout << "[FIFOOutput] Closing pipe..." << std::endl;
        ::close(fd_);
        fd_ = -1;
    }
    
    std::cout << "[FIFOOutput] Statistics:" << std::endl;
    std::cout << "  Packets written: " << packets_written_.load() << std::endl;
    std::cout << "  Bytes written: " << bytes_written_.load() << std::endl;
}

bool FIFOOutput::writePacket(const ts::TSPacket& packet) {
    // Check if pipe is open
    if (fd_ < 0) {
        std::cerr << "[FIFOOutput] Pipe not open, attempting to open..." << std::endl;
        if (!open()) {
            return false;
        }
    }
    
    // Write 188-byte TS packet
    // This is a blocking write - it will wait if the pipe buffer is full
    ssize_t written = write(fd_, packet.b, ts::PKT_SIZE);
    
    if (written != ts::PKT_SIZE) {
        int err = errno;
        
        // EPIPE means the reader closed the pipe (ffmpeg crashed/restarted)
        if (err == EPIPE) {
            std::cerr << "[FIFOOutput] Broken pipe (reader disconnected) - FFmpeg likely restarting" << std::endl;
            std::cout << "[FIFOOutput] Closing and reopening pipe..." << std::endl;
            
            // Close the pipe
            ::close(fd_);
            fd_ = -1;
            
            // Wait a moment for FFmpeg to restart
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            // Try to reopen (will block until FFmpeg opens for reading)
            if (!open()) {
                std::cerr << "[FIFOOutput] Failed to reopen pipe" << std::endl;
                return false;
            }
            
            // Packet was lost during reconnection
            std::cout << "[FIFOOutput] Pipe reopened - packet dropped during reconnection" << std::endl;
            return false;
        }
        
        // Other write errors
        std::cerr << "[FIFOOutput] Write failed - expected " << ts::PKT_SIZE
                  << " bytes, wrote " << written << " bytes, errno=" << err
                  << " (" << strerror(err) << ")" << std::endl;
        return false;
    }
    
    packets_written_++;
    bytes_written_ += ts::PKT_SIZE;
    
    return true;
}

bool FIFOOutput::writePackets(const std::vector<ts::TSPacket>& packets) {
    for (const auto& packet : packets) {
        if (!writePacket(packet)) {
            return false;
        }
    }
    return true;
}
