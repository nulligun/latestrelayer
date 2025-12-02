#include "Multiplexer.h"
#include "SPSPPSInjector.h"
#include <iostream>
#include <thread>
#include <csignal>

Multiplexer::Multiplexer(const Config& config)
    : config_(config),
      http_client_(std::make_shared<HttpClient>(CONTROLLER_URL)),
      running_(false),
      initialized_(false),
      camera_stream_ready_(false),
      drone_stream_ready_(false),
      initial_privacy_mode_(false),
      input_switching_(false),
      pending_input_switch_(InputSource::CAMERA),
      packets_processed_(0),
      live_idr_timeout_ms_(config.getLiveIdrTimeoutMs()),
      fallback_idr_timeout_ms_(config.getFallbackIdrTimeoutMs()),
      input_switch_idr_timeout_ms_(config.getInputSwitchIdrTimeoutMs()) {
    std::cout << "[Multiplexer] Configured with IDR timeouts: live=" << live_idr_timeout_ms_
              << "ms, fallback=" << fallback_idr_timeout_ms_ << "ms"
              << ", input_switch=" << input_switch_idr_timeout_ms_ << "ms" << std::endl;
}

Multiplexer::~Multiplexer() {
    stop();
    
    // Stop HTTP server if running
    if (http_server_) {
        http_server_->stop();
    }
}

bool Multiplexer::initialize() {
    if (initialized_.load()) {
        std::cout << "[Multiplexer] Already initialized" << std::endl;
        return true;
    }
    
    // Set running flag early so shutdown can be detected during initialization
    running_ = true;
    
    std::cout << "[Multiplexer] Initializing..." << std::endl;
    
    // Initialize input source manager and load saved state
    input_source_manager_ = std::make_shared<InputSourceManager>(config_.getInputSourceFile());
    if (!input_source_manager_->load()) {
        std::cerr << "[Multiplexer] Warning: Failed to load input source state, using default (camera)" << std::endl;
    }
    std::cout << "[Multiplexer] Input source: " << input_source_manager_->getInputSourceString() << std::endl;
    
    // Start HTTP server for receiving callbacks (privacy mode changes, health status, input source)
    http_server_ = std::make_unique<HttpServer>(HTTP_SERVER_PORT);
    http_server_->setPrivacyCallback([this](bool enabled) {
        onPrivacyModeChange(enabled);
    });
    http_server_->setInputSourceManager(input_source_manager_);
    
    // Note: Health callback will be set after rtmp_output_ is created
    
    if (!http_server_->start()) {
        std::cerr << "[Multiplexer] Failed to start HTTP server on port " << HTTP_SERVER_PORT << std::endl;
        // Non-fatal - continue without callback server
        std::cout << "[Multiplexer] Continuing without privacy callback server" << std::endl;
    } else {
        std::cout << "[Multiplexer] HTTP server started on port " << HTTP_SERVER_PORT << std::endl;
    }
    
    // Query initial privacy mode from controller
    queryInitialPrivacyMode();
    
    // Create packet queues with configurable size - separate queues for camera, drone, and fallback
    uint32_t queue_size = config_.getTsQueueSize();
    std::cout << "[Multiplexer] Creating packet queues with size=" << queue_size << " (camera, drone, fallback)" << std::endl;
    camera_queue_ = std::make_unique<TSPacketQueue>(queue_size);
    drone_queue_ = std::make_unique<TSPacketQueue>(queue_size);
    fallback_queue_ = std::make_unique<TSPacketQueue>(queue_size);
    
    // Create BOTH input receivers (hot standby mode for faster switching)
    uint32_t udp_buffer = config_.getUdpRcvbufSize();
    
    // Camera receiver: UDP receiver (from ffmpeg-srt-live)
    std::cout << "[Multiplexer] Creating Camera UDP receiver on port " << config_.getLiveUdpPort()
              << " with buffer=" << udp_buffer << " bytes" << std::endl;
    camera_receiver_ = std::make_unique<UDPReceiver>(
        "Camera", config_.getLiveUdpPort(), *camera_queue_, udp_buffer);
    
    // Drone receiver: RTMP receiver with auto-reconnect
    std::cout << "[Multiplexer] Creating Drone RTMP receiver for URL: " << config_.getDroneRtmpUrl() << std::endl;
    std::cout << "[Multiplexer] Drone reconnect config: initial=" << config_.getDroneReconnectInitialMs()
              << "ms, max=" << config_.getDroneReconnectMaxMs()
              << "ms, backoff=" << config_.getDroneReconnectBackoff() << "x" << std::endl;
    drone_receiver_ = std::make_unique<RTMPReceiver>(
        "Drone", config_.getDroneRtmpUrl(), *drone_queue_,
        config_.getDroneReconnectInitialMs(),
        config_.getDroneReconnectMaxMs(),
        config_.getDroneReconnectBackoff());
    
    // Fallback receiver (always UDP)
    std::cout << "[Multiplexer] Creating Fallback UDP receiver on port " << config_.getFallbackUdpPort()
              << " with buffer=" << udp_buffer << " bytes" << std::endl;
    fallback_receiver_ = std::make_unique<UDPReceiver>(
        "Fallback", config_.getFallbackUdpPort(), *fallback_queue_, udp_buffer);
    
    // Create analyzers - separate analyzers for camera, drone, and fallback
    camera_analyzer_ = std::make_unique<TSAnalyzer>();
    drone_analyzer_ = std::make_unique<TSAnalyzer>();
    fallback_analyzer_ = std::make_unique<TSAnalyzer>();
    
    // Create processing components
    timestamp_mgr_ = std::make_unique<TimestampManager>();
    pid_mapper_ = std::make_unique<PIDMapper>();
    
    // Create stream switcher with configurable min consecutive packets
    uint32_t min_consecutive = config_.getMinConsecutiveForSwitch();
    std::cout << "[Multiplexer] Creating StreamSwitcher with min_consecutive=" << min_consecutive << std::endl;
    switcher_ = std::make_unique<StreamSwitcher>(config_.getMaxLiveGapMs(), http_client_, min_consecutive);
    
    // Apply initial privacy mode that was queried earlier
    if (initial_privacy_mode_.load()) {
        switcher_->setPrivacyMode(true);
    }
    
    // Set up mode change callback to notify controller
    switcher_->setModeChangeCallback([this](Mode mode) {
        if (mode == Mode::LIVE) {
            http_client_->notifySceneLive();
        } else {
            http_client_->notifySceneFallback();
        }
    });
    
    // Set up input source change callback for runtime switching
    input_source_manager_->setInputSourceChangeCallback(
        [this](InputSource old_source, InputSource new_source) {
            onInputSourceChange(old_source, new_source);
        }
    );
    
    // Create RTMP output with configurable pacing
    uint32_t pacing_us = config_.getRtmpPacingUs();
    std::cout << "[Multiplexer] Creating RTMPOutput with pacing=" << pacing_us << "µs" << std::endl;
    rtmp_output_ = std::make_unique<RTMPOutput>(config_.getRtmpUrl(), pacing_us);
    
    // Create SPS/PPS injector for splice points
    sps_pps_injector_ = std::make_unique<SPSPPSInjector>();
    
    // Start BOTH input receivers (hot standby mode)
    if (!camera_receiver_->start()) {
        std::cerr << "[Multiplexer] Failed to start camera receiver" << std::endl;
        return false;
    }
    
    if (!drone_receiver_->start()) {
        std::cerr << "[Multiplexer] Failed to start drone receiver" << std::endl;
        return false;
    }
    
    std::cout << "[Multiplexer] Both camera and drone receivers started (hot standby mode)" << std::endl;
    
    // Start fallback receiver
    if (!fallback_receiver_->start()) {
        std::cerr << "[Multiplexer] Failed to start fallback receiver" << std::endl;
        return false;
    }
    
    // Analyze streams to get PID information
    // This will wait indefinitely for fallback stream
    if (!analyzeStreams()) {
        std::cerr << "[Multiplexer] Failed to analyze streams" << std::endl;
        return false;
    }
    
    // Set initial mode based on privacy mode and live stream availability
    bool active_stream_ready = input_source_manager_->isCamera() ?
        camera_stream_ready_.load() : drone_stream_ready_.load();
    
    if (initial_privacy_mode_.load()) {
        std::cout << "[Multiplexer] Starting in FALLBACK mode (privacy mode enabled)" << std::endl;
        switcher_->setMode(Mode::FALLBACK);
    } else if (!active_stream_ready) {
        std::cout << "[Multiplexer] Starting in FALLBACK mode (no live stream detected on "
                  << input_source_manager_->getInputSourceString() << ")" << std::endl;
        switcher_->setMode(Mode::FALLBACK);
    } else {
        std::cout << "[Multiplexer] Starting in LIVE mode with "
                  << input_source_manager_->getInputSourceString() << std::endl;
        switcher_->setMode(Mode::LIVE);
    }
    
    // Start RTMP output
    if (!rtmp_output_->start()) {
        std::cerr << "[Multiplexer] Failed to start RTMP output" << std::endl;
        return false;
    }
    
    // Now set health callback (rtmp_output_ is available)
    http_server_->setHealthCallback([this]() -> HealthStatus {
        HealthStatus status;
        if (rtmp_output_) {
            status.rtmp_connected = rtmp_output_->isConnected();
            status.packets_written = rtmp_output_->getPacketsWritten();
            status.ms_since_last_write = rtmp_output_->getMsSinceLastWrite();
        }
        return status;
    });
    
    initialized_ = true;
    start_time_ = std::chrono::steady_clock::now();
    
    // Explicitly notify controller of initial scene (in case callback didn't work)
    notifyInitialScene();
    
    std::cout << "[Multiplexer] Initialization complete" << std::endl;
    return true;
}

