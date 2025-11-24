#pragma once

#include "Config.h"
#include "TSPacketQueue.h"
#include "UDPReceiver.h"
#include "TSAnalyzer.h"
#include "TimestampManager.h"
#include "PIDMapper.h"
#include "StreamSwitcher.h"
#include "RTMPOutput.h"
#include <memory>
#include <atomic>
#include <thread>

class Multiplexer {
public:
    Multiplexer(const Config& config);
    ~Multiplexer();
    
    // Initialize and start multiplexer
    bool initialize();
    
    // Run the main multiplexing loop
    void run();
    
    // Stop the multiplexer
    void stop();
    
private:
    // Analyze initial packets from both sources
    bool analyzeStreams();
    
    // Dynamically analyze live stream when it becomes available
    bool analyzeLiveStreamDynamically();
    
    // Main processing loop
    void processLoop();
    
    // Process a single packet
    void processPacket(ts::TSPacket& packet, Source source);
    
    const Config& config_;
    
    // Packet queues
    std::unique_ptr<TSPacketQueue> live_queue_;
    std::unique_ptr<TSPacketQueue> fallback_queue_;
    
    // UDP receivers
    std::unique_ptr<UDPReceiver> live_receiver_;
    std::unique_ptr<UDPReceiver> fallback_receiver_;
    
    // Stream analyzers
    std::unique_ptr<TSAnalyzer> live_analyzer_;
    std::unique_ptr<TSAnalyzer> fallback_analyzer_;
    
    // Processing components
    std::unique_ptr<TimestampManager> timestamp_mgr_;
    std::unique_ptr<PIDMapper> pid_mapper_;
    std::unique_ptr<StreamSwitcher> switcher_;
    
    // Output
    std::unique_ptr<RTMPOutput> rtmp_output_;
    
    // Control
    std::atomic<bool> running_;
    std::atomic<bool> initialized_;
    std::atomic<bool> live_stream_ready_;
    
    // Statistics
    std::atomic<uint64_t> packets_processed_;
    std::chrono::steady_clock::time_point start_time_;
};