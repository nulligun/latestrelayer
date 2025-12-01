#include "Multiplexer.h"
#include <iostream>
#include <thread>
#include <csignal>
#include <fstream>

Multiplexer::Multiplexer(const Config& config)
    : config_(config),
      http_client_(std::make_shared<HttpClient>(CONTROLLER_URL)),
      running_(false),
      initialized_(false),
      live_stream_ready_(false),
      drone_stream_ready_(false),
      initial_privacy_mode_(false),
      current_input_source_(InputSource::CAMERA),
      packets_processed_(0) {
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
    
    // Start HTTP server for receiving callbacks (privacy mode changes, health status)
    http_server_ = std::make_unique<HttpServer>(HTTP_SERVER_PORT);
    http_server_->setPrivacyCallback([this](bool enabled) {
        onPrivacyModeChange(enabled);
    });
    
    // Set up input source callbacks
    http_server_->setInputSourceCallback([this](const std::string& source) {
        onInputSourceChange(source);
    });
    
    http_server_->setInputSourceGetCallback([this]() -> InputSourceStatus {
        InputSourceStatus status;
        status.current_source = (current_input_source_.load() == InputSource::CAMERA) ? "camera" : "drone";
        return status;
    });
    
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
    
    // Load persisted input source
    loadInputSource();
    
    // Create packet queues
    live_queue_ = std::make_unique<TSPacketQueue>(10000);
    fallback_queue_ = std::make_unique<TSPacketQueue>(10000);
    drone_queue_ = std::make_unique<TSPacketQueue>(10000);
    
    // Create UDP receivers
    live_receiver_ = std::make_unique<UDPReceiver>(
        "Live", config_.getLiveUdpPort(), *live_queue_);
    fallback_receiver_ = std::make_unique<UDPReceiver>(
        "Fallback", config_.getFallbackUdpPort(), *fallback_queue_);
    drone_receiver_ = std::make_unique<UDPReceiver>(
        "Drone", config_.getDroneUdpPort(), *drone_queue_);
    
    // Create analyzers
    live_analyzer_ = std::make_unique<TSAnalyzer>();
    fallback_analyzer_ = std::make_unique<TSAnalyzer>();
    drone_analyzer_ = std::make_unique<TSAnalyzer>();
    
    // Create processing components
    timestamp_mgr_ = std::make_unique<TimestampManager>();
    pid_mapper_ = std::make_unique<PIDMapper>();
    switcher_ = std::make_unique<StreamSwitcher>(config_.getMaxLiveGapMs(), http_client_);
    
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
    
    // Create RTMP output
    rtmp_output_ = std::make_unique<RTMPOutput>(config_.getRtmpUrl());
    
    // Start UDP receivers
    if (!live_receiver_->start()) {
        std::cerr << "[Multiplexer] Failed to start live receiver" << std::endl;
        return false;
    }
    
    if (!fallback_receiver_->start()) {
        std::cerr << "[Multiplexer] Failed to start fallback receiver" << std::endl;
        return false;
    }
    
    if (!drone_receiver_->start()) {
        std::cerr << "[Multiplexer] Failed to start drone receiver" << std::endl;
        return false;
    }
    
    // Analyze streams to get PID information
    // This will wait indefinitely for fallback stream
    if (!analyzeStreams()) {
        std::cerr << "[Multiplexer] Failed to analyze streams" << std::endl;
        return false;
    }
    
    // Set initial mode based on privacy mode and live stream availability
    if (initial_privacy_mode_.load()) {
        std::cout << "[Multiplexer] Starting in FALLBACK mode (privacy mode enabled)" << std::endl;
        switcher_->setMode(Mode::FALLBACK);
    } else if (!live_stream_ready_.load()) {
        std::cout << "[Multiplexer] Starting in FALLBACK mode (no live stream detected)" << std::endl;
        switcher_->setMode(Mode::FALLBACK);
    } else {
        std::cout << "[Multiplexer] Starting in LIVE mode" << std::endl;
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
    std::cout << "[Multiplexer] Stopping UDP receivers..." << std::endl;
    if (live_receiver_) live_receiver_->stop();
    if (fallback_receiver_) fallback_receiver_->stop();
    if (drone_receiver_) drone_receiver_->stop();
    
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
    std::cout << "  Packets processed: " << packets_processed_.load() << std::endl;
    std::cout << "  Live packets received: " << live_receiver_->getPacketsReceived() << std::endl;
    std::cout << "  Drone packets received: " << drone_receiver_->getPacketsReceived() << std::endl;
    std::cout << "  Fallback packets received: " << fallback_receiver_->getPacketsReceived() << std::endl;
    std::cout << "  RTMP packets written: " << rtmp_output_->getPacketsWritten() << std::endl;
    std::cout << "  Input source: " << (current_input_source_.load() == InputSource::CAMERA ? "CAMERA" : "DRONE") << std::endl;
    
    auto shutdown_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - shutdown_start
    );
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] Shutdown complete (" << shutdown_duration.count() << "ms)" << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
}

