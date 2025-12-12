#pragma once

#include "Config.h"
#include "TSPacketQueue.h"
#include "TCPReceiver.h"
#include "RTMPReceiver.h"
#include "InputSourceManager.h"
#include "TSAnalyzer.h"
#include "TimestampManager.h"
#include "PIDMapper.h"
#include "StreamSwitcher.h"
#include "RTMPOutput.h"
#include "HttpClient.h"
#include "HttpServer.h"
#include "NALParser.h"
#include "SPSPPSInjector.h"
#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>

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
    
    // Switch input source (camera/drone) at runtime
    // Returns true if switch was successful
    bool switchInputSource(InputSource new_source);
    
    // Request input source switch (thread-safe, called from HTTP handler)
    // Sets pending flag to be processed in main loop
    void requestInputSourceSwitch(InputSource new_source);
    
    // IDR-aware switch to live - buffers packets until IDR found
    // Returns true if switch was successful, false if timeout
    bool switchToLiveAtIDR();
    
    // IDR-aware switch to fallback - buffers packets until IDR found
    // Returns true if switch was successful, false if timeout
    bool switchToFallbackAtIDR();
    
    // Drain buffered packets to output starting from IDR
    // If needs_sps_pps_injection is true, injects stored SPS/PPS before IDR
    void drainBufferFromIDR(std::vector<ts::TSPacket>& buffer, size_t idr_index,
                            Source source, bool needs_sps_pps_injection = false);
    
    // Inject SPS/PPS packets before IDR frame (gets data from analyzer's NALParser)
    // Returns number of packets injected
    size_t injectSPSPPS(Source source, uint16_t video_pid,
                        std::optional<uint64_t> pts, std::optional<uint64_t> dts);
    
    // Inject SPS/PPS packets before IDR frame (uses provided SPS/PPS data directly)
    // Returns number of packets injected
    size_t injectSPSPPS(Source source, uint16_t video_pid,
                        const std::vector<uint8_t>& sps,
                        const std::vector<uint8_t>& pps,
                        std::optional<uint64_t> pts, std::optional<uint64_t> dts);
    
    // Inject PAT/PMT tables at splice points for decoder stream configuration update
    // Per splice.md: "Emit a new PAT/PMT at the splice" and "ensure tables repeat a few times for safety"
    // Returns the total number of packets injected
    size_t injectPATMT(Source source, int repetitions = 3);
    
    // Query initial privacy mode from controller
    void queryInitialPrivacyMode();
    
    // Handle privacy mode callback from controller
    void onPrivacyModeChange(bool enabled);
    
    // Notify controller of initial scene with retry logic
    void notifyInitialScene();
    
    const Config& config_;
    
    // HTTP client for controller communication
    std::shared_ptr<HttpClient> http_client_;
    
    // HTTP server for receiving callbacks
    std::unique_ptr<HttpServer> http_server_;
    
    // Input source manager
    std::shared_ptr<InputSourceManager> input_source_manager_;
    
    // TCP Receivers (TCPReceiver has internal rolling buffer)
    std::unique_ptr<TCPReceiver> camera_tcp_receiver_;      // Camera input via TCP (from ffmpeg-srt-live)
    std::unique_ptr<TCPReceiver> drone_tcp_receiver_;       // Drone input via TCP (from ffmpeg-rtmp-live)
    std::unique_ptr<TCPReceiver> fallback_tcp_receiver_;    // Fallback input via TCP
    
    // Stream analyzers
    std::unique_ptr<TSAnalyzer> live_analyzer_;
    std::unique_ptr<TSAnalyzer> fallback_analyzer_;
    
    // Processing components
    std::unique_ptr<TimestampManager> timestamp_mgr_;
    std::unique_ptr<PIDMapper> pid_mapper_;
    std::unique_ptr<StreamSwitcher> switcher_;
    
    // Output
    std::unique_ptr<RTMPOutput> rtmp_output_;
    
    // SPS/PPS injector for splice points
    std::unique_ptr<SPSPPSInjector> sps_pps_injector_;
    
    // Control
    std::atomic<bool> running_;
    std::atomic<bool> initialized_;
    std::atomic<bool> live_stream_ready_;
    std::atomic<bool> initial_privacy_mode_;  // Privacy mode queried on startup
    
    // Statistics
    std::atomic<uint64_t> packets_processed_;
    std::chrono::steady_clock::time_point start_time_;
    
    // Timestamp tracking for tcp_main.cpp-style rebasing
    uint64_t current_pts_base_ = 0;
    uint64_t current_pcr_base_ = 0;
    int64_t current_pcr_pts_alignment_ = 0;
    
    // Controller URL
    static constexpr const char* CONTROLLER_URL = "http://controller:8089";
    static constexpr uint16_t HTTP_SERVER_PORT = 8091;
    
    // IDR wait timeouts (configurable via Config)
    uint32_t live_idr_timeout_ms_;     // Loaded from config
    uint32_t fallback_idr_timeout_ms_; // Loaded from config
    
    // Current input source tracking
    InputSource current_input_source_ = InputSource::CAMERA;
    
    // Pending input source switch (thread-safe communication from HTTP handler to main loop)
    std::atomic<bool> input_source_change_pending_{false};
    std::atomic<InputSource> pending_input_source_{InputSource::CAMERA};
    
    // Pending privacy mode switch (thread-safe communication from HTTP handler to main loop)
    std::atomic<bool> privacy_mode_change_pending_{false};
    std::atomic<bool> pending_privacy_enabled_{false};
};