void Multiplexer::notifyInitialScene() {
    std::cout << "[Multiplexer] Notifying controller of initial scene..." << std::endl;
    
    Mode current_mode = switcher_->getMode();
    bool success = false;
    int max_retries = 5;
    int retry_delay_ms = 1000;
    
    for (int attempt = 1; attempt <= max_retries && !success; attempt++) {
        try {
            if (current_mode == Mode::LIVE) {
                std::cout << "[Multiplexer] Sending initial scene: LIVE (attempt " << attempt << "/" << max_retries << ")" << std::endl;
                http_client_->notifySceneLive();
            } else {
                std::cout << "[Multiplexer] Sending initial scene: FALLBACK (attempt " << attempt << "/" << max_retries << ")" << std::endl;
                http_client_->notifySceneFallback();
            }
            success = true;
            std::cout << "[Multiplexer] Initial scene notification successful" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[Multiplexer] Failed to notify initial scene (attempt " << attempt << "): " << e.what() << std::endl;
            if (attempt < max_retries) {
                std::cout << "[Multiplexer] Retrying in " << retry_delay_ms << "ms..." << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
                retry_delay_ms *= 2; // Exponential backoff
            }
        }
    }
    
    if (!success) {
        std::cerr << "[Multiplexer] WARNING: Failed to notify controller of initial scene after " << max_retries << " attempts" << std::endl;
    }
}

void Multiplexer::run() {
    if (!initialized_.load()) {
        std::cerr << "[Multiplexer] Not initialized" << std::endl;
        return;
    }
    
    // running_ is already set to true in initialize()
    std::cout << "[Multiplexer] Starting main loop" << std::endl;
    
    processLoop();
}

void Multiplexer::stop() {
    if (!running_.load()) {
        return;
    }
    
    auto shutdown_start = std::chrono::steady_clock::now();
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] Beginning graceful shutdown..." << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
    
    running_ = false;
    
    // Stop HTTP server
    std::cout << "[Multiplexer] Stopping HTTP server..." << std::endl;
    if (http_server_) http_server_->stop();
    
    // Stop receivers
    std::cout << "[Multiplexer] Stopping input receivers..." << std::endl;
    if (camera_receiver_) camera_receiver_->stop();
    if (drone_receiver_) drone_receiver_->stop();
    if (fallback_receiver_) fallback_receiver_->stop();
    
    // Stop RTMP output
    std::cout << "[Multiplexer] Stopping RTMP output..." << std::endl;
    if (rtmp_output_) rtmp_output_->stop();
    
    // Print statistics
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time_
    );
    
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] Statistics:" << std::endl;
    std::cout << "  Runtime: " << elapsed.count() << "s" << std::endl;
    std::cout << "  Input source: " << (input_source_manager_ ? input_source_manager_->getInputSourceString() : "unknown") << std::endl;
    std::cout << "  Packets processed: " << packets_processed_.load() << std::endl;
    std::cout << "  Camera packets received: " << (camera_receiver_ ? camera_receiver_->getPacketsReceived() : 0) << std::endl;
    std::cout << "  Drone packets received: " << (drone_receiver_ ? drone_receiver_->getPacketsReceived() : 0) << std::endl;
    std::cout << "  Fallback packets received: " << fallback_receiver_->getPacketsReceived() << std::endl;
    std::cout << "  RTMP packets written: " << rtmp_output_->getPacketsWritten() << std::endl;
    
    auto shutdown_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - shutdown_start
    );
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] Shutdown complete (" << shutdown_duration.count() << "ms)" << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
}

