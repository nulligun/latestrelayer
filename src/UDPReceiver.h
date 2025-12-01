#pragma once

#include <tsduck.h>
#include "TSPacketQueue.h"
#include <thread>
#include <atomic>
#include <cstdint>
#include <string>

class UDPReceiver {
public:
    // Constructor with configurable receive buffer size
    // buffer_size: SO_RCVBUF size in bytes (default 262144 = 256KB)
    UDPReceiver(const std::string& name, uint16_t port, TSPacketQueue& queue,
                uint32_t buffer_size = 262144);
    ~UDPReceiver();
    
    // Start receiving on the specified port
    bool start();
    
    // Stop receiving and join thread
    void stop();
    
    // Check if receiver is running
    bool isRunning() const { return running_.load(); }
    
    // Get statistics
    uint64_t getPacketsReceived() const { return packets_received_.load(); }
    uint64_t getPacketsDropped() const { return packets_dropped_.load(); }
    uint64_t getDatagramsReceived() const { return datagrams_received_.load(); }
    
private:
    // Thread function for receiving packets
    void receiveLoop();
    
    // Create and bind UDP socket
    bool createSocket();
    
    // Close socket
    void closeSocket();
    
    std::string name_;
    uint16_t port_;
    TSPacketQueue& queue_;
    uint32_t buffer_size_;  // SO_RCVBUF size in bytes
    
    std::thread receiver_thread_;
    std::atomic<bool> running_;
    std::atomic<bool> should_stop_;
    
    int socket_fd_;
    
    // Statistics
    std::atomic<uint64_t> packets_received_;
    std::atomic<uint64_t> packets_dropped_;
    std::atomic<uint64_t> datagrams_received_;
};