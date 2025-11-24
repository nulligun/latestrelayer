#include "TSPacketQueue.h"
#include <iostream>

TSPacketQueue::TSPacketQueue(size_t max_size)
    : max_size_(max_size), current_size_(0) {
}

TSPacketQueue::~TSPacketQueue() {
    clear();
}

bool TSPacketQueue::push(const ts::TSPacket& packet) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // Check if queue is full
    if (queue_.size() >= max_size_) {
        // Drop oldest packet to make room (prevent unbounded growth)
        queue_.pop();
        std::cerr << "Warning: Queue full, dropping oldest packet" << std::endl;
    }
    
    queue_.push(packet);
    current_size_ = queue_.size();
    
    // Notify one waiting thread
    cv_.notify_one();
    
    return true;
}

bool TSPacketQueue::pop(ts::TSPacket& packet, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // Wait for packet or timeout
    if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
        // Timeout occurred
        return false;
    }
    
    // Get packet from queue
    packet = queue_.front();
    queue_.pop();
    current_size_ = queue_.size();
    
    return true;
}

size_t TSPacketQueue::size() const {
    return current_size_.load();
}

void TSPacketQueue::clear() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
        queue_.pop();
    }
    current_size_ = 0;
}

bool TSPacketQueue::empty() const {
    return current_size_.load() == 0;
}