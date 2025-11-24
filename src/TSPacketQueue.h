#pragma once

#include <tsduck.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>

class TSPacketQueue {
public:
    TSPacketQueue(size_t max_size = 10000);
    ~TSPacketQueue();
    
    // Push a packet to the queue (non-blocking)
    // Returns false if queue is full
    bool push(const ts::TSPacket& packet);
    
    // Pop a packet from the queue (blocking with timeout)
    // Returns true if a packet was retrieved, false on timeout
    bool pop(ts::TSPacket& packet, std::chrono::milliseconds timeout);
    
    // Get current queue size
    size_t size() const;
    
    // Clear all packets
    void clear();
    
    // Check if queue is empty
    bool empty() const;
    
private:
    std::queue<ts::TSPacket> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    size_t max_size_;
    std::atomic<size_t> current_size_;
};