bool Multiplexer::analyzeStreams() {
    std::cout << "[Multiplexer] Analyzing streams..." << std::endl;
    
    ts::TSPacket packet;
    auto last_status_log = std::chrono::steady_clock::now();
    
    // Try to analyze live stream packets (optional)
    std::cout << "[Multiplexer] Checking for live stream..." << std::endl;
    int live_packets_analyzed = 0;
    while (live_packets_analyzed < 100 && live_queue_->pop(packet, std::chrono::milliseconds(100))) {
        live_analyzer_->analyzePacket(packet);
        live_packets_analyzed++;
        
        if (live_analyzer_->isInitialized()) {
            break;
        }
    }
    
    const auto& live_info = live_analyzer_->getStreamInfo();
    if (live_info.initialized) {
        std::cout << "[Multiplexer] Live stream detected during initialization" << std::endl;
    } else {
        std::cout << "[Multiplexer] Live stream not available - will be detected dynamically later" << std::endl;
    }
    
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
    
    // Check live stream status and initialize PID mapper if available
    if (live_info.initialized) {
        std::cout << "[Multiplexer] Live stream:" << std::endl;
        std::cout << "  Video PID: " << live_info.video_pid << std::endl;
        std::cout << "  Audio PID: " << live_info.audio_pid << std::endl;
        std::cout << "  PMT PID: " << live_info.pmt_pid << std::endl;
        
        // Initialize PID mapper with both streams
        pid_mapper_->initialize(live_info, fallback_info);
        live_stream_ready_ = true;
        std::cout << "[Multiplexer] Both streams ready" << std::endl;
    } else {
        std::cout << "[Multiplexer] Starting in fallback-only mode" << std::endl;
        std::cout << "[Multiplexer] Live stream will be detected dynamically when SRT connects" << std::endl;
        live_stream_ready_ = false;
    }
    
    return true;
}

