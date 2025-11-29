#pragma once

#include "Config.h"
#include "TSPacketQueue.h"
#include "UDPReceiver.h"
#include "TSAnalyzer.h"
#include "TimestampManager.h"
#include "PIDMapper.h"
#include "StreamSwitcher.h"
#include "RTMPOutput.h"
#include "HttpClient.h"
#include "HttpServer.h"
#include <memory>
#include <atomic>
#include <thread>
#include <string>

// Input source types
enum class InputSource {
    CAMERA,  // SRT input on port 10000
    DRONE    // RTMP input on port 10002
};

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
    
    // Dynamically analyze drone stream when it becomes available
    bool analyzeDroneStreamDynamically();
    
    // Main processing loop
    void processLoop();
    
    // Process a single packet
    void processPacket(ts::TSPacket& packet, Source source);
    
    // Query initial privacy mode from controller
    void queryInitialPrivacyMode();
    
    // Handle privacy mode callback from controller
    void onPrivacyModeChange(bool enabled);
    
    // Handle input source change callback from HTTP endpoint
    void onInputSourceChange(const std::string& source);
    
    // Notify controller of initial scene with retry logic
    void notifyInitialScene();
    
    // Load input source from persistent storage
    void loadInputSource();
    
    // Save input source to persistent storage
    void saveInputSource();
    
    // Get the active live queue based on current input source
    TSPacketQueue* getActiveLiveQueue();
    
    // Get the active live analyzer based on current input source
    TSAnalyzer* getActiveLiveAnalyzer();
    
    // Check if the active live stream is ready
    bool isActiveLiveStreamReady();
    
    const Config& config_;
    
    // HTTP client for controller communication
    std::shared_ptr<HttpClient> http_client_;
    
    // HTTP server for receiving callbacks
    std::unique_ptr<HttpServer> http_server_;
    
    // Packet queues (camera = live, drone = drone)
    std::unique_ptr<TSPacketQueue> live_queue_;      // Camera/SRT input
    std::unique_ptr<TSPacketQueue> fallback_queue_;
    std::unique_ptr<TSPacketQueue> drone_queue_;     // Drone/RTMP input
    
    // UDP receivers
    std::unique_ptr<UDPReceiver> live_receiver_;     // Camera/SRT input
    std::unique_ptr<UDPReceiver> fallback_receiver_;
    std::unique_ptr<UDPReceiver> drone_receiver_;    // Drone/RTMP input
    
    // Stream analyzers
    std::unique_ptr<TSAnalyzer> live_analyzer_;      // Camera/SRT input
    std::unique_ptr<TSAnalyzer> fallback_analyzer_;
    std::unique_ptr<TSAnalyzer> drone_analyzer_;     // Drone/RTMP input
    
    // Processing components
    std::unique_ptr<TimestampManager> timestamp_mgr_;
    std::unique_ptr<PIDMapper> pid_mapper_;
    std::unique_ptr<StreamSwitcher> switcher_;
    
    // Output
    std::unique_ptr<RTMPOutput> rtmp_output_;
    
    // Control
    std::atomic<bool> running_;
    std::atomic<bool> initialized_;
    std::atomic<bool> live_stream_ready_;            // Camera stream ready
    std::atomic<bool> drone_stream_ready_;           // Drone stream ready
    std::atomic<bool> initial_privacy_mode_;         // Privacy mode queried on startup
    std::atomic<InputSource> current_input_source_;  // Currently selected input source
    
    // Statistics
    std::atomic<uint64_t> packets_processed_;
    std::chrono::steady_clock::time_point start_time_;
    
    // Controller URL
    static constexpr const char* CONTROLLER_URL = "http://controller:8089";
    static constexpr uint16_t HTTP_SERVER_PORT = 8091;
    static constexpr const char* INPUT_SOURCE_FILE = "/app/shared/input_source.json";
};