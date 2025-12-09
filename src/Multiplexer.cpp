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
      live_stream_ready_(false),
      initial_privacy_mode_(false),
      packets_processed_(0),
      live_idr_timeout_ms_(config.getLiveIdrTimeoutMs()),
      fallback_idr_timeout_ms_(config.getFallbackIdrTimeoutMs()) {
    std::cout << "[Multiplexer] Configured with IDR timeouts: live=" << live_idr_timeout_ms_
              << "ms, fallback=" << fallback_idr_timeout_ms_ << "ms" << std::endl;
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
    
    // Create TCP receivers (no queues needed - TCPReceiver has internal rolling buffer)
    
    if (input_source_manager_->isCamera()) {
        // Camera mode: Use TCP receiver (from ffmpeg-srt-live)
        std::cout << "[Multiplexer] Creating Camera TCP receiver for port " << config_.getLiveTcpPort() << std::endl;
        camera_tcp_receiver_ = std::make_unique<TCPReceiver>(
            "Camera", "ffmpeg-srt-live", config_.getLiveTcpPort());
    } else {
        // Drone mode: Use RTMP receiver with auto-reconnect (kept for dual-source support)
        std::cout << "[Multiplexer] Creating Drone RTMP receiver for URL: " << config_.getDroneRtmpUrl() << std::endl;
        std::cout << "[Multiplexer] Drone reconnect config: initial=" << config_.getDroneReconnectInitialMs()
                  << "ms, max=" << config_.getDroneReconnectMaxMs()
                  << "ms, backoff=" << config_.getDroneReconnectBackoff() << "x" << std::endl;
        // Note: Drone still needs a queue - create one
        live_queue_ = std::make_unique<TSPacketQueue>(config_.getTsQueueSize());
        drone_receiver_ = std::make_unique<RTMPReceiver>(
            "Drone", config_.getDroneRtmpUrl(), *live_queue_,
            config_.getDroneReconnectInitialMs(),
            config_.getDroneReconnectMaxMs(),
            config_.getDroneReconnectBackoff());
    }
    
    // Fallback receiver (always TCP)
    std::cout << "[Multiplexer] Creating Fallback TCP receiver for port " << config_.getFallbackTcpPort() << std::endl;
    fallback_tcp_receiver_ = std::make_unique<TCPReceiver>(
        "Fallback", "ffmpeg-fallback", config_.getFallbackTcpPort());
    
    // Create analyzers
    live_analyzer_ = std::make_unique<TSAnalyzer>();
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
    
    // Create RTMP output with configurable pacing
    uint32_t pacing_us = config_.getRtmpPacingUs();
    std::cout << "[Multiplexer] Creating RTMPOutput with pacing=" << pacing_us << "µs" << std::endl;
    rtmp_output_ = std::make_unique<RTMPOutput>(config_.getRtmpUrl(), pacing_us);
    
    // Create SPS/PPS injector for splice points
    sps_pps_injector_ = std::make_unique<SPSPPSInjector>();
    
    // Start live input receiver (camera TCP or drone RTMP depending on configuration)
    if (camera_tcp_receiver_) {
        if (!camera_tcp_receiver_->start()) {
            std::cerr << "[Multiplexer] Failed to start camera TCP receiver" << std::endl;
            return false;
        }
    } else if (drone_receiver_) {
        if (!drone_receiver_->start()) {
            std::cerr << "[Multiplexer] Failed to start drone receiver" << std::endl;
            return false;
        }
    }
    
    // Start fallback TCP receiver
    if (!fallback_tcp_receiver_->start()) {
        std::cerr << "[Multiplexer] Failed to start fallback TCP receiver" << std::endl;
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
    std::cout << "[Multiplexer] Stopping input receivers..." << std::endl;
    if (camera_tcp_receiver_) camera_tcp_receiver_->stop();
    if (drone_receiver_) drone_receiver_->stop();
    if (fallback_tcp_receiver_) fallback_tcp_receiver_->stop();
    
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
    if (camera_tcp_receiver_) {
        std::cout << "  Camera packets received: " << camera_tcp_receiver_->getPacketsReceived() << std::endl;
    }
    if (drone_receiver_) {
        std::cout << "  Drone packets received: " << drone_receiver_->getPacketsReceived() << std::endl;
    }
    std::cout << "  Fallback packets received: " << fallback_tcp_receiver_->getPacketsReceived() << std::endl;
    std::cout << "  RTMP packets written: " << rtmp_output_->getPacketsWritten() << std::endl;
    
    auto shutdown_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - shutdown_start
    );
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] Shutdown complete (" << shutdown_duration.count() << "ms)" << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
}