bool Multiplexer::analyzeStreams() {
    std::cout << "[Multiplexer] Analyzing streams..." << std::endl;
    std::cout << "[Multiplexer] Waiting for fallback stream only (live sources will be detected dynamically)" << std::endl;
    
    ts::TSPacket packet;
    auto last_status_log = std::chrono::steady_clock::now();
    
    // Camera and drone streams are NOT required at startup
    // They will be detected dynamically in processLoop() via analyzeLiveStreamDynamically()
    // This allows the multiplexer to start immediately with fallback when no live sources are available
    
    // Wait for fallback stream packets (required) - but allow graceful shutdown
    std::cout << "[Multiplexer] Waiting for fallback stream..." << std::endl;
    bool fallback_initialized = false;
    int total_wait_seconds = 0;
    
    while (!fallback_initialized && running_.load()) {
        int fallback_packets_analyzed = 0;
        
        // Try to analyze up to 100 packets
        while (fallback_packets_analyzed < 100 && running_.load() &&
               fallback_queue_->pop(packet, std::chrono::milliseconds(100))) {
            fallback_packets_analyzed++;
            fallback_analyzer_->analyzePacket(packet);
            
            if (fallback_analyzer_->isInitialized()) {
                fallback_initialized = true;
                break;
            }
        }
        
        // Check for shutdown signal
        if (!running_.load()) {
            std::cout << "[Multiplexer] Shutdown requested during initialization" << std::endl;
            return false;
        }
        
        // Check if we should log status
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_status_log);
        
        if (!fallback_initialized && elapsed.count() >= 5) {
            total_wait_seconds += elapsed.count();
            std::cout << "[Multiplexer] Still waiting for fallback stream... ("
                      << total_wait_seconds << "s elapsed)" << std::endl;
            last_status_log = now;
        }
        
        // If we got some packets but not initialized yet, wait a bit before retry
        // Use shorter sleeps with periodic shutdown checks
        if (!fallback_initialized && fallback_packets_analyzed > 0) {
            // Sleep in 100ms increments to allow faster shutdown response
            for (int i = 0; i < 5 && running_.load(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } else if (!fallback_initialized) {
            // No packets received, wait but check shutdown every 100ms
            for (int i = 0; i < 10 && running_.load(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
    
    // Check if we exited due to shutdown
    if (!running_.load()) {
        std::cout << "[Multiplexer] Initialization aborted due to shutdown signal" << std::endl;
        return false;
    }
    
    const auto& fallback_info = fallback_analyzer_->getStreamInfo();
    
    std::cout << "[Multiplexer] Fallback stream ready!" << std::endl;
    std::cout << "  Video PID: " << fallback_info.video_pid << std::endl;
    std::cout << "  Audio PID: " << fallback_info.audio_pid << std::endl;
    std::cout << "  PMT PID: " << fallback_info.pmt_pid << std::endl;
    
    // Check active input stream status and initialize PID mapper if available
    // Use the currently configured input source for initial PID mapping
    TSAnalyzer* active_analyzer = getActiveLiveAnalyzer();
    bool active_stream_ready = input_source_manager_->isCamera() ?
        camera_stream_ready_.load() : drone_stream_ready_.load();
    
    if (active_stream_ready && active_analyzer) {
        const auto& active_info = active_analyzer->getStreamInfo();
        std::cout << "[Multiplexer] Active input stream ("
                  << input_source_manager_->getInputSourceString() << "):" << std::endl;
        std::cout << "  Video PID: " << active_info.video_pid << std::endl;
        std::cout << "  Audio PID: " << active_info.audio_pid << std::endl;
        std::cout << "  PMT PID: " << active_info.pmt_pid << std::endl;
        
        // Initialize PID mapper with active stream and fallback
        pid_mapper_->initialize(active_info, fallback_info);
        std::cout << "[Multiplexer] Both streams ready" << std::endl;
    } else {
        std::cout << "[Multiplexer] Starting in fallback-only mode" << std::endl;
        std::cout << "[Multiplexer] Active input stream will be detected dynamically" << std::endl;
    }
    
    return true;
}

bool Multiplexer::analyzeLiveStreamDynamically() {
    static int analysis_attempt_count = 0;
    analysis_attempt_count++;
    
    // Get active queue and analyzer based on current input source
    TSPacketQueue* active_queue = getActiveLiveQueue();
    TSAnalyzer* active_analyzer = getActiveLiveAnalyzer();
    std::atomic<bool>* stream_ready_flag = input_source_manager_->isCamera() ?
        &camera_stream_ready_ : &drone_stream_ready_;
    std::string source_name = input_source_manager_->getInputSourceString();
    
    std::cout << "[Multiplexer] ================================================" << std::endl;
    std::cout << "[Multiplexer] Attempting to detect " << source_name << " stream (attempt #"
              << analysis_attempt_count << ")" << std::endl;
    std::cout << "[Multiplexer] ================================================" << std::endl;
    
    // Get the current queue size - we'll analyze accumulated packets instead of clearing them
    // This is more efficient as accumulated packets contain PAT, PMT, and media data we need
    size_t queue_size = active_queue->size();
    
    // Use adaptive packet budget: analyze at least 500 packets, or queue size if larger
    // This ensures we have enough packets to find PAT/PMT and validate media
    size_t max_packets_to_analyze = std::max(queue_size, static_cast<size_t>(500));
    
    std::cout << "[Multiplexer] DEBUG: Queue size before analysis: " << queue_size << std::endl;
    std::cout << "[Multiplexer] DEBUG: Max packets to analyze: " << max_packets_to_analyze << std::endl;
    std::cout << "[Multiplexer] DEBUG: stream_ready_ before: " << stream_ready_flag->load() << std::endl;
    
    // Log analyzer state BEFORE reset
    const auto& pre_reset_info = active_analyzer->getStreamInfo();
    std::cout << "[Multiplexer] DEBUG: Pre-reset analyzer state:" << std::endl;
    std::cout << "[Multiplexer]   initialized: " << pre_reset_info.initialized << std::endl;
    std::cout << "[Multiplexer]   video_pid: " << pre_reset_info.video_pid << std::endl;
    std::cout << "[Multiplexer]   audio_pid: " << pre_reset_info.audio_pid << std::endl;
    std::cout << "[Multiplexer]   valid_video_packets: " << pre_reset_info.valid_video_packets << std::endl;
    std::cout << "[Multiplexer]   valid_audio_packets: " << pre_reset_info.valid_audio_packets << std::endl;
    
    // Reset the analyzer to start fresh (clears old PID state from previous connection)
    // Note: reset() now re-registers PID_PAT for continued PAT parsing
    std::cout << "[Multiplexer] DEBUG: Calling analyzer->reset()..." << std::endl;
    active_analyzer->reset();
    
    // Log analyzer state AFTER reset
    const auto& post_reset_info = active_analyzer->getStreamInfo();
    std::cout << "[Multiplexer] DEBUG: Post-reset analyzer state:" << std::endl;
    std::cout << "[Multiplexer]   initialized: " << post_reset_info.initialized << std::endl;
    std::cout << "[Multiplexer]   video_pid: " << post_reset_info.video_pid << std::endl;
    std::cout << "[Multiplexer]   audio_pid: " << post_reset_info.audio_pid << std::endl;
    
    // Analyze accumulated packets from the queue
    size_t live_packets_analyzed = 0;
    ts::TSPacket packet;
    int pop_failures = 0;
    
    std::cout << "[Multiplexer] DEBUG: Starting packet analysis loop..." << std::endl;
    
    // Use a longer timeout (50ms) to allow real-time packets to arrive
    while (live_packets_analyzed < max_packets_to_analyze && running_.load()) {
        bool got_packet = active_queue->pop(packet, std::chrono::milliseconds(50));
        
        if (!got_packet) {
            pop_failures++;
            if (pop_failures <= 3) {
                std::cout << "[Multiplexer] DEBUG: pop() returned false (timeout), pop_failures="
                          << pop_failures << ", queue size=" << active_queue->size() << std::endl;
            }
            if (pop_failures >= 10) {
                std::cout << "[Multiplexer] DEBUG: Too many pop failures (" << pop_failures
                          << "), breaking out of analysis loop" << std::endl;
                break;
            }
            continue;
        }
        
        pop_failures = 0; // Reset on success
        active_analyzer->analyzePacket(packet);
        live_packets_analyzed++;
        
        // Log progress periodically (every 50 packets for more granularity)
        if (live_packets_analyzed % 50 == 0) {
            const auto& info = active_analyzer->getStreamInfo();
            std::cout << "[Multiplexer] Analysis progress: " << live_packets_analyzed << " packets"
                      << ", initialized=" << (info.initialized ? "yes" : "no")
                      << ", video=" << info.valid_video_packets
                      << "/" << StreamInfo::MIN_VALID_VIDEO_PACKETS
                      << ", audio=" << info.valid_audio_packets
                      << "/" << StreamInfo::MIN_VALID_AUDIO_PACKETS
                      << ", hasValidMediaData=" << (active_analyzer->hasValidMediaData() ? "yes" : "no")
                      << std::endl;
        }
        
        // Check if we have valid media data (not just PSI tables)
        if (active_analyzer->hasValidMediaData()) {
            std::cout << "[Multiplexer] Valid media data found after "
                      << live_packets_analyzed << " packets" << std::endl;
            break;
        }
    }
    
    std::cout << "[Multiplexer] DEBUG: Analysis loop ended. Packets analyzed: "
              << live_packets_analyzed << ", pop_failures: " << pop_failures << std::endl;
    
    // Check for shutdown signal
    if (!running_.load()) {
        std::cout << "[Multiplexer] DEBUG: Shutdown detected, returning false" << std::endl;
        return false;
    }
    
    const auto& stream_info = active_analyzer->getStreamInfo();
    
    std::cout << "[Multiplexer] DEBUG: Final analyzer state after analysis:" << std::endl;
    std::cout << "[Multiplexer]   initialized: " << stream_info.initialized << std::endl;
    std::cout << "[Multiplexer]   video_pid: " << stream_info.video_pid << std::endl;
    std::cout << "[Multiplexer]   audio_pid: " << stream_info.audio_pid << std::endl;
    std::cout << "[Multiplexer]   pmt_pid: " << stream_info.pmt_pid << std::endl;
    std::cout << "[Multiplexer]   pcr_pid: " << stream_info.pcr_pid << std::endl;
    std::cout << "[Multiplexer]   valid_video_packets: " << stream_info.valid_video_packets << std::endl;
    std::cout << "[Multiplexer]   valid_audio_packets: " << stream_info.valid_audio_packets << std::endl;
    std::cout << "[Multiplexer]   hasValidMediaData(): " << (active_analyzer->hasValidMediaData() ? "true" : "false") << std::endl;
    
    // Check for valid media data instead of just initialization
    if (!active_analyzer->hasValidMediaData()) {
        // Log current validation status
        if (stream_info.initialized) {
            std::cout << "[Multiplexer] RESULT: " << source_name << " stream PSI detected but waiting for valid media packets..." << std::endl;
            std::cout << "  Analyzed: " << live_packets_analyzed << " packets" << std::endl;
            std::cout << "  Video packets: " << stream_info.valid_video_packets
                      << "/" << StreamInfo::MIN_VALID_VIDEO_PACKETS << std::endl;
            std::cout << "  Audio packets: " << stream_info.valid_audio_packets
                      << "/" << StreamInfo::MIN_VALID_AUDIO_PACKETS << std::endl;
        } else {
            std::cout << "[Multiplexer] RESULT: No PSI tables found after "
                      << live_packets_analyzed << " packets" << std::endl;
        }
        std::cout << "[Multiplexer] DEBUG: Returning false from analyzeLiveStreamDynamically()" << std::endl;
        return false;
    }
    
    std::cout << "[Multiplexer] RESULT: " << source_name << " stream detected!" << std::endl;
    std::cout << "  Video PID: " << stream_info.video_pid << std::endl;
    std::cout << "  Audio PID: " << stream_info.audio_pid << std::endl;
    std::cout << "  PMT PID: " << stream_info.pmt_pid << std::endl;
    std::cout << "  Valid video packets: " << stream_info.valid_video_packets << std::endl;
    std::cout << "  Valid audio packets: " << stream_info.valid_audio_packets << std::endl;
    
    // Reinitialize PID mapper with active stream info
    const auto& fallback_info = fallback_analyzer_->getStreamInfo();
    pid_mapper_->initialize(stream_info, fallback_info);
    
    std::cout << "[Multiplexer] DEBUG: Setting stream_ready_ = true for " << source_name << std::endl;
    *stream_ready_flag = true;
    
    std::cout << "[Multiplexer] DEBUG: Returning true from analyzeLiveStreamDynamically()" << std::endl;
    return true;
}

void Multiplexer::processLoop() {
    ts::TSPacket packet;
    uint64_t log_interval = 1000; // Log every 1000 packets
    uint64_t live_check_interval = 100; // Check for live stream every 100 packets
    
    while (running_.load()) {
        // Check if input switch is pending - process it before normal loop
        if (pending_input_switch_.load() != input_source_manager_->getInputSource()) {
            // There was a pending switch that completed or got cancelled - reset
            pending_input_switch_ = input_source_manager_->getInputSource();
        }
        
        Mode current_mode = switcher_->getMode();
        
        // Get active queue and stream ready flag based on current input source
        TSPacketQueue* active_live_queue = getActiveLiveQueue();
        bool active_stream_ready = input_source_manager_->isCamera() ?
            camera_stream_ready_.load() : drone_stream_ready_.load();
        
        // Periodically check for live stream if not yet detected (and not during input switching)
        if (!active_stream_ready && !input_switching_.load() && packets_processed_ % live_check_interval == 0) {
            std::cout << "[Multiplexer] DEBUG: Periodic live check triggered (packets_processed_="
                      << packets_processed_.load() << ", stream_ready="
                      << active_stream_ready << ", active_queue empty="
                      << active_live_queue->empty() << ", queue size=" << active_live_queue->size() << ")" << std::endl;
            if (!active_live_queue->empty()) {
                std::cout << "[Multiplexer] Live packets detected in "
                          << input_source_manager_->getInputSourceString() << " queue, attempting analysis..." << std::endl;
                if (analyzeLiveStreamDynamically()) {
                    std::cout << "[Multiplexer] Stream successfully initialized!" << std::endl;
                    std::cout << "[Multiplexer] Attempting IDR-aware switch to LIVE mode..." << std::endl;
                    
                    // Use IDR-aware switch to live
                    if (switchToLiveAtIDR()) {
                        std::cout << "[Multiplexer] Successfully switched to LIVE at IDR!" << std::endl;
                    } else {
                        std::cout << "[Multiplexer] Failed to find IDR in live stream - staying on FALLBACK" << std::endl;
                        // Reset the appropriate stream ready flag
                        if (input_source_manager_->isCamera()) {
                            camera_stream_ready_ = false;
                        } else {
                            drone_stream_ready_ = false;
                        }
                    }
                    continue;
                }
            }
        }
        
        // Select queue based on current mode - use active live queue when in LIVE mode
        TSPacketQueue* queue = (current_mode == Mode::LIVE) ? active_live_queue : fallback_queue_.get();
        Source source = (current_mode == Mode::LIVE) ? Source::LIVE : Source::FALLBACK;
        
        // Try to get a packet (with short timeout for responsive shutdown)
        if (queue->pop(packet, std::chrono::milliseconds(1))) {
            // Update live timestamp if from live source
            if (current_mode == Mode::LIVE) {
                switcher_->updateLiveTimestamp();
            }
            
            // Process the packet
            processPacket(packet, source);
            
            packets_processed_++;
            
            // Periodic logging
            if (packets_processed_ % log_interval == 0) {
                std::cout << "[Multiplexer] Processed " << packets_processed_.load()
                          << " packets. Mode: " << (current_mode == Mode::LIVE ? "LIVE" : "FALLBACK")
                          << ", Queue size: " << queue->size()
                          << ", Live ready: " << (active_stream_ready ? "Yes" : "No") << std::endl;
            }
            
            // Check for mode switch after processing (but not during input switching)
            if (current_mode == Mode::LIVE) {
                // Check if we need to switch to fallback (skip if input switching is in progress)
                if (!input_switching_.load() && switcher_->checkLiveTimeout()) {
                    std::cout << "[Multiplexer] Live timeout detected - initiating IDR-aware switch to FALLBACK" << std::endl;
                    
                    // Use IDR-aware switch to fallback
                    switchToFallbackAtIDR();
                    
                    // DEBUG: Log switcher state
                    std::cout << "[Multiplexer] DEBUG: Switcher state after fallback switch:" << std::endl;
                    std::cout << "[Multiplexer]   mode: " << (switcher_->getMode() == Mode::LIVE ? "LIVE" : "FALLBACK") << std::endl;
                    std::cout << "[Multiplexer]   privacy_mode: " << switcher_->isPrivacyMode() << std::endl;
                    std::cout << "[Multiplexer]   camera_stream_ready_: " << camera_stream_ready_.load() << std::endl;
                    std::cout << "[Multiplexer]   drone_stream_ready_: " << drone_stream_ready_.load() << std::endl;
                }
            } else {
                // In FALLBACK mode, check if live packets are arriving on active queue
                // If so, increment the counter to track consecutive live packets
                if (!active_live_queue->empty()) {
                    switcher_->updateLiveTimestamp();
                    
                    // DEBUG: Occasionally log fallback mode state
                    static uint64_t fallback_log_counter = 0;
                    fallback_log_counter++;
                    if (fallback_log_counter % 1000 == 0) {
                        std::cout << "[Multiplexer] DEBUG: In FALLBACK mode, active_queue not empty"
                                  << ", input_source=" << input_source_manager_->getInputSourceString()
                                  << ", stream_ready=" << active_stream_ready
                                  << ", queue size=" << active_live_queue->size()
                                  << ", time_since_last_live=" << switcher_->getTimeSinceLastLive().count() << "ms"
                                  << std::endl;
                    }
                }
                
                // Try to return to live if packets are available and active stream is ready
                // Use IDR-aware switching for clean splice (but not during input switching)
                if (!input_switching_.load() && active_stream_ready &&
                    !active_live_queue->empty() && switcher_->tryReturnToLive()) {
                    std::cout << "[Multiplexer] tryReturnToLive() triggered - initiating IDR-aware switch to LIVE" << std::endl;
                    
                    // Reset the mode back to fallback temporarily - switchToLiveAtIDR will set it to LIVE
                    switcher_->setMode(Mode::FALLBACK);
                    
                    // Use IDR-aware switch to live
                    if (switchToLiveAtIDR()) {
                        std::cout << "[Multiplexer] Successfully switched to LIVE at IDR!" << std::endl;
                    } else {
                        std::cout << "[Multiplexer] Failed to find IDR in live stream - staying on FALLBACK" << std::endl;
                        // Reset the appropriate stream ready flag
                        if (input_source_manager_->isCamera()) {
                            camera_stream_ready_ = false;
                        } else {
                            drone_stream_ready_ = false;
                        }
                    }
                }
            }
        } else {
            // No packet available, check for timeout
            switcher_->checkLiveTimeout();
        }
    }
}

void Multiplexer::processPacket(ts::TSPacket& packet, Source source) {
    // Extract timestamps
    TimestampInfo ts_info = (source == Source::LIVE)
        ? getActiveLiveAnalyzer()->extractTimestamps(packet)
        : fallback_analyzer_->extractTimestamps(packet);
    
    // adjustPacket() handles both LIVE and FALLBACK:
    // - LIVE: Applies live_offset_ if non-zero (for FALLBACK→LIVE continuity), otherwise passthrough
    // - FALLBACK: Applies fallback_offset_ (for LIVE→FALLBACK continuity)
    timestamp_mgr_->adjustPacket(packet, source, ts_info);
    
    if (source == Source::FALLBACK) {
        // Remap PIDs to match live stream PIDs (only needed for fallback)
        pid_mapper_->remapPacket(packet);
    }
    
    // Fix continuity counters for ALL packets to ensure seamless output stream.
    // Input CC values are irrelevant - what matters is output stream consistency.
    // This prevents CC discontinuities when switching between LIVE and FALLBACK,
    // which could cause strict decoders to drop packets and cause brief glitches.
    pid_mapper_->fixContinuityCounter(packet);
    
    // Write to RTMP output
    rtmp_output_->writePacket(packet);
}

void Multiplexer::queryInitialPrivacyMode() {
    std::cout << "[Multiplexer] Querying initial privacy mode from controller..." << std::endl;
    
    try {
        bool privacy_enabled = http_client_->queryPrivacyMode();
        initial_privacy_mode_ = privacy_enabled;
        
        if (privacy_enabled) {
            std::cout << "[Multiplexer] Privacy mode is ENABLED on controller" << std::endl;
        } else {
            std::cout << "[Multiplexer] Privacy mode is DISABLED on controller" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Multiplexer] Failed to query privacy mode: " << e.what() << std::endl;
        std::cout << "[Multiplexer] Defaulting to privacy mode disabled" << std::endl;
        initial_privacy_mode_ = false;
    }
}

void Multiplexer::onPrivacyModeChange(bool enabled) {
    std::cout << "[Multiplexer] Received privacy mode change: " << (enabled ? "ENABLED" : "DISABLED") << std::endl;
    
    if (switcher_) {
        switcher_->setPrivacyMode(enabled);
        
        if (enabled && switcher_->getMode() == Mode::LIVE) {
            // Force switch to fallback immediately - use IDR-aware switching
            std::cout << "[Multiplexer] Forcing switch to FALLBACK mode due to privacy mode" << std::endl;
            switchToFallbackAtIDR();
        }
    }
}

bool Multiplexer::switchToLiveAtIDR() {
    // Get active queue and analyzer based on current input source
    TSPacketQueue* active_queue = getActiveLiveQueue();
    TSAnalyzer* active_analyzer = getActiveLiveAnalyzer();
    std::string source_name = input_source_manager_->getInputSourceString();
    
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] IDR-AWARE SWITCH: Waiting for " << source_name << " IDR frame..." << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
    
    switch_buffer_.clear();
    switch_wait_start_ = std::chrono::steady_clock::now();
    
    ts::TSPacket packet;
    size_t idr_packet_index = 0;
    bool found_idr = false;
    bool needs_sps_pps_injection = false;  // Track if we need to inject SPS/PPS
    
    while (running_.load()) {
        // Check timeout
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - switch_wait_start_
        );
        
        if (elapsed.count() >= static_cast<int64_t>(live_idr_timeout_ms_)) {
            std::cout << "[Multiplexer] IDR TIMEOUT: No IDR found in " << source_name << " stream after "
                      << elapsed.count() << "ms (timeout=" << live_idr_timeout_ms_ << "ms) - staying on FALLBACK" << std::endl;
            switch_buffer_.clear();
            return false;
        }
        
        // Try to get a live packet from active queue
        if (!active_queue->pop(packet, std::chrono::milliseconds(10))) {
            continue;
        }
        
        // Store packet in buffer
        switch_buffer_.push_back(packet);
        
        // Check if this is a video packet with PUSI (potential IDR)
        if (active_analyzer->isVideoPacket(packet) && packet.getPUSI()) {
            FrameInfo frame_info = active_analyzer->extractFrameInfo(packet);
            
            if (frame_info.isCleanSwitchPoint()) {
                found_idr = true;
                needs_sps_pps_injection = false;  // IDR has inline SPS/PPS
                idr_packet_index = switch_buffer_.size() - 1;
                
                std::cout << "[Multiplexer] IDR FOUND! Clean switch point at buffer index "
                          << idr_packet_index << " (after " << elapsed.count() << "ms)"
                          << " - has_sps=" << frame_info.has_sps
                          << ", has_pps=" << frame_info.has_pps
                          << " (no injection needed)" << std::endl;
                break;
            } else if (frame_info.is_idr) {
                // IDR found but missing SPS/PPS - check if we have them stored
                if (active_analyzer->getNALParser().hasParameterSets()) {
                    found_idr = true;
                    needs_sps_pps_injection = true;  // Need to inject stored SPS/PPS
                    idr_packet_index = switch_buffer_.size() - 1;
                    
                    std::cout << "[Multiplexer] IDR FOUND (will inject stored SPS/PPS) at buffer index "
                              << idr_packet_index << " (after " << elapsed.count() << "ms)" << std::endl;
                    break;
                } else {
                    std::cout << "[Multiplexer] IDR found but no SPS/PPS available - continuing to search..." << std::endl;
                }
            }
        }
        
        // Log progress periodically
        if (switch_buffer_.size() % 100 == 0) {
            std::cout << "[Multiplexer] IDR search: buffered " << switch_buffer_.size()
                      << " packets, elapsed=" << elapsed.count() << "ms" << std::endl;
        }
    }
    
    if (!found_idr) {
        std::cout << "[Multiplexer] IDR search aborted (shutdown)" << std::endl;
        switch_buffer_.clear();
        return false;
    }
    
    // We found an IDR! Switch to live mode and drain buffer from IDR
    std::cout << "[Multiplexer] Switching to LIVE mode at IDR boundary!" << std::endl;
    switcher_->setMode(Mode::LIVE);
    switcher_->updateLiveTimestamp();
    
    // Set up timestamp offset from the IDR packet
    TimestampInfo ts_info = active_analyzer->extractTimestamps(switch_buffer_[idr_packet_index]);
    timestamp_mgr_->onSourceSwitch(Source::LIVE, ts_info);
    
    // Drain buffer starting from IDR (with SPS/PPS injection if needed)
    drainBufferFromIDR(switch_buffer_, idr_packet_index, Source::LIVE, needs_sps_pps_injection);
    
    switch_buffer_.clear();
    return true;
}

bool Multiplexer::switchToFallbackAtIDR() {
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] IDR-AWARE SWITCH: Waiting for fallback IDR frame..." << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
    
    switch_buffer_.clear();
    switch_wait_start_ = std::chrono::steady_clock::now();
    
    ts::TSPacket packet;
    size_t idr_packet_index = 0;
    bool found_idr = false;
    bool needs_sps_pps_injection = false;  // Track if we need to inject SPS/PPS
    
    while (running_.load()) {
        // Check timeout
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - switch_wait_start_
        );
        
        if (elapsed.count() >= static_cast<int64_t>(fallback_idr_timeout_ms_)) {
            std::cout << "[Multiplexer] IDR TIMEOUT: No IDR found in fallback stream after "
                      << elapsed.count() << "ms (timeout=" << fallback_idr_timeout_ms_ << "ms) - forcing switch anyway (fallback should have regular IDRs)" << std::endl;
            // For fallback, we force switch even without IDR since it loops and should have IDRs
            // This is a safety fallback - shouldn't normally happen
            break;
        }
        
        // Try to get a fallback packet
        if (!fallback_queue_->pop(packet, std::chrono::milliseconds(10))) {
            continue;
        }
        
        // Store packet in buffer
        switch_buffer_.push_back(packet);
        
        // Check if this is a video packet with PUSI (potential IDR)
        if (fallback_analyzer_->isVideoPacket(packet) && packet.getPUSI()) {
            FrameInfo frame_info = fallback_analyzer_->extractFrameInfo(packet);
            
            if (frame_info.isCleanSwitchPoint()) {
                found_idr = true;
                needs_sps_pps_injection = false;  // IDR has inline SPS/PPS
                idr_packet_index = switch_buffer_.size() - 1;
                
                std::cout << "[Multiplexer] FALLBACK IDR FOUND! Clean switch point at buffer index "
                          << idr_packet_index << " (after " << elapsed.count() << "ms)"
                          << " - has_sps=" << frame_info.has_sps
                          << ", has_pps=" << frame_info.has_pps
                          << " (no injection needed)" << std::endl;
                break;
            } else if (frame_info.is_idr) {
                // IDR found but missing SPS/PPS - check if we have them stored
                if (fallback_analyzer_->getNALParser().hasParameterSets()) {
                    found_idr = true;
                    needs_sps_pps_injection = true;  // Need to inject stored SPS/PPS
                    idr_packet_index = switch_buffer_.size() - 1;
                    
                    std::cout << "[Multiplexer] FALLBACK IDR FOUND (will inject stored SPS/PPS) at buffer index "
                              << idr_packet_index << " (after " << elapsed.count() << "ms)" << std::endl;
                    break;
                }
            }
        }
        
        // Log progress periodically
        if (switch_buffer_.size() % 50 == 0) {
            std::cout << "[Multiplexer] Fallback IDR search: buffered " << switch_buffer_.size()
                      << " packets, elapsed=" << elapsed.count() << "ms" << std::endl;
        }
    }
    
    // Switch to fallback mode
    std::cout << "[Multiplexer] Switching to FALLBACK mode" << (found_idr ? " at IDR boundary!" : " (timeout)") << std::endl;
    switcher_->setMode(Mode::FALLBACK);
    
    if (found_idr && !switch_buffer_.empty()) {
        // Set up timestamp offset from the IDR packet
        TimestampInfo ts_info = fallback_analyzer_->extractTimestamps(switch_buffer_[idr_packet_index]);
        timestamp_mgr_->onSourceSwitch(Source::FALLBACK, ts_info);
        
        // Drain buffer starting from IDR (with SPS/PPS injection if needed)
        drainBufferFromIDR(switch_buffer_, idr_packet_index, Source::FALLBACK, needs_sps_pps_injection);
    } else if (!switch_buffer_.empty()) {
        // Timeout case - process from beginning of buffer
        // Try to inject SPS/PPS if available since we don't have a clean switch point
        bool timeout_injection_needed = fallback_analyzer_->getNALParser().hasParameterSets();
        TimestampInfo ts_info = fallback_analyzer_->extractTimestamps(switch_buffer_[0]);
        timestamp_mgr_->onSourceSwitch(Source::FALLBACK, ts_info);
        drainBufferFromIDR(switch_buffer_, 0, Source::FALLBACK, timeout_injection_needed);
    }
    
    switch_buffer_.clear();
    
    // Reset live stream ready flag so we re-analyze when live comes back
    if (input_source_manager_->isCamera()) {
        camera_stream_ready_ = false;
    } else {
        drone_stream_ready_ = false;
    }
    
    return found_idr;
}

void Multiplexer::drainBufferFromIDR(std::vector<ts::TSPacket>& buffer, size_t idr_index,
                                     Source source, bool needs_sps_pps_injection) {
    std::cout << "[Multiplexer] Draining " << (buffer.size() - idr_index)
              << " packets from buffer starting at IDR (index " << idr_index << ")"
              << (needs_sps_pps_injection ? " [with SPS/PPS injection]" : "") << std::endl;
    
    // STEP 1: Inject PAT/PMT at splice point
    // Per splice.md: "Emit a new PAT/PMT at the splice" and "ensure tables repeat a few times for safety"
    // This ensures the downstream decoder receives updated stream configuration
    size_t patpmt_injected = injectPATMT(source, 3);  // Repeat 3 times for safety
    if (patpmt_injected > 0) {
        std::cout << "[Multiplexer] Injected " << patpmt_injected << " PAT/PMT packets at splice point" << std::endl;
    }
    
    // STEP 2: Inject SPS/PPS before the IDR if needed
    if (needs_sps_pps_injection && idr_index < buffer.size()) {
        // Get video PID and timestamps from the IDR packet
        uint16_t video_pid = buffer[idr_index].getPID();
        
        TSAnalyzer* analyzer = (source == Source::LIVE) ? getActiveLiveAnalyzer() : fallback_analyzer_.get();
        TimestampInfo ts_info = analyzer->extractTimestamps(buffer[idr_index]);
        
        size_t injected = injectSPSPPS(source, video_pid, ts_info.pts, ts_info.dts);
        std::cout << "[Multiplexer] Injected " << injected << " SPS/PPS packets before IDR" << std::endl;
    }
    
    for (size_t i = idr_index; i < buffer.size(); i++) {
        processPacket(buffer[i], source);
        packets_processed_++;
    }
    
    std::cout << "[Multiplexer] Buffer drain complete" << std::endl;
}

size_t Multiplexer::injectPATMT(Source source, int repetitions) {
    // Get the appropriate analyzer for the source
    TSAnalyzer* analyzer = (source == Source::LIVE) ? getActiveLiveAnalyzer() : fallback_analyzer_.get();
    if (!analyzer) {
        std::cerr << "[Multiplexer] Analyzer not available for PAT/PMT injection" << std::endl;
        return 0;
    }
    
    // Check if we have stored PAT/PMT packets
    if (!analyzer->hasPATPackets() && !analyzer->hasPMTPackets()) {
        std::cout << "[Multiplexer] No PAT/PMT packets stored for injection - skipping" << std::endl;
        return 0;
    }
    
    const std::vector<ts::TSPacket>& pat_packets = analyzer->getLastPATPackets();
    const std::vector<ts::TSPacket>& pmt_packets = analyzer->getLastPMTPackets();
    
    std::cout << "[Multiplexer] Injecting PAT/PMT at splice point:"
              << " PAT=" << pat_packets.size() << " packets,"
              << " PMT=" << pmt_packets.size() << " packets,"
              << " repetitions=" << repetitions << std::endl;
    
    size_t total_injected = 0;
    
    // Inject PAT and PMT multiple times for safety
    // Per splice.md: "ensure tables repeat a few times for safety"
    for (int rep = 0; rep < repetitions; rep++) {
        // Inject PAT packets
        for (const auto& packet : pat_packets) {
            ts::TSPacket pkt_copy = packet;
            
            // Fix continuity counter for output stream consistency
            pid_mapper_->fixContinuityCounter(pkt_copy);
            
            rtmp_output_->writePacket(pkt_copy);
            total_injected++;
        }
        
        // Inject PMT packets
        for (const auto& packet : pmt_packets) {
            ts::TSPacket pkt_copy = packet;
            
            // For fallback source, remap PIDs to match live stream
            if (source == Source::FALLBACK) {
                pid_mapper_->remapPacket(pkt_copy);
            }
            
            // Fix continuity counter for output stream consistency
            pid_mapper_->fixContinuityCounter(pkt_copy);
            
            rtmp_output_->writePacket(pkt_copy);
            total_injected++;
        }
    }
    
    std::cout << "[Multiplexer] PAT/PMT injection complete - wrote " << total_injected
              << " packets total" << std::endl;
    
    return total_injected;
}

size_t Multiplexer::injectSPSPPS(Source source, uint16_t video_pid,
                                  std::optional<uint64_t> pts, std::optional<uint64_t> dts) {
    if (!sps_pps_injector_) {
        std::cerr << "[Multiplexer] SPS/PPS injector not initialized!" << std::endl;
        return 0;
    }
    
    // Get the appropriate analyzer for the source
    TSAnalyzer* analyzer = (source == Source::LIVE) ? getActiveLiveAnalyzer() : fallback_analyzer_.get();
    if (!analyzer) {
        std::cerr << "[Multiplexer] Analyzer not available for source" << std::endl;
        return 0;
    }
    
    const NALParser& nal_parser = analyzer->getNALParser();
    if (!nal_parser.hasParameterSets()) {
        std::cerr << "[Multiplexer] No SPS/PPS stored for injection" << std::endl;
        return 0;
    }
    
    const std::vector<uint8_t>& sps = nal_parser.getLastSPS();
    const std::vector<uint8_t>& pps = nal_parser.getLastPPS();
    
    std::cout << "[Multiplexer] Creating SPS/PPS injection packets for PID " << video_pid
              << " (SPS: " << sps.size() << " bytes, PPS: " << pps.size() << " bytes)"
              << ", PTS=" << (pts.has_value() ? std::to_string(pts.value()) : "none")
              << ", DTS=" << (dts.has_value() ? std::to_string(dts.value()) : "none")
              << std::endl;
    
    // Create the injection packets
    std::vector<ts::TSPacket> injection_packets = sps_pps_injector_->createSPSPPSPackets(
        sps, pps, video_pid, pts, dts
    );
    
    if (injection_packets.empty()) {
        std::cerr << "[Multiplexer] Failed to create SPS/PPS injection packets" << std::endl;
        return 0;
    }
    
    // Write injection packets to output
    for (auto& packet : injection_packets) {
        // For fallback source, remap PIDs and fix continuity counters
        if (source == Source::FALLBACK) {
            pid_mapper_->remapPacket(packet);
            pid_mapper_->fixContinuityCounter(packet);
        }
        
        rtmp_output_->writePacket(packet);
        packets_processed_++;
    }
    
    std::cout << "[Multiplexer] SPS/PPS injection complete - wrote " << injection_packets.size()
              << " packets" << std::endl;
    
    return injection_packets.size();
}

// Helper method to get the active live queue based on current input source
TSPacketQueue* Multiplexer::getActiveLiveQueue() const {
    if (input_source_manager_->isCamera()) {
        return camera_queue_.get();
    } else {
        return drone_queue_.get();
    }
}

// Helper method to get the active live analyzer based on current input source
TSAnalyzer* Multiplexer::getActiveLiveAnalyzer() const {
    if (input_source_manager_->isCamera()) {
        return camera_analyzer_.get();
    } else {
        return drone_analyzer_.get();
    }
}

// Callback handler for input source changes
void Multiplexer::onInputSourceChange(InputSource old_source, InputSource new_source) {
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] INPUT SOURCE CHANGE REQUESTED: "
              << InputSourceManager::toString(old_source) << " -> "
              << InputSourceManager::toString(new_source) << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
    
    // Store the pending switch target
    pending_input_switch_ = new_source;
    
    // Validate that target stream is ready (has been analyzed and has active data)
    bool target_ready = (new_source == InputSource::CAMERA) ?
        camera_stream_ready_.load() : drone_stream_ready_.load();
    
    std::string target_name = InputSourceManager::toString(new_source);
    
    if (!target_ready) {
        std::cout << "[Multiplexer] WARNING: Target stream (" << target_name
                  << ") is not ready - will attempt dynamic analysis during switch" << std::endl;
        
        // Try to analyze the target stream before switching
        TSPacketQueue* target_queue = (new_source == InputSource::CAMERA) ?
            camera_queue_.get() : drone_queue_.get();
        TSAnalyzer* target_analyzer = (new_source == InputSource::CAMERA) ?
            camera_analyzer_.get() : drone_analyzer_.get();
        std::atomic<bool>* target_ready_flag = (new_source == InputSource::CAMERA) ?
            &camera_stream_ready_ : &drone_stream_ready_;
        
        if (!target_queue->empty()) {
            std::cout << "[Multiplexer] Attempting to analyze " << target_name << " stream..." << std::endl;
            
            // Quick analysis attempt
            target_analyzer->reset();
            ts::TSPacket packet;
            int packets_analyzed = 0;
            while (packets_analyzed < 200 && target_queue->pop(packet, std::chrono::milliseconds(10))) {
                target_analyzer->analyzePacket(packet);
                packets_analyzed++;
                if (target_analyzer->hasValidMediaData()) {
                    *target_ready_flag = true;
                    target_ready = true;
                    std::cout << "[Multiplexer] " << target_name << " stream analyzed successfully!" << std::endl;
                    break;
                }
            }
        }
        
        if (!target_ready) {
            std::cout << "[Multiplexer] WARNING: " << target_name
                      << " stream still not ready - switch may fail or take longer" << std::endl;
        }
    }
    
    // If we're currently in FALLBACK mode, we may not need IDR-aware switching
    // because we're already not outputting from either camera or drone
    Mode current_mode = switcher_->getMode();
    
    if (current_mode == Mode::FALLBACK) {
        std::cout << "[Multiplexer] Currently in FALLBACK mode - switching input source directly" << std::endl;
        
        // Just update which stream we monitor for going back to LIVE
        // The next time we switch to LIVE, we'll use the new input source
        switcher_->resetState();  // Reset state for new input source
        
        std::cout << "[Multiplexer] Input source changed to " << target_name << std::endl;
        return;
    }
    
    // We're in LIVE mode - need IDR-aware switch
    std::cout << "[Multiplexer] Currently in LIVE mode - initiating IDR-aware input switch" << std::endl;
    
    if (new_source == InputSource::CAMERA) {
        switchToCameraAtIDR();
    } else {
        switchToDroneAtIDR();
    }
}

