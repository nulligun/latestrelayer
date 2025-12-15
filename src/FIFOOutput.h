#ifndef FIFO_OUTPUT_H
#define FIFO_OUTPUT_H

#include <string>
#include <atomic>
#include <cstdint>
#include <tsduck.h>

/**
 * FIFOOutput - Named pipe (FIFO) writer for TS packets
 * 
 * Writes MPEG-TS packets to a named pipe for consumption by FFmpeg.
 * Blocking writes ensure no packet drops (pipe blocks when full).
 * Automatically increases pipe buffer size to 1MB for better performance.
 */
class FIFOOutput {
public:
    FIFOOutput(const std::string& pipe_path, const std::atomic<bool>& running);
    ~FIFOOutput();
    
    // Open the named pipe for writing (blocks until reader connects)
    bool open();
    
    // Close the pipe
    void close();
    
    // Write a single TS packet (blocks if pipe is full)
    bool writePacket(const ts::TSPacket& packet);
    
    // Write multiple TS packets
    bool writePackets(const std::vector<ts::TSPacket>& packets);
    
    // Check if pipe is open
    bool isOpen() const { return fd_ >= 0; }
    
    // Statistics
    uint64_t getPacketsWritten() const { return packets_written_.load(); }
    uint64_t getBytesWritten() const { return bytes_written_.load(); }
    
private:
    std::string pipe_path_;
    int fd_;
    const std::atomic<bool>& running_;
    
    std::atomic<uint64_t> packets_written_;
    std::atomic<uint64_t> bytes_written_;
    
    static constexpr int PIPE_BUFFER_SIZE = 1 * 1024 * 1024;  // 1MB
};

#endif // FIFO_OUTPUT_H
