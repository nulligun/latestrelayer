#ifndef TCP_OUTPUT_H
#define TCP_OUTPUT_H

#include <string>
#include <atomic>
#include <cstdint>
#include <tsduck.h>

/**
 * TCPOutput - Simple TCP client for writing TS packets
 * 
 * Connects to FFmpeg TCP server (listen mode) and writes MPEG-TS packets.
 * Uses 2MB send buffer matching ffmpeg-fallback settings.
 */
class TCPOutput {
public:
    TCPOutput(const std::string& host, uint16_t port, const std::atomic<bool>& running);
    ~TCPOutput();
    
    // Connect to FFmpeg TCP server
    bool connect();
    
    // Disconnect and cleanup
    void disconnect();
    
    // Write a single TS packet
    bool writePacket(const ts::TSPacket& packet);
    
    // Write multiple TS packets
    bool writePackets(const std::vector<ts::TSPacket>& packets);
    
    // Connection status
    bool isConnected() const { return connected_.load(); }
    
    // Statistics
    uint64_t getPacketsWritten() const { return packets_written_.load(); }
    uint64_t getBytesWritten() const { return bytes_written_.load(); }
    
private:
    std::string host_;
    uint16_t port_;
    int sockfd_;
    const std::atomic<bool>& running_;
    
    std::atomic<bool> connected_;
    std::atomic<uint64_t> packets_written_;
    std::atomic<uint64_t> bytes_written_;
    
    static constexpr int TCP_SEND_BUFFER_SIZE = 2 * 1024 * 1024;  // 2MB
    static constexpr int TCP_RECONNECT_DELAY_MS = 2000;  // 2 seconds
};

#endif // TCP_OUTPUT_H