bool Multiplexer::switchToCameraAtIDR() {
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] IDR-AWARE INPUT SWITCH: Waiting for camera IDR frame..." << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
    
    // Set input switching flag to prevent fallback timeout during switch
    input_switching_ = true;
    
    // CRITICAL FIX: Drain stale packets from CAMERA queue BEFORE searching for IDR
    // While drone was active, camera packets accumulated in the queue with advancing timestamps.
    // Using those stale packets would cause massive timestamp offsets.
    std::cout << "[Multiplexer] Draining stale packets from camera queue BEFORE IDR search..." << std::endl;
    size_t drained_before = 0;
    ts::TSPacket drain_packet;
    while (camera_queue_->pop(drain_packet, std::chrono::milliseconds(1))) {
        drained_before++;
    }
    if (drained_before > 0) {
        std::cout << "[Multiplexer] Drained " << drained_before << " stale packets from camera queue" << std::endl;
    }
    
    switch_buffer_.clear();
    switch_wait_start_ = std::chrono::steady_clock::now();
    switch_state_ = SwitchState::WAITING_CAMERA_IDR;
    
    ts::TSPacket packet;
    size_t idr_packet_index = 0;
    bool found_idr = false;
    bool needs_sps_pps_injection = false;
    
    while (running_.load()) {
        // Check timeout
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - switch_wait_start_
        );
        
        if (elapsed.count() >= static_cast<int64_t>(input_switch_idr_timeout_ms_)) {
            std::cout << "[Multiplexer] IDR TIMEOUT: No IDR found in camera stream after "
                      << elapsed.count() << "ms (timeout=" << input_switch_idr_timeout_ms_ 
                      << "ms) - staying on current source" << std::endl;
            switch_buffer_.clear();
            switch_state_ = SwitchState::NONE;
            input_switching_ = false;
            return false;
        }
        
        // Try to get a camera packet
        if (!camera_queue_->pop(packet, std::chrono::milliseconds(10))) {
            continue;
        }
        
        // Store packet in buffer
        switch_buffer_.push_back(packet);
        
        // Check if this is a video packet with PUSI (potential IDR)
        if (camera_analyzer_->isVideoPacket(packet) && packet.getPUSI()) {
            FrameInfo frame_info = camera_analyzer_->extractFrameInfo(packet);
            
            if (frame_info.isCleanSwitchPoint()) {
                found_idr = true;
                needs_sps_pps_injection = false;
                idr_packet_index = switch_buffer_.size() - 1;
                
                std::cout << "[Multiplexer] CAMERA IDR FOUND! Clean switch point at buffer index "
                          << idr_packet_index << " (after " << elapsed.count() << "ms)"
                          << " - has_sps=" << frame_info.has_sps
                          << ", has_pps=" << frame_info.has_pps << std::endl;
                break;
            } else if (frame_info.is_idr) {
                if (camera_analyzer_->getNALParser().hasParameterSets()) {
                    found_idr = true;
                    needs_sps_pps_injection = true;
                    idr_packet_index = switch_buffer_.size() - 1;
                    
                    std::cout << "[Multiplexer] CAMERA IDR FOUND (will inject stored SPS/PPS) at buffer index "
                              << idr_packet_index << " (after " << elapsed.count() << "ms)" << std::endl;
                    break;
                }
            }
        }
        
        // Log progress periodically
        if (switch_buffer_.size() % 100 == 0) {
            std::cout << "[Multiplexer] Camera IDR search: buffered " << switch_buffer_.size()
                      << " packets, elapsed=" << elapsed.count() << "ms" << std::endl;
        }
    }
    
    if (!found_idr) {
        std::cout << "[Multiplexer] Camera IDR search aborted (shutdown or timeout)" << std::endl;
        switch_buffer_.clear();
        switch_state_ = SwitchState::NONE;
        input_switching_ = false;
        return false;
    }
    
    // Found IDR - perform the switch
    std::cout << "[Multiplexer] Switching to CAMERA input at IDR boundary!" << std::endl;
    
    // Set up timestamp offset from the IDR packet using LIVE→LIVE transition method
    // This ensures seamless timestamp continuity when switching between live sources
    TimestampInfo ts_info = camera_analyzer_->extractTimestamps(switch_buffer_[idr_packet_index]);
    timestamp_mgr_->onLiveSourceSwitch(ts_info);
    
    // Reinitialize PID mapper with camera stream info
    const auto& camera_info = camera_analyzer_->getStreamInfo();
    const auto& fallback_info = fallback_analyzer_->getStreamInfo();
    pid_mapper_->initialize(camera_info, fallback_info);
    
    // Reset stream switcher state for new input (clears consecutive counter, updates timestamp)
    switcher_->resetState();
    
    // Clear any remaining stale packets from the old (drone) queue
    std::cout << "[Multiplexer] Clearing stale packets from drone queue..." << std::endl;
    size_t drained = 0;
    ts::TSPacket drain_pkt;
    while (drone_queue_->pop(drain_pkt, std::chrono::milliseconds(1))) {
        drained++;
    }
    if (drained > 0) {
        std::cout << "[Multiplexer] Drained " << drained << " stale packets from drone queue" << std::endl;
    }
    
    // Drain buffer starting from IDR (with SPS/PPS injection if needed)
    drainBufferFromIDR(switch_buffer_, idr_packet_index, Source::LIVE, needs_sps_pps_injection);
    
    switch_buffer_.clear();
    switch_state_ = SwitchState::NONE;
    input_switching_ = false;
    
    std::cout << "[Multiplexer] Camera input switch complete!" << std::endl;
    return true;
}

bool Multiplexer::switchToDroneAtIDR() {
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] IDR-AWARE INPUT SWITCH: Waiting for drone IDR frame..." << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
    
    // Set input switching flag to prevent fallback timeout during switch
    input_switching_ = true;
    
    // CRITICAL FIX: Drain stale packets from DRONE queue BEFORE searching for IDR
    // While camera was active, drone packets accumulated in the queue with advancing timestamps.
    // Using those stale packets would cause massive timestamp offsets.
    std::cout << "[Multiplexer] Draining stale packets from drone queue BEFORE IDR search..." << std::endl;
    size_t drained_before = 0;
    ts::TSPacket drain_packet;
    while (drone_queue_->pop(drain_packet, std::chrono::milliseconds(1))) {
        drained_before++;
    }
    if (drained_before > 0) {
        std::cout << "[Multiplexer] Drained " << drained_before << " stale packets from drone queue" << std::endl;
    }
    
    switch_buffer_.clear();
    switch_wait_start_ = std::chrono::steady_clock::now();
    switch_state_ = SwitchState::WAITING_DRONE_IDR;
    
    ts::TSPacket packet;
    size_t idr_packet_index = 0;
    bool found_idr = false;
    bool needs_sps_pps_injection = false;
    
    while (running_.load()) {
        // Check timeout
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - switch_wait_start_
        );
        
        if (elapsed.count() >= static_cast<int64_t>(input_switch_idr_timeout_ms_)) {
            std::cout << "[Multiplexer] IDR TIMEOUT: No IDR found in drone stream after "
                      << elapsed.count() << "ms (timeout=" << input_switch_idr_timeout_ms_ 
                      << "ms) - staying on current source" << std::endl;
            switch_buffer_.clear();
            switch_state_ = SwitchState::NONE;
            input_switching_ = false;
            return false;
        }
        
        // Try to get a drone packet
        if (!drone_queue_->pop(packet, std::chrono::milliseconds(10))) {
            continue;
        }
        
        // Store packet in buffer
        switch_buffer_.push_back(packet);
        
        // Check if this is a video packet with PUSI (potential IDR)
        if (drone_analyzer_->isVideoPacket(packet) && packet.getPUSI()) {
            FrameInfo frame_info = drone_analyzer_->extractFrameInfo(packet);
            
            if (frame_info.isCleanSwitchPoint()) {
                found_idr = true;
                needs_sps_pps_injection = false;
                idr_packet_index = switch_buffer_.size() - 1;
                
                std::cout << "[Multiplexer] DRONE IDR FOUND! Clean switch point at buffer index "
                          << idr_packet_index << " (after " << elapsed.count() << "ms)"
                          << " - has_sps=" << frame_info.has_sps
                          << ", has_pps=" << frame_info.has_pps << std::endl;
                break;
            } else if (frame_info.is_idr) {
                if (drone_analyzer_->getNALParser().hasParameterSets()) {
                    found_idr = true;
                    needs_sps_pps_injection = true;
                    idr_packet_index = switch_buffer_.size() - 1;
                    
                    std::cout << "[Multiplexer] DRONE IDR FOUND (will inject stored SPS/PPS) at buffer index "
                              << idr_packet_index << " (after " << elapsed.count() << "ms)" << std::endl;
                    break;
                }
            }
        }
        
        // Log progress periodically
        if (switch_buffer_.size() % 100 == 0) {
            std::cout << "[Multiplexer] Drone IDR search: buffered " << switch_buffer_.size()
                      << " packets, elapsed=" << elapsed.count() << "ms" << std::endl;
        }
    }
    
    if (!found_idr) {
        std::cout << "[Multiplexer] Drone IDR search aborted (shutdown or timeout)" << std::endl;
        switch_buffer_.clear();
        switch_state_ = SwitchState::NONE;
        input_switching_ = false;
        return false;
    }
    
    // Found IDR - perform the switch
    std::cout << "[Multiplexer] Switching to DRONE input at IDR boundary!" << std::endl;
    
    // Set up timestamp offset from the IDR packet using LIVE→LIVE transition method
    // This ensures seamless timestamp continuity when switching between live sources
    TimestampInfo ts_info = drone_analyzer_->extractTimestamps(switch_buffer_[idr_packet_index]);
    timestamp_mgr_->onLiveSourceSwitch(ts_info);
    
    // Reinitialize PID mapper with drone stream info
    const auto& drone_info = drone_analyzer_->getStreamInfo();
    const auto& fallback_info = fallback_analyzer_->getStreamInfo();
    pid_mapper_->initialize(drone_info, fallback_info);
    
    // Reset stream switcher state for new input (clears consecutive counter, updates timestamp)
    switcher_->resetState();
    
    // Clear any remaining stale packets from the old (camera) queue
    std::cout << "[Multiplexer] Clearing stale packets from camera queue..." << std::endl;
    size_t drained = 0;
    ts::TSPacket drain_pkt;
    while (camera_queue_->pop(drain_pkt, std::chrono::milliseconds(1))) {
        drained++;
    }
    if (drained > 0) {
        std::cout << "[Multiplexer] Drained " << drained << " stale packets from camera queue" << std::endl;
    }
    
    // Drain buffer starting from IDR (with SPS/PPS injection if needed)
    drainBufferFromIDR(switch_buffer_, idr_packet_index, Source::LIVE, needs_sps_pps_injection);
    
    switch_buffer_.clear();
    switch_state_ = SwitchState::NONE;
    input_switching_ = false;
    
    std::cout << "[Multiplexer] Drone input switch complete!" << std::endl;
    return true;
}