bool Multiplexer::analyzeLiveStreamDynamically() {
    std::cout << "[Multiplexer] Attempting to detect live stream..." << std::endl;
    
    // Get the current queue size - we'll analyze accumulated packets instead of clearing them
    // This is more efficient as accumulated packets contain PAT, PMT, and media data we need
    size_t queue_size = live_queue_->size();
    
    // Use adaptive packet budget: analyze at least 500 packets, or queue size if larger
    // This ensures we have enough packets to find PAT/PMT and validate media
    size_t max_packets_to_analyze = std::max(queue_size, static_cast<size_t>(500));
    
    std::cout << "[Multiplexer] Analyzing up to " << max_packets_to_analyze
              << " packets (queue size: " << queue_size << ")" << std::endl;
    
    // Reset the analyzer to start fresh (clears old PID state from previous connection)
    // Note: reset() now re-registers PID_PAT for continued PAT parsing
    live_analyzer_->reset();
    
    // Analyze accumulated packets from the queue
    size_t live_packets_analyzed = 0;
    ts::TSPacket packet;
    
    // Use a longer timeout (50ms) to allow real-time packets to arrive
    while (live_packets_analyzed < max_packets_to_analyze && running_.load() &&
           live_queue_->pop(packet, std::chrono::milliseconds(50))) {
        live_analyzer_->analyzePacket(packet);
        live_packets_analyzed++;
        
        // Log progress periodically
        if (live_packets_analyzed % 100 == 0) {
            const auto& info = live_analyzer_->getStreamInfo();
            std::cout << "[Multiplexer] Analysis progress: " << live_packets_analyzed << " packets"
                      << ", initialized=" << (info.initialized ? "yes" : "no")
                      << ", video=" << info.valid_video_packets
                      << "/" << StreamInfo::MIN_VALID_VIDEO_PACKETS
                      << ", audio=" << info.valid_audio_packets
                      << "/" << StreamInfo::MIN_VALID_AUDIO_PACKETS << std::endl;
        }
        
        // Check if we have valid media data (not just PSI tables)
        if (live_analyzer_->hasValidMediaData()) {
            std::cout << "[Multiplexer] Valid media data found after "
                      << live_packets_analyzed << " packets" << std::endl;
            break;
        }
    }
    
    // Check for shutdown signal
    if (!running_.load()) {
        return false;
    }
    
    const auto& live_info = live_analyzer_->getStreamInfo();
    
    // Check for valid media data instead of just initialization
    if (!live_analyzer_->hasValidMediaData()) {
        // Log current validation status
        if (live_info.initialized) {
            std::cout << "[Multiplexer] Live stream PSI detected but waiting for valid media packets..." << std::endl;
            std::cout << "  Analyzed: " << live_packets_analyzed << " packets" << std::endl;
            std::cout << "  Video packets: " << live_info.valid_video_packets
                      << "/" << StreamInfo::MIN_VALID_VIDEO_PACKETS << std::endl;
            std::cout << "  Audio packets: " << live_info.valid_audio_packets
                      << "/" << StreamInfo::MIN_VALID_AUDIO_PACKETS << std::endl;
        } else {
            std::cout << "[Multiplexer] No PSI tables found after "
                      << live_packets_analyzed << " packets" << std::endl;
        }
        return false;
    }
    
    std::cout << "[Multiplexer] Live stream detected!" << std::endl;
    std::cout << "  Video PID: " << live_info.video_pid << std::endl;
    std::cout << "  Audio PID: " << live_info.audio_pid << std::endl;
    std::cout << "  PMT PID: " << live_info.pmt_pid << std::endl;
    std::cout << "  Valid video packets: " << live_info.valid_video_packets << std::endl;
    std::cout << "  Valid audio packets: " << live_info.valid_audio_packets << std::endl;
    
    // Reinitialize PID mapper with live stream info
    const auto& fallback_info = fallback_analyzer_->getStreamInfo();
    pid_mapper_->initialize(live_info, fallback_info);
    
    live_stream_ready_ = true;
    
    return true;
}