bool Multiplexer::analyzeStreams() {
    std::cout << "[Multiplexer] Analyzing streams using TCP receivers..." << std::endl;
    
    // Wait for fallback stream (required)
    std::cout << "[Multiplexer] Waiting for fallback TCP stream..." << std::endl;
    fallback_tcp_receiver_->waitForStreamInfo();
    fallback_tcp_receiver_->waitForIDR();
    
    StreamInfo fallback_info = fallback_tcp_receiver_->getStreamInfo();
    std::cout << "[Multiplexer] Fallback stream ready!" << std::endl;
    std::cout << "  Video PID: " << fallback_info.video_pid << std::endl;
    std::cout << "  Audio PID: " << fallback_info.audio_pid << std::endl;
    std::cout << "  PMT PID: " << fallback_info.pmt_pid << std::endl;
    
    // Extract timestamp bases from fallback
    if (!fallback_tcp_receiver_->extractTimestampBases()) {
        std::cerr << "[Multiplexer] Warning: Could not extract fallback timestamp bases" << std::endl;
    }
    
    // Try to get live stream if using camera (optional)
    StreamInfo live_info;
    if (camera_tcp_receiver_) {
        std::cout << "[Multiplexer] Checking for live TCP stream..." << std::endl;
        
        // Try to wait for stream info with short timeout approach
        // Use a simple check: is receiver connected and has stream info?
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        
        if (camera_tcp_receiver_->isConnected()) {
            camera_tcp_receiver_->waitForStreamInfo();
            camera_tcp_receiver_->waitForIDR();
            
            live_info = camera_tcp_receiver_->getStreamInfo();
            if (live_info.initialized) {
                std::cout << "[Multiplexer] Live stream detected during initialization" << std::endl;
                std::cout << "  Video PID: " << live_info.video_pid << std::endl;
                std::cout << "  Audio PID: " << live_info.audio_pid << std::endl;
                std::cout << "  PMT PID: " << live_info.pmt_pid << std::endl;
                
                // Extract timestamp bases from live
                if (!camera_tcp_receiver_->extractTimestampBases()) {
                    std::cerr << "[Multiplexer] Warning: Could not extract live timestamp bases" << std::endl;
                }
                
                // Initialize PID mapper with both streams
                pid_mapper_->initialize(live_info, fallback_info);
                live_stream_ready_ = true;
                std::cout << "[Multiplexer] Both streams ready" << std::endl;
            }
        }
        
        if (!live_info.initialized) {
            std::cout << "[Multiplexer] Live stream not available - will be detected dynamically later" << std::endl;
            live_stream_ready_ = false;
        }
    } else if (drone_receiver_) {
        // Drone mode - try to analyze from queue
        std::cout << "[Multiplexer] Live stream will be detected dynamically when drone connects" << std::endl;
        live_stream_ready_ = false;
    }
    
    return true;
}

bool Multiplexer::analyzeLiveStreamDynamically() {
    // This method is only used for drone RTMP mode
    // For camera TCP, stream discovery happens automatically in TCPReceiver background thread
    if (!drone_receiver_ || !live_queue_) {
        std::cout << "[Multiplexer] analyzeLiveStreamDynamically() not supported in TCP camera mode" << std::endl;
        return false;
    }
    
    static int analysis_attempt_count = 0;
    analysis_attempt_count++;
    
    std::cout << "[Multiplexer] ================================================" << std::endl;
    std::cout << "[Multiplexer] Attempting to detect drone stream (attempt #"
              << analysis_attempt_count << ")" << std::endl;
    std::cout << "[Multiplexer] ================================================" << std::endl;
    
    size_t queue_size = live_queue_->size();
    size_t max_packets_to_analyze = std::max(queue_size, static_cast<size_t>(500));
    
    live_analyzer_->reset();
    
    // Analyze accumulated packets from the queue
    size_t live_packets_analyzed = 0;
    ts::TSPacket packet;
    
    while (live_packets_analyzed < max_packets_to_analyze && running_.load()) {
        if (!live_queue_->pop(packet, std::chrono::milliseconds(50))) {
            break;
        }
        
        live_analyzer_->analyzePacket(packet);
        live_packets_analyzed++;
        
        if (live_analyzer_->hasValidMediaData()) {
            break;
        }
    }
    
    if (!live_analyzer_->hasValidMediaData()) {
        return false;
    }
    
    const auto& live_info = live_analyzer_->getStreamInfo();
    const auto& fallback_info = fallback_tcp_receiver_->getStreamInfo();
    pid_mapper_->initialize(live_info, fallback_info);
    
    live_stream_ready_ = true;
    std::cout << "[Multiplexer] Drone stream detected and ready!" << std::endl;
    
    return true;
}

void Multiplexer::processLoop() {
    std::cout << "[Multiplexer] Starting TCP-based processing loop" << std::endl;
    
    uint64_t log_interval = 1000;
    
    // Initialize with fallback stream (already connected and ready from analyzeStreams)
    current_pts_base_ = fallback_tcp_receiver_->getPTSBase();
    current_pcr_base_ = fallback_tcp_receiver_->getPCRBase();
    current_pcr_pts_alignment_ = fallback_tcp_receiver_->getPCRPTSAlignmentOffset();
    
    std::cout << "[Multiplexer] Initial fallback bases: PTS=" << current_pts_base_
              << ", PCR=" << current_pcr_base_
              << ", alignment=" << current_pcr_pts_alignment_ << std::endl;
    
    // Start consuming from the IDR point in fallback
    fallback_tcp_receiver_->initConsumptionFromIndex(0);
    
    Mode current_mode = switcher_->getMode();
    TCPReceiver* active_receiver = fallback_tcp_receiver_.get();
    Source active_source = Source::FALLBACK;
    
    // If we start in LIVE mode (privacy disabled and live ready), switch to live
    if (current_mode == Mode::LIVE && live_stream_ready_.load()) {
        std::cout << "[Multiplexer] Starting in LIVE mode - switching to camera receiver" << std::endl;
        active_receiver = camera_tcp_receiver_.get();
        active_source = Source::LIVE;
        current_pts_base_ = camera_tcp_receiver_->getPTSBase();
        current_pcr_base_ = camera_tcp_receiver_->getPCRBase();
        current_pcr_pts_alignment_ = camera_tcp_receiver_->getPCRPTSAlignmentOffset();
        camera_tcp_receiver_->initConsumptionFromIndex(0);
    }
    
    while (running_.load()) {
        // Receive packets from active receiver
        auto packets = active_receiver->receivePackets(100, 100);
        
        if (packets.empty()) {
            // Check for timeout in LIVE mode
            if (current_mode == Mode::LIVE && switcher_->checkLiveTimeout()) {
                std::cout << "[Multiplexer] Live timeout - switching to FALLBACK" << std::endl;
                if (switchToFallbackAtIDR()) {
                    current_mode = Mode::FALLBACK;
                    active_receiver = fallback_tcp_receiver_.get();
                    active_source = Source::FALLBACK;
                }
            }
            continue;
        }
        
        // Process received packets
        for (auto& packet : packets) {
            // Update live timestamp tracking
            if (current_mode == Mode::LIVE) {
                switcher_->updateLiveTimestamp();
            }
            
            processPacket(packet, active_source);
            packets_processed_++;
        }
        
        // Periodic logging
        if (packets_processed_ % log_interval == 0) {
            std::cout << "[Multiplexer] Processed " << packets_processed_.load()
                      << " packets. Mode: " << (current_mode == Mode::LIVE ? "LIVE" : "FALLBACK")
                      << ", Live ready: " << (live_stream_ready_.load() ? "Yes" : "No") << std::endl;
        }
        
        // Check if camera stream became ready dynamically (if not already ready)
        if (current_mode == Mode::FALLBACK && !live_stream_ready_.load() && camera_tcp_receiver_) {
            if (camera_tcp_receiver_->isConnected() && camera_tcp_receiver_->isStreamReady()) {
                std::cout << "[Multiplexer] ================================================" << std::endl;
                std::cout << "[Multiplexer] Camera stream became ready dynamically!" << std::endl;
                std::cout << "[Multiplexer] ================================================" << std::endl;
                
                // Extract stream info and timestamp bases
                StreamInfo camera_info = camera_tcp_receiver_->getStreamInfo();
                std::cout << "[Multiplexer] Camera stream info:" << std::endl;
                std::cout << "  Video PID: " << camera_info.video_pid << std::endl;
                std::cout << "  Audio PID: " << camera_info.audio_pid << std::endl;
                std::cout << "  PMT PID: " << camera_info.pmt_pid << std::endl;
                
                // Extract timestamp bases from camera
                if (!camera_tcp_receiver_->extractTimestampBases()) {
                    std::cerr << "[Multiplexer] Warning: Could not extract camera timestamp bases" << std::endl;
                } else {
                    std::cout << "[Multiplexer] Camera timestamp bases extracted:" << std::endl;
                    std::cout << "  PTS base: " << camera_tcp_receiver_->getPTSBase() << std::endl;
                    std::cout << "  PCR base: " << camera_tcp_receiver_->getPCRBase() << std::endl;
                }
                
                // Initialize PID mapper with both streams
                StreamInfo fallback_info = fallback_tcp_receiver_->getStreamInfo();
                pid_mapper_->initialize(camera_info, fallback_info);
                std::cout << "[Multiplexer] PID mapper initialized with camera and fallback streams" << std::endl;
                
                // Mark camera stream as ready
                live_stream_ready_ = true;
                std::cout << "[Multiplexer] Camera stream is now ready for switching!" << std::endl;
                
                // Auto-switch to LIVE mode if not in privacy mode
                if (!switcher_->isPrivacyMode()) {
                    std::cout << "[Multiplexer] Auto-switching to LIVE mode..." << std::endl;
                    if (switchToLiveAtIDR()) {
                        current_mode = Mode::LIVE;
                        active_receiver = camera_tcp_receiver_.get();
                        active_source = Source::LIVE;
                        std::cout << "[Multiplexer] Auto-switch to LIVE complete!" << std::endl;
                    } else {
                        std::cout << "[Multiplexer] Auto-switch to LIVE failed" << std::endl;
                    }
                } else {
                    std::cout << "[Multiplexer] Privacy mode enabled - staying in FALLBACK" << std::endl;
                }
                std::cout << "[Multiplexer] ================================================" << std::endl;
            }
        }
        
        // Check for mode switch
        if (current_mode == Mode::FALLBACK && live_stream_ready_.load()) {
            // Check if we should return to live
            if (camera_tcp_receiver_ && camera_tcp_receiver_->isConnected() && switcher_->tryReturnToLive()) {
                std::cout << "[Multiplexer] Returning to LIVE mode" << std::endl;
                if (switchToLiveAtIDR()) {
                    current_mode = Mode::LIVE;
                    active_receiver = camera_tcp_receiver_.get();
                    active_source = Source::LIVE;
                }
            }
        }
    }
    
    std::cout << "[Multiplexer] Processing loop ended" << std::endl;
}

void Multiplexer::processPacket(ts::TSPacket& packet, Source source) {
    // Rebase timestamps using tcp_main.cpp approach
    // Use current stream's bases and global offsets from TimestampManager
    timestamp_mgr_->rebasePacket(packet, current_pts_base_, current_pcr_base_, current_pcr_pts_alignment_);
    
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
    if (!camera_tcp_receiver_) {
        std::cerr << "[Multiplexer] No camera receiver available for switch to live" << std::endl;
        return false;
    }
    
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] IDR-AWARE SWITCH TO LIVE: Using TCP approach" << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
    
    // Reset camera receiver for new loop - triggers new IDR detection
    camera_tcp_receiver_->resetForNewLoop();
    
    // Wait for fresh IDR
    std::cout << "[Multiplexer] Waiting for fresh IDR frame..." << std::endl;
    camera_tcp_receiver_->waitForIDR();
    std::cout << "[Multiplexer] Fresh IDR frame detected" << std::endl;
    
    // Re-extract timestamp bases from current stream position
    if (!camera_tcp_receiver_->extractTimestampBases()) {
        std::cerr << "[Multiplexer] Warning: Could not extract timestamp bases" << std::endl;
    }
    
    // Update current bases for timestamp rebasing
    current_pts_base_ = camera_tcp_receiver_->getPTSBase();
    current_pcr_base_ = camera_tcp_receiver_->getPCRBase();
    current_pcr_pts_alignment_ = camera_tcp_receiver_->getPCRPTSAlignmentOffset();
    
    std::cout << "[Multiplexer] Updated bases: PTS=" << current_pts_base_
              << ", PCR=" << current_pcr_base_
              << ", alignment=" << current_pcr_pts_alignment_ << std::endl;
    
    // Get buffered packets from IDR
    auto buffered = camera_tcp_receiver_->getBufferedPacketsFromIDR();
    std::cout << "[Multiplexer] Processing " << buffered.size() << " buffered packets from IDR" << std::endl;
    
    // Inject PAT/PMT before processing buffered packets
    // TODO: Need to create PAT/PMT from StreamInfo
    
    // Inject SPS/PPS if available
    std::vector<uint8_t> sps = camera_tcp_receiver_->getSPSData();
    std::vector<uint8_t> pps = camera_tcp_receiver_->getPPSData();
    if (!sps.empty() && !pps.empty()) {
        std::cout << "[Multiplexer] Injecting SPS/PPS before IDR" << std::endl;
        // TODO: Use createSPSPPSPackets
    }
    
    // Switch to LIVE mode
    switcher_->setMode(Mode::LIVE);
    switcher_->updateLiveTimestamp();
    
    // Process buffered packets
    for (auto& pkt : buffered) {
        processPacket(pkt, Source::LIVE);
        packets_processed_++;
    }
    
    // Initialize consumption from end of snapshot
    camera_tcp_receiver_->initConsumptionFromIndex(camera_tcp_receiver_->getLastSnapshotEnd());
    
    std::cout << "[Multiplexer] Switch to LIVE complete" << std::endl;
    return true;
}

bool Multiplexer::switchToFallbackAtIDR() {
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] IDR-AWARE SWITCH TO FALLBACK: Using TCP approach" << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
    
    // Reset fallback receiver for new loop - triggers new IDR detection
    fallback_tcp_receiver_->resetForNewLoop();
    
    // Wait for fresh IDR
    std::cout << "[Multiplexer] Waiting for fresh fallback IDR frame..." << std::endl;
    fallback_tcp_receiver_->waitForIDR();
    std::cout << "[Multiplexer] Fresh fallback IDR frame detected" << std::endl;
    
    // Re-extract timestamp bases from current stream position
    if (!fallback_tcp_receiver_->extractTimestampBases()) {
        std::cerr << "[Multiplexer] Warning: Could not extract fallback timestamp bases" << std::endl;
    }
    
    // Update current bases for timestamp rebasing
    current_pts_base_ = fallback_tcp_receiver_->getPTSBase();
    current_pcr_base_ = fallback_tcp_receiver_->getPCRBase();
    current_pcr_pts_alignment_ = fallback_tcp_receiver_->getPCRPTSAlignmentOffset();
    
    std::cout << "[Multiplexer] Updated fallback bases: PTS=" << current_pts_base_
              << ", PCR=" << current_pcr_base_
              << ", alignment=" << current_pcr_pts_alignment_ << std::endl;
    
    // Get buffered packets from IDR
    auto buffered = fallback_tcp_receiver_->getBufferedPacketsFromIDR();
    std::cout << "[Multiplexer] Processing " << buffered.size() << " buffered packets from fallback IDR" << std::endl;
    
    // Inject PAT/PMT before processing buffered packets
    // TODO: Need to create PAT/PMT from StreamInfo
    
    // Inject SPS/PPS if available
    std::vector<uint8_t> sps = fallback_tcp_receiver_->getSPSData();
    std::vector<uint8_t> pps = fallback_tcp_receiver_->getPPSData();
    if (!sps.empty() && !pps.empty()) {
        std::cout << "[Multiplexer] Injecting SPS/PPS before fallback IDR" << std::endl;
        // TODO: Use createSPSPPSPackets
    }
    
    // Switch to FALLBACK mode
    switcher_->setMode(Mode::FALLBACK);
    
    // Process buffered packets
    for (auto& pkt : buffered) {
        processPacket(pkt, Source::FALLBACK);
        packets_processed_++;
    }
    
    // Initialize consumption from end of snapshot
    fallback_tcp_receiver_->initConsumptionFromIndex(fallback_tcp_receiver_->getLastSnapshotEnd());
    
    // Reset live stream ready flag so we re-analyze when live comes back
    live_stream_ready_ = false;
    
    std::cout << "[Multiplexer] Switch to FALLBACK complete" << std::endl;
    return true;
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
        
        TSAnalyzer* analyzer = (source == Source::LIVE) ? live_analyzer_.get() : fallback_analyzer_.get();
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
    TSAnalyzer* analyzer = (source == Source::LIVE) ? live_analyzer_.get() : fallback_analyzer_.get();
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
    TSAnalyzer* analyzer = (source == Source::LIVE) ? live_analyzer_.get() : fallback_analyzer_.get();
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