void Multiplexer::processLoop() {
    ts::TSPacket packet;
    uint64_t log_interval = 1000; // Log every 1000 packets
    uint64_t live_check_interval = 100; // Check for live stream every 100 packets
    
    while (running_.load()) {
        Mode current_mode = switcher_->getMode();
        InputSource input_source = current_input_source_.load();
        
        // Get the active queue and analyzer based on input source
        TSPacketQueue* active_live_queue = getActiveLiveQueue();
        TSAnalyzer* active_live_analyzer = getActiveLiveAnalyzer();
        bool active_stream_ready = isActiveLiveStreamReady();
        
        // Periodically check for active live stream if not yet detected
        if (!active_stream_ready && packets_processed_ % live_check_interval == 0) {
            if (!active_live_queue->empty()) {
                bool detected = false;
                
                if (input_source == InputSource::CAMERA) {
                    std::cout << "[Multiplexer] Camera packets detected, attempting analysis..." << std::endl;
                    detected = analyzeLiveStreamDynamically();
                } else {
                    std::cout << "[Multiplexer] Drone packets detected, attempting analysis..." << std::endl;
                    detected = analyzeDroneStreamDynamically();
                }
                
                if (detected) {
                    std::cout << "[Multiplexer] " << (input_source == InputSource::CAMERA ? "Camera" : "Drone")
                              << " stream successfully initialized!" << std::endl;
                    std::cout << "[Multiplexer] Switching to LIVE mode" << std::endl;
                    
                    // Switch to live mode
                    switcher_->setMode(Mode::LIVE);
                    switcher_->updateLiveTimestamp();
                    
                    // Calculate timestamp offset for live stream
                    ts::TSPacket first_live_packet;
                    if (active_live_queue->pop(first_live_packet, std::chrono::milliseconds(10))) {
                        TimestampInfo ts_info = active_live_analyzer->extractTimestamps(first_live_packet);
                        timestamp_mgr_->onSourceSwitch(Source::LIVE, ts_info);
                        processPacket(first_live_packet, Source::LIVE);
                        packets_processed_++;
                    }
                    continue;
                }
            }
        }
        
        // Select queue based on current mode and input source
        TSPacketQueue* queue = (current_mode == Mode::LIVE) ? active_live_queue : fallback_queue_.get();
        Source source = (current_mode == Mode::LIVE) ? Source::LIVE : Source::FALLBACK;
        
        // Try to get a packet (with short timeout for responsive shutdown)
        if (queue->pop(packet, std::chrono::milliseconds(1))) {
            // Update live timestamp if from live source
            if (current_mode == Mode::LIVE) {
                switcher_->updateLiveTimestamp();
            }
            
            // Process the packet - use appropriate analyzer based on input source
            if (source == Source::LIVE) {
                TimestampInfo ts_info = active_live_analyzer->extractTimestamps(packet);
                timestamp_mgr_->trackLiveTimestamps(ts_info);
            } else {
                processPacket(packet, source);
            }
            
            // Write to RTMP output
            if (source == Source::LIVE) {
                rtmp_output_->writePacket(packet);
            }
            
            packets_processed_++;
            
            // Periodic logging
            if (packets_processed_ % log_interval == 0) {
                std::cout << "[Multiplexer] Processed " << packets_processed_.load()
                          << " packets. Mode: " << (current_mode == Mode::LIVE ? "LIVE" : "FALLBACK")
                          << ", Input: " << (input_source == InputSource::CAMERA ? "CAMERA" : "DRONE")
                          << ", Queue size: " << queue->size()
                          << ", Stream ready: " << (active_stream_ready ? "Yes" : "No") << std::endl;
            }
            
            // Check for mode switch after processing
            if (current_mode == Mode::LIVE) {
                // Check if we need to switch to fallback
                if (switcher_->checkLiveTimeout()) {
                    std::cout << "[Multiplexer] Switched to FALLBACK mode (live timeout)" << std::endl;
                    
                    // Reset the appropriate stream_ready flag so we can re-detect the stream
                    if (input_source == InputSource::CAMERA) {
                        live_stream_ready_ = false;
                        std::cout << "[Multiplexer] Camera stream marked as not ready - will re-analyze when packets arrive" << std::endl;
                    } else {
                        drone_stream_ready_ = false;
                        std::cout << "[Multiplexer] Drone stream marked as not ready - will re-analyze when packets arrive" << std::endl;
                    }
                }
            } else {
                // In FALLBACK mode, check if live packets are arriving
                // If so, increment the counter to track consecutive live packets
                if (!active_live_queue->empty()) {
                    switcher_->updateLiveTimestamp();
                }
                
                // Try to return to live if packets are available and stream is ready
                if (isActiveLiveStreamReady() && !active_live_queue->empty() && switcher_->tryReturnToLive()) {
                    std::cout << "[Multiplexer] Switched to LIVE mode ("
                              << (input_source == InputSource::CAMERA ? "camera" : "drone")
                              << " stream restored)" << std::endl;
                    
                    // Calculate new timestamp offset for live stream
                    ts::TSPacket first_live_packet;
                    if (active_live_queue->pop(first_live_packet, std::chrono::milliseconds(10))) {
                        TimestampInfo ts_info = active_live_analyzer->extractTimestamps(first_live_packet);
                        timestamp_mgr_->onSourceSwitch(Source::LIVE, ts_info);
                        processPacket(first_live_packet, Source::LIVE);
                        packets_processed_++;
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
        ? live_analyzer_->extractTimestamps(packet)
        : fallback_analyzer_->extractTimestamps(packet);
    
    if (source == Source::LIVE) {
        // LIVE: Passthrough mode - only track timestamps, don't modify packet
        // Camera is the canonical time source
        timestamp_mgr_->trackLiveTimestamps(ts_info);
        // No PID remapping needed (live PIDs are already correct)
        // No continuity counter fix (preserve original stream integrity)
    } else {
        // FALLBACK: Full processing - adjust timestamps to continue from live timeline
        timestamp_mgr_->adjustPacket(packet, source, ts_info);
        // Remap PIDs to match live stream PIDs
        pid_mapper_->remapPacket(packet);
        // Fix continuity counters for seamless stream
        pid_mapper_->fixContinuityCounter(packet);
    }
    
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
            // Force switch to fallback immediately
            std::cout << "[Multiplexer] Forcing switch to FALLBACK mode due to privacy mode" << std::endl;
            switcher_->setMode(Mode::FALLBACK);
        }
    }
}

void Multiplexer::onInputSourceChange(const std::string& source) {
    InputSource new_source = (source == "drone") ? InputSource::DRONE : InputSource::CAMERA;
    InputSource old_source = current_input_source_.exchange(new_source);
    
    if (old_source != new_source) {
        std::cout << "[Multiplexer] Input source changed: "
                  << (old_source == InputSource::CAMERA ? "CAMERA" : "DRONE")
                  << " -> "
                  << (new_source == InputSource::CAMERA ? "CAMERA" : "DRONE") << std::endl;
        
        // Save the new input source to persistent storage
        saveInputSource();
        
        // If currently in LIVE mode, we need to switch based on stream availability
        if (switcher_->getMode() == Mode::LIVE) {
            // Check if the new source is ready
            if (!isActiveLiveStreamReady()) {
                std::cout << "[Multiplexer] New input source not ready, switching to FALLBACK mode" << std::endl;
                switcher_->setMode(Mode::FALLBACK);
            }
        }
    }
}

void Multiplexer::loadInputSource() {
    std::cout << "[Multiplexer] Loading persisted input source..." << std::endl;
    
    try {
        std::ifstream file(INPUT_SOURCE_FILE);
        if (file.is_open()) {
            std::string content((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
            file.close();
            
            // Simple JSON parsing for "source" field
            if (content.find("\"source\": \"drone\"") != std::string::npos ||
                content.find("\"source\":\"drone\"") != std::string::npos) {
                current_input_source_ = InputSource::DRONE;
                std::cout << "[Multiplexer] Loaded input source: DRONE" << std::endl;
            } else {
                current_input_source_ = InputSource::CAMERA;
                std::cout << "[Multiplexer] Loaded input source: CAMERA" << std::endl;
            }
        } else {
            std::cout << "[Multiplexer] No persisted input source found, defaulting to CAMERA" << std::endl;
            current_input_source_ = InputSource::CAMERA;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Multiplexer] Error loading input source: " << e.what() << std::endl;
        current_input_source_ = InputSource::CAMERA;
    }
}

void Multiplexer::saveInputSource() {
    std::cout << "[Multiplexer] Saving input source..." << std::endl;
    
    try {
        std::ofstream file(INPUT_SOURCE_FILE);
        if (file.is_open()) {
            std::string source = (current_input_source_.load() == InputSource::CAMERA) ? "camera" : "drone";
            file << "{\n  \"source\": \"" << source << "\"\n}\n";
            file.close();
            std::cout << "[Multiplexer] Saved input source: " << source << std::endl;
        } else {
            std::cerr << "[Multiplexer] Failed to open file for writing: " << INPUT_SOURCE_FILE << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Multiplexer] Error saving input source: " << e.what() << std::endl;
    }
}

TSPacketQueue* Multiplexer::getActiveLiveQueue() {
    return (current_input_source_.load() == InputSource::CAMERA)
           ? live_queue_.get()
           : drone_queue_.get();
}

TSAnalyzer* Multiplexer::getActiveLiveAnalyzer() {
    return (current_input_source_.load() == InputSource::CAMERA)
           ? live_analyzer_.get()
           : drone_analyzer_.get();
}

bool Multiplexer::isActiveLiveStreamReady() {
    return (current_input_source_.load() == InputSource::CAMERA)
           ? live_stream_ready_.load()
           : drone_stream_ready_.load();
}

bool Multiplexer::analyzeDroneStreamDynamically() {
    std::cout << "[Multiplexer] Attempting to detect drone stream..." << std::endl;
    
    // Get the current queue size
    size_t queue_size = drone_queue_->size();
    
    // Use adaptive packet budget
    size_t max_packets_to_analyze = std::max(queue_size, static_cast<size_t>(500));
    
    std::cout << "[Multiplexer] Analyzing up to " << max_packets_to_analyze
              << " drone packets (queue size: " << queue_size << ")" << std::endl;
    
    // Reset the analyzer to start fresh
    drone_analyzer_->reset();
    
    // Analyze accumulated packets from the queue
    size_t drone_packets_analyzed = 0;
    ts::TSPacket packet;
    
    // Use a longer timeout (50ms) to allow real-time packets to arrive
    while (drone_packets_analyzed < max_packets_to_analyze && running_.load() &&
           drone_queue_->pop(packet, std::chrono::milliseconds(50))) {
        drone_analyzer_->analyzePacket(packet);
        drone_packets_analyzed++;
        
        // Log progress periodically
        if (drone_packets_analyzed % 100 == 0) {
            const auto& info = drone_analyzer_->getStreamInfo();
            std::cout << "[Multiplexer] Drone analysis progress: " << drone_packets_analyzed << " packets"
                      << ", initialized=" << (info.initialized ? "yes" : "no")
                      << ", video=" << info.valid_video_packets
                      << "/" << StreamInfo::MIN_VALID_VIDEO_PACKETS
                      << ", audio=" << info.valid_audio_packets
                      << "/" << StreamInfo::MIN_VALID_AUDIO_PACKETS << std::endl;
        }
        
        // Check if we have valid media data
        if (drone_analyzer_->hasValidMediaData()) {
            std::cout << "[Multiplexer] Valid drone media data found after "
                      << drone_packets_analyzed << " packets" << std::endl;
            break;
        }
    }
    
    // Check for shutdown signal
    if (!running_.load()) {
        return false;
    }
    
    const auto& drone_info = drone_analyzer_->getStreamInfo();
    
    // Check for valid media data
    if (!drone_analyzer_->hasValidMediaData()) {
        if (drone_info.initialized) {
            std::cout << "[Multiplexer] Drone stream PSI detected but waiting for valid media packets..." << std::endl;
        } else {
            std::cout << "[Multiplexer] No drone PSI tables found after "
                      << drone_packets_analyzed << " packets" << std::endl;
        }
        return false;
    }
    
    std::cout << "[Multiplexer] Drone stream detected!" << std::endl;
    std::cout << "  Video PID: " << drone_info.video_pid << std::endl;
    std::cout << "  Audio PID: " << drone_info.audio_pid << std::endl;
    std::cout << "  PMT PID: " << drone_info.pmt_pid << std::endl;
    std::cout << "  Valid video packets: " << drone_info.valid_video_packets << std::endl;
    std::cout << "  Valid audio packets: " << drone_info.valid_audio_packets << std::endl;
    
    // Reinitialize PID mapper with drone stream info
    const auto& fallback_info = fallback_analyzer_->getStreamInfo();
    pid_mapper_->initialize(drone_info, fallback_info);
    
    drone_stream_ready_ = true;
    
    return true;
}