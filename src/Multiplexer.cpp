#include "Multiplexer.h"
#include "SPSPPSInjector.h"
#include <iostream>
#include <iomanip>
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
    http_server_->setInputSourceCallback([this](InputSource source) {
        requestInputSourceSwitch(source);
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
    
    // Create TCP receivers for both camera and drone (both always available for instant switching)
    std::cout << "[Multiplexer] Creating Camera TCP receiver for port " << config_.getLiveTcpPort() << std::endl;
    camera_tcp_receiver_ = std::make_unique<TCPReceiver>(
        "Camera", "ffmpeg-srt-live", config_.getLiveTcpPort());
    
    std::cout << "[Multiplexer] Creating Drone TCP receiver for port " << config_.getDroneTcpPort() << std::endl;
    drone_tcp_receiver_ = std::make_unique<TCPReceiver>(
        "Drone", "ffmpeg-rtmp-live", config_.getDroneTcpPort());
    
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
    
    // Start live input receivers (both camera and drone for instant switching)
    if (camera_tcp_receiver_) {
        if (!camera_tcp_receiver_->start()) {
            std::cerr << "[Multiplexer] Failed to start camera TCP receiver" << std::endl;
            return false;
        }
    }
    if (drone_tcp_receiver_) {
        if (!drone_tcp_receiver_->start()) {
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
    
    // Configure timestamp monitoring with video/audio PIDs
    if (live_stream_ready_.load()) {
        // Use live stream PIDs (camera or drone, whichever is available)
        TCPReceiver* primary_receiver = (current_input_source_ == InputSource::CAMERA &&
                                         camera_tcp_receiver_ && camera_tcp_receiver_->isConnected()) ?
                                        camera_tcp_receiver_.get() :
                                        (drone_tcp_receiver_ && drone_tcp_receiver_->isConnected() ?
                                         drone_tcp_receiver_.get() : nullptr);
        
        if (primary_receiver) {
            StreamInfo primary_info = primary_receiver->getStreamInfo();
            rtmp_output_->setVideoPID(primary_info.video_pid);
            rtmp_output_->setAudioPID(primary_info.audio_pid);
            std::cout << "[Multiplexer] Configured timestamp monitoring: Video PID="
                      << primary_info.video_pid << ", Audio PID=" << primary_info.audio_pid << std::endl;
        }
    } else {
        // Use fallback stream PIDs
        StreamInfo fallback_info = fallback_tcp_receiver_->getStreamInfo();
        rtmp_output_->setVideoPID(fallback_info.video_pid);
        rtmp_output_->setAudioPID(fallback_info.audio_pid);
        std::cout << "[Multiplexer] Configured timestamp monitoring (fallback): Video PID="
                  << fallback_info.video_pid << ", Audio PID=" << fallback_info.audio_pid << std::endl;
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
    if (drone_tcp_receiver_) drone_tcp_receiver_->stop();
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
    if (drone_tcp_receiver_) {
        std::cout << "  Drone packets received: " << drone_tcp_receiver_->getPacketsReceived() << std::endl;
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
    
    // Wait for both camera and drone streams (non-blocking, they may connect later)
    std::cout << "[Multiplexer] Checking for camera and drone TCP streams..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    
    StreamInfo camera_info;
    StreamInfo drone_info;
    bool camera_ready = false;
    bool drone_ready = false;
    
    // Check camera stream
    if (camera_tcp_receiver_->isConnected()) {
        camera_tcp_receiver_->waitForStreamInfo();
        camera_tcp_receiver_->waitForIDR();
        camera_info = camera_tcp_receiver_->getStreamInfo();
        
        if (camera_info.initialized) {
            std::cout << "[Multiplexer] Camera stream detected" << std::endl;
            std::cout << "  Video PID: " << camera_info.video_pid << std::endl;
            std::cout << "  Audio PID: " << camera_info.audio_pid << std::endl;
            
            if (!camera_tcp_receiver_->extractTimestampBases()) {
                std::cerr << "[Multiplexer] Warning: Could not extract camera timestamp bases" << std::endl;
            }
            camera_ready = true;
        }
    }
    
    // Check drone stream
    if (drone_tcp_receiver_->isConnected()) {
        drone_tcp_receiver_->waitForStreamInfo();
        drone_tcp_receiver_->waitForIDR();
        drone_info = drone_tcp_receiver_->getStreamInfo();
        
        if (drone_info.initialized) {
            std::cout << "[Multiplexer] Drone stream detected" << std::endl;
            std::cout << "  Video PID: " << drone_info.video_pid << std::endl;
            std::cout << "  Audio PID: " << drone_info.audio_pid << std::endl;
            
            if (!drone_tcp_receiver_->extractTimestampBases()) {
                std::cerr << "[Multiplexer] Warning: Could not extract drone timestamp bases" << std::endl;
            }
            drone_ready = true;
        }
    }
    
    // Initialize PID mapper based on what's available
    if (camera_ready || drone_ready) {
        // Use whichever stream is available, prefer current input source
        StreamInfo primary_info = input_source_manager_->isCamera() ?
            (camera_ready ? camera_info : drone_info) :
            (drone_ready ? drone_info : camera_info);
        
        pid_mapper_->initialize(primary_info, fallback_info);
        live_stream_ready_ = true;
        std::cout << "[Multiplexer] Live stream(s) ready" << std::endl;
    } else {
        std::cout << "[Multiplexer] No live streams available yet - will detect dynamically" << std::endl;
        live_stream_ready_ = false;
    }
    
    return true;
}

void Multiplexer::requestInputSourceSwitch(InputSource new_source) {
    std::cout << "[Multiplexer] Input source switch requested: "
              << InputSourceManager::toString(new_source) << std::endl;
    pending_input_source_ = new_source;
    input_source_change_pending_ = true;
}

bool Multiplexer::switchInputSource(InputSource new_source) {
    if (new_source == current_input_source_) {
        std::cout << "[Multiplexer] Already using input source: "
                  << InputSourceManager::toString(new_source) << std::endl;
        return true;
    }
    
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] AUDIO-SAFE INPUT SWITCH: "
              << InputSourceManager::toString(current_input_source_) << " → "
              << InputSourceManager::toString(new_source) << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
    
    TCPReceiver* new_receiver = (new_source == InputSource::CAMERA) ?
                                camera_tcp_receiver_.get() :
                                drone_tcp_receiver_.get();
    
    if (!new_receiver->isConnected() || !new_receiver->isStreamReady()) {
        std::cerr << "[Multiplexer] Target input source not ready yet" << std::endl;
        return false;
    }
    
    // Reset for new loop - triggers new IDR AND audio sync detection
    new_receiver->resetForNewLoop();
    
    // Wait for fresh video IDR frame
    std::cout << "[Multiplexer] Waiting for fresh IDR frame on new input..." << std::endl;
    new_receiver->waitForIDR();
    std::cout << "[Multiplexer] Fresh IDR frame detected" << std::endl;
    
    // AUDIO-SAFE: Wait for audio sync point (first audio PUSI after IDR)
    // This ensures we start from a clean audio PES boundary, preventing ADTS frame corruption
    std::cout << "[Multiplexer] Waiting for audio sync point..." << std::endl;
    new_receiver->waitForAudioSync();
    std::cout << "[Multiplexer] Audio sync point acquired" << std::endl;
    
    // Re-extract timestamp bases from current stream position
    if (!new_receiver->extractTimestampBases()) {
        std::cerr << "[Multiplexer] Warning: Could not extract timestamp bases" << std::endl;
    }
    
    // Update current bases and source for timestamp rebasing
    current_pts_base_ = new_receiver->getPTSBase();
    current_pcr_base_ = new_receiver->getPCRBase();
    current_pcr_pts_alignment_ = new_receiver->getPCRPTSAlignmentOffset();
    current_input_source_ = new_source;
    
    std::cout << "[Multiplexer] Updated bases: PTS=" << current_pts_base_
              << ", PCR=" << current_pcr_base_
              << ", alignment=" << current_pcr_pts_alignment_ << std::endl;
    
    // AUDIO-SAFE: Get buffered packets from audio sync point (not just IDR)
    // This ensures the first audio packet has a complete ADTS frame header
    auto buffered = new_receiver->getBufferedPacketsFromAudioSync();
    std::cout << "[Multiplexer] Processing " << buffered.size() << " packets from audio sync point" << std::endl;
    
    // Validate first audio packet has valid ADTS header
    if (!new_receiver->validateFirstAudioADTS(buffered)) {
        std::cerr << "[Multiplexer] Warning: First audio ADTS validation failed - may have audio glitch" << std::endl;
        // Continue anyway - the stream will likely recover
    } else {
        std::cout << "[Multiplexer] Audio ADTS validation passed" << std::endl;
    }
    
    // Inject SPS/PPS if available (before video IDR)
    std::vector<uint8_t> sps = new_receiver->getSPSData();
    std::vector<uint8_t> pps = new_receiver->getPPSData();
    if (!sps.empty() && !pps.empty()) {
        StreamInfo new_info = new_receiver->getStreamInfo();
        size_t injected = injectSPSPPS(Source::LIVE, new_info.video_pid,
                                       sps, pps, std::nullopt, std::nullopt);
        std::cout << "[Multiplexer] Injected " << injected << " SPS/PPS packets before IDR" << std::endl;
    } else {
        std::cerr << "[Multiplexer] WARNING: No SPS/PPS available for injection at input switch!" << std::endl;
    }
    
    // Process buffered packets from new source
    Source source = (new_source == InputSource::CAMERA) ? Source::LIVE : Source::LIVE;
    for (auto& pkt : buffered) {
        processPacket(pkt, source);
        packets_processed_++;
    }
    
    // Initialize consumption from end of snapshot
    new_receiver->initConsumptionFromIndex(new_receiver->getLastSnapshotEnd());
    
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] Audio-safe input switch complete!" << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
    return true;
}

void Multiplexer::processLoop() {
    std::cout << "[Multiplexer] Starting TCP-based processing loop" << std::endl;
    
    uint64_t log_interval = 1000;
    auto last_jitter_log = std::chrono::steady_clock::now();
    const int jitter_log_interval_sec = 5;  // Log jitter stats every 5 seconds
    
    // Initialize with fallback stream (already connected and ready from analyzeStreams)
    current_pts_base_ = fallback_tcp_receiver_->getPTSBase();
    current_pcr_base_ = fallback_tcp_receiver_->getPCRBase();
    current_pcr_pts_alignment_ = fallback_tcp_receiver_->getPCRPTSAlignmentOffset();
    
    std::cout << "[Multiplexer] Initial fallback bases: PTS=" << current_pts_base_
              << ", PCR=" << current_pcr_base_
              << ", alignment=" << current_pcr_pts_alignment_ << std::endl;
    
    // CRITICAL: Initialize TimestampManager with the PCR/PTS alignment offset
    // Per splice.md and multi2/tcp_main.cpp: PTS must start at the alignment offset (not 0)
    // This preserves the decoder's buffer timing - the gap between PCR and PTS
    // Without this, packets appear "corrupt" because DTS provides no buffer margin
    timestamp_mgr_->initializeWithAlignmentOffset(current_pcr_pts_alignment_);
    
    // CRITICAL: Get buffered packets starting from audio sync point (which includes IDR)
    // This avoids orphaned video continuation packets that cause "Packet corrupt" errors
    // (Packets between PAT/PMT and first IDR have PUSI=0 and no PES headers)
    auto initial_buffered = fallback_tcp_receiver_->getBufferedPacketsFromAudioSync();
    std::cout << "[Multiplexer] Initial buffer has " << initial_buffered.size()
              << " packets from audio sync point" << std::endl;
    
    // CRITICAL: Write PAT/PMT first, like multi2 does (line 1119-1129 of tcp_main.cpp)
    // Without PAT/PMT at the start, FFmpeg may not properly initialize stream decoders
    // which can cause "Packet corrupt" errors
    StreamInfo fallback_info = fallback_tcp_receiver_->getStreamInfo();
    
    std::cout << "[Multiplexer] Injecting initial PAT/PMT..." << std::endl;
    // Create and inject PAT
    ts::DuckContext duck;
    ts::PAT pat;
    pat.pmts[fallback_info.program_number > 0 ? fallback_info.program_number : 1] = fallback_info.pmt_pid;
    pat.setVersion(0);
    ts::BinaryTable patTable;
    pat.serialize(duck, patTable);
    ts::TSPacketVector patPackets;
    ts::OneShotPacketizer patPacketizer(duck, ts::PID_PAT);
    patPacketizer.addTable(patTable);
    patPacketizer.getPackets(patPackets);
    
    for (auto& patPkt : patPackets) {
        pid_mapper_->fixContinuityCounter(patPkt);
        rtmp_output_->writePacket(patPkt);
    }
    std::cout << "[Multiplexer] PAT injection: " << patPackets.size() << " packets" << std::endl;
    
    // Create and inject PMT
    ts::PMT pmt;
    pmt.service_id = fallback_info.program_number > 0 ? fallback_info.program_number : 1;
    pmt.pcr_pid = fallback_info.video_pid;  // PCR usually on video PID
    pmt.setVersion(0);
    pmt.streams[fallback_info.video_pid].stream_type = fallback_info.video_stream_type;
    if (fallback_info.audio_pid != ts::PID_NULL) {
        pmt.streams[fallback_info.audio_pid].stream_type = fallback_info.audio_stream_type;
    }
    ts::BinaryTable pmtTable;
    pmt.serialize(duck, pmtTable);
    ts::TSPacketVector pmtPackets;
    ts::OneShotPacketizer pmtPacketizer(duck, fallback_info.pmt_pid);
    pmtPacketizer.addTable(pmtTable);
    pmtPacketizer.getPackets(pmtPackets);
    
    for (auto& pmtPkt : pmtPackets) {
        pid_mapper_->fixContinuityCounter(pmtPkt);
        rtmp_output_->writePacket(pmtPkt);
    }
    std::cout << "[Multiplexer] PMT injection: " << pmtPackets.size() << " packets" << std::endl;
    
    // CRITICAL: Extract first video frame's timestamps BEFORE SPS/PPS injection
    // Per multi2: SPS/PPS must have proper PTS to avoid "Packet corrupt" errors
    std::optional<uint64_t> first_video_pts;
    std::optional<uint64_t> first_video_dts;
    // Note: fallback_info already declared above (line 524)
    
    for (const auto& pkt : initial_buffered) {
        if (pkt.getPID() == fallback_info.video_pid && pkt.getPUSI() && pkt.hasPayload()) {
            size_t header_size = pkt.getHeaderSize();
            const uint8_t* payload = pkt.b + header_size;
            size_t payload_size = ts::PKT_SIZE - header_size;
            
            // Check for PES start code and extract PTS/DTS
            if (payload_size >= 14 && payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
                uint8_t pts_dts_flags = (payload[7] >> 6) & 0x03;
                
                if (pts_dts_flags == 0x02 || pts_dts_flags == 0x03) {
                    // Extract PTS
                    uint64_t pts = ((uint64_t)(payload[9] & 0x0E) << 29) |
                                  ((uint64_t)(payload[10]) << 22) |
                                  ((uint64_t)(payload[11] & 0xFE) << 14) |
                                  ((uint64_t)(payload[12]) << 7) |
                                  ((uint64_t)(payload[13] >> 1));
                    first_video_pts = pts;
                    
                    if (pts_dts_flags == 0x03 && payload_size >= 19) {
                        // Extract DTS
                        uint64_t dts = ((uint64_t)(payload[14] & 0x0E) << 29) |
                                      ((uint64_t)(payload[15]) << 22) |
                                      ((uint64_t)(payload[16] & 0xFE) << 14) |
                                      ((uint64_t)(payload[17]) << 7) |
                                      ((uint64_t)(payload[18] >> 1));
                        first_video_dts = dts;
                    }
                    
                    std::cout << "[Multiplexer] First video frame timestamps: PTS="
                              << (first_video_pts.has_value() ? std::to_string(first_video_pts.value()) : "none")
                              << ", DTS="
                              << (first_video_dts.has_value() ? std::to_string(first_video_dts.value()) : "none")
                              << std::endl;
                    break;
                }
            }
        }
    }
    
    // CRITICAL: Inject SPS/PPS WITH proper PTS/DTS to avoid "Packet corrupt" errors
    // FFmpeg requires strictly increasing DTS values. Additionally, with B-frames present,
    // PTS can go backward (B-frames display before I/P frames but decode after).
    //
    // The issue: If SPS/PPS PTS is based on the IDR's PTS, subsequent B-frames may have
    // PTS < SPS/PPS PTS, which FFmpeg interprets as corruption.
    //
    // Solution: Use DTS (decode order) as the base for BOTH SPS/PPS PTS and DTS.
    // DTS is always monotonically increasing, so (first_video_DTS - one_frame) will be
    // earlier than ALL video timestamps (both PTS and DTS).
    std::vector<uint8_t> initial_sps = fallback_tcp_receiver_->getSPSData();
    std::vector<uint8_t> initial_pps = fallback_tcp_receiver_->getPPSData();
    if (!initial_sps.empty() && !initial_pps.empty()) {
        // At 29.97fps, one frame duration is 3003 ticks (90kHz / 29.97 ≈ 3003)
        const uint64_t ONE_FRAME_DURATION = 3003;
        
        std::optional<uint64_t> rebased_pts;
        std::optional<uint64_t> rebased_dts;
        
        // Use DTS as the base for BOTH PTS and DTS of SPS/PPS
        // This ensures SPS/PPS timestamps are earlier than ALL video frames (including B-frames)
        if (first_video_dts.has_value()) {
            uint64_t dts_rebased = (first_video_dts.value() - current_pts_base_ + timestamp_mgr_->getGlobalPTSOffset());
            
            // Subtract one frame duration so SPS/PPS is earlier than all video
            if (dts_rebased >= ONE_FRAME_DURATION) {
                dts_rebased -= ONE_FRAME_DURATION;
            }
            dts_rebased = dts_rebased & 0x1FFFFFFFFULL;
            
            // Use same value for both PTS and DTS (parameter sets don't have display timing issues)
            rebased_dts = dts_rebased;
            rebased_pts = dts_rebased;  // Use DTS-based value for PTS too
        }
        
        std::cout << "[Multiplexer] SPS/PPS injection with DTS-based timestamps: PTS="
                  << (rebased_pts.has_value() ? std::to_string(rebased_pts.value()) : "none")
                  << ", DTS="
                  << (rebased_dts.has_value() ? std::to_string(rebased_dts.value()) : "none")
                  << " (one frame before first video DTS, safe for B-frames)" << std::endl;
        
        size_t injected = injectSPSPPS(Source::FALLBACK, fallback_info.video_pid,
                                       initial_sps, initial_pps, rebased_pts, rebased_dts);
        std::cout << "[Multiplexer] Initial SPS/PPS injection complete - " << injected << " packets" << std::endl;
    } else {
        std::cerr << "[Multiplexer] WARNING: No SPS/PPS available for initial injection!" << std::endl;
    }
    
    // Process the initial buffered packets from audio sync point
    for (auto& pkt : initial_buffered) {
        processPacket(pkt, Source::FALLBACK);
        packets_processed_++;
    }
    std::cout << "[Multiplexer] Processed " << initial_buffered.size() << " initial packets" << std::endl;
    
    // Start consuming from end of snapshot (continue from where buffered packets ended)
    fallback_tcp_receiver_->initConsumptionFromIndex(fallback_tcp_receiver_->getLastSnapshotEnd());
    
    Mode current_mode = switcher_->getMode();
    TCPReceiver* active_receiver = fallback_tcp_receiver_.get();
    Source active_source = Source::FALLBACK;
    
    // Set initial input source from InputSourceManager
    current_input_source_ = input_source_manager_->getInputSource();
    
    // If we start in LIVE mode (privacy disabled and live ready), switch to appropriate live input
    if (current_mode == Mode::LIVE && live_stream_ready_.load()) {
        TCPReceiver* initial_receiver = (current_input_source_ == InputSource::CAMERA) ?
                                        camera_tcp_receiver_.get() :
                                        drone_tcp_receiver_.get();
        
        if (initial_receiver->isConnected() && initial_receiver->isStreamReady()) {
            std::cout << "[Multiplexer] Starting in LIVE mode with "
                      << InputSourceManager::toString(current_input_source_) << " input" << std::endl;
            
            // Get buffered packets from audio sync point (avoid orphaned packets)
            auto live_buffered = initial_receiver->getBufferedPacketsFromAudioSync();
            std::cout << "[Multiplexer] Initial LIVE buffer has " << live_buffered.size()
                      << " packets from audio sync point" << std::endl;
            
            // Update bases for LIVE stream
            current_pts_base_ = initial_receiver->getPTSBase();
            current_pcr_base_ = initial_receiver->getPCRBase();
            current_pcr_pts_alignment_ = initial_receiver->getPCRPTSAlignmentOffset();
            
            // Inject SPS/PPS for LIVE stream
            std::vector<uint8_t> live_sps = initial_receiver->getSPSData();
            std::vector<uint8_t> live_pps = initial_receiver->getPPSData();
            if (!live_sps.empty() && !live_pps.empty()) {
                StreamInfo live_info = initial_receiver->getStreamInfo();
                size_t injected = injectSPSPPS(Source::LIVE, live_info.video_pid,
                                               live_sps, live_pps, std::nullopt, std::nullopt);
                std::cout << "[Multiplexer] Initial LIVE SPS/PPS injection - " << injected << " packets" << std::endl;
            }
            
            // Process initial LIVE buffered packets
            for (auto& pkt : live_buffered) {
                processPacket(pkt, Source::LIVE);
                packets_processed_++;
            }
            std::cout << "[Multiplexer] Processed " << live_buffered.size() << " initial LIVE packets" << std::endl;
            
            // Set active receiver and start consumption
            active_receiver = initial_receiver;
            active_source = Source::LIVE;
            initial_receiver->initConsumptionFromIndex(initial_receiver->getLastSnapshotEnd());
        }
    }
    
    while (running_.load()) {
        // Check for pending input source switch request
        if (input_source_change_pending_.load()) {
            InputSource requested_source = pending_input_source_.load();
            if (requested_source != current_input_source_) {
                std::cout << "[Multiplexer] Processing pending input source switch to "
                          << InputSourceManager::toString(requested_source) << std::endl;
                
                // Attempt the switch
                if (switchInputSource(requested_source)) {
                    // Switch succeeded - update local variables
                    TCPReceiver* new_receiver = (requested_source == InputSource::CAMERA) ?
                                                camera_tcp_receiver_.get() :
                                                drone_tcp_receiver_.get();
                    
                    // Only update active receiver if we're in LIVE mode
                    if (current_mode == Mode::LIVE) {
                        active_receiver = new_receiver;
                        active_source = Source::LIVE;
                        std::cout << "[Multiplexer] Active receiver updated to "
                                  << InputSourceManager::toString(requested_source) << std::endl;
                    }
                    
                    // Clear the pending flag since switch succeeded
                    input_source_change_pending_ = false;
                    std::cout << "[Multiplexer] Input source switch completed successfully" << std::endl;
                } else {
                    // Switch failed (stream not ready) - keep pending flag set
                    // The switch will be retried when the stream becomes available
                    std::cout << "[Multiplexer] Input source switch deferred (stream not ready yet)" << std::endl;
                }
            } else {
                // Already using the requested source
                input_source_change_pending_ = false;
            }
        }
        
        // Check for pending privacy mode change request
        if (privacy_mode_change_pending_.load()) {
            bool privacy_enabled = pending_privacy_enabled_.load();
            std::cout << "[Multiplexer] Processing pending privacy mode change: "
                      << (privacy_enabled ? "ENABLE" : "DISABLE") << std::endl;
            
            if (privacy_enabled) {
                // Privacy mode enabled - force switch to FALLBACK immediately
                std::cout << "[Multiplexer] Privacy mode enabled - switching to FALLBACK" << std::endl;
                if (switchToFallbackAtIDR()) {
                    current_mode = Mode::FALLBACK;
                    active_receiver = fallback_tcp_receiver_.get();
                    active_source = Source::FALLBACK;
                    std::cout << "[Multiplexer] Switched to FALLBACK mode due to privacy" << std::endl;
                }
            } else {
                // Privacy mode disabled - check if we can return to LIVE
                std::cout << "[Multiplexer] Privacy mode disabled - attempting to return to LIVE" << std::endl;
                
                // Determine which live receiver to use based on input source
                TCPReceiver* live_receiver = (current_input_source_ == InputSource::CAMERA) ?
                                             camera_tcp_receiver_.get() :
                                             drone_tcp_receiver_.get();
                
                // Check if the live stream is available
                if (live_receiver && live_receiver->isConnected() && live_receiver->isStreamReady()) {
                    std::cout << "[Multiplexer] Live stream available - switching back to LIVE mode" << std::endl;
                    if (switchToLiveAtIDR()) {
                        current_mode = Mode::LIVE;
                        active_receiver = live_receiver;
                        active_source = Source::LIVE;
                        std::cout << "[Multiplexer] Returned to LIVE mode" << std::endl;
                    }
                } else {
                    std::cout << "[Multiplexer] Live stream not ready - staying in FALLBACK mode" << std::endl;
                }
            }
            
            // Clear the pending flag
            privacy_mode_change_pending_ = false;
        }
        
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
        
        // Periodic jitter statistics logging
        auto now = std::chrono::steady_clock::now();
        auto jitter_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_jitter_log).count();
        if (jitter_elapsed >= jitter_log_interval_sec) {
            std::cout << "\n[Jitter Stats] ========================================" << std::endl;
            
            // Camera stream stats
            if (camera_tcp_receiver_ && camera_tcp_receiver_->isConnected()) {
                JitterStats camera_stats = camera_tcp_receiver_->getJitterStats();
                std::cout << "[Jitter] Camera:" << std::endl;
                std::cout << "  Packet Arrival: avg=" << std::fixed << std::setprecision(2)
                          << camera_stats.avg_packet_interval_ms << "ms, jitter="
                          << camera_stats.packet_jitter_ms << "ms, range=["
                          << camera_stats.min_packet_interval_ms << "-"
                          << camera_stats.max_packet_interval_ms << "ms]" << std::endl;
                std::cout << "  PTS: avg_delta=" << camera_stats.avg_pts_delta_ms
                          << "ms, jitter=" << camera_stats.pts_jitter_ms
                          << "ms, count=" << camera_stats.total_pts_packets << std::endl;
                std::cout << "  PCR: avg_delta=" << camera_stats.avg_pcr_delta_ms
                          << "ms, jitter=" << camera_stats.pcr_jitter_ms
                          << "ms, count=" << camera_stats.total_pcr_packets << std::endl;
                std::cout << "  Buffer: current=" << camera_stats.current_buffer_size
                          << "/" << camera_stats.max_buffer_size
                          << ", avg=" << camera_stats.avg_buffer_fill
                          << ", range=[" << camera_stats.min_buffer_fill
                          << "-" << camera_stats.max_buffer_fill << "]" << std::endl;
                std::cout << "  IDR: avg_interval=" << camera_stats.avg_idr_interval_ms
                          << "ms, jitter=" << camera_stats.idr_interval_jitter_ms
                          << "ms, count=" << camera_stats.total_idr_frames << std::endl;
                std::cout << "  Total: " << camera_stats.total_packets
                          << " packets, uptime=" << (int)camera_stats.uptime_seconds << "s" << std::endl;
            } else {
                std::cout << "[Jitter] Camera: Not connected" << std::endl;
            }
            
            // Drone stream stats
            if (drone_tcp_receiver_ && drone_tcp_receiver_->isConnected()) {
                JitterStats drone_stats = drone_tcp_receiver_->getJitterStats();
                std::cout << "[Jitter] Drone:" << std::endl;
                std::cout << "  Packet Arrival: avg=" << std::fixed << std::setprecision(2)
                          << drone_stats.avg_packet_interval_ms << "ms, jitter="
                          << drone_stats.packet_jitter_ms << "ms, range=["
                          << drone_stats.min_packet_interval_ms << "-"
                          << drone_stats.max_packet_interval_ms << "ms]" << std::endl;
                std::cout << "  PTS: avg_delta=" << drone_stats.avg_pts_delta_ms
                          << "ms, jitter=" << drone_stats.pts_jitter_ms
                          << "ms, count=" << drone_stats.total_pts_packets << std::endl;
                std::cout << "  PCR: avg_delta=" << drone_stats.avg_pcr_delta_ms
                          << "ms, jitter=" << drone_stats.pcr_jitter_ms
                          << "ms, count=" << drone_stats.total_pcr_packets << std::endl;
                std::cout << "  Buffer: current=" << drone_stats.current_buffer_size
                          << "/" << drone_stats.max_buffer_size
                          << ", avg=" << drone_stats.avg_buffer_fill
                          << ", range=[" << drone_stats.min_buffer_fill
                          << "-" << drone_stats.max_buffer_fill << "]" << std::endl;
                std::cout << "  IDR: avg_interval=" << drone_stats.avg_idr_interval_ms
                          << "ms, jitter=" << drone_stats.idr_interval_jitter_ms
                          << "ms, count=" << drone_stats.total_idr_frames << std::endl;
                std::cout << "  Total: " << drone_stats.total_packets
                          << " packets, uptime=" << (int)drone_stats.uptime_seconds << "s" << std::endl;
            } else {
                std::cout << "[Jitter] Drone: Not connected" << std::endl;
            }
            
            // Fallback stream stats
            if (fallback_tcp_receiver_ && fallback_tcp_receiver_->isConnected()) {
                JitterStats fallback_stats = fallback_tcp_receiver_->getJitterStats();
                std::cout << "[Jitter] Fallback:" << std::endl;
                std::cout << "  Packet Arrival: avg=" << std::fixed << std::setprecision(2)
                          << fallback_stats.avg_packet_interval_ms << "ms, jitter="
                          << fallback_stats.packet_jitter_ms << "ms, range=["
                          << fallback_stats.min_packet_interval_ms << "-"
                          << fallback_stats.max_packet_interval_ms << "ms]" << std::endl;
                std::cout << "  PTS: avg_delta=" << fallback_stats.avg_pts_delta_ms
                          << "ms, jitter=" << fallback_stats.pts_jitter_ms
                          << "ms, count=" << fallback_stats.total_pts_packets << std::endl;
                std::cout << "  PCR: avg_delta=" << fallback_stats.avg_pcr_delta_ms
                          << "ms, jitter=" << fallback_stats.pcr_jitter_ms
                          << "ms, count=" << fallback_stats.total_pcr_packets << std::endl;
                std::cout << "  Buffer: current=" << fallback_stats.current_buffer_size
                          << "/" << fallback_stats.max_buffer_size
                          << ", avg=" << fallback_stats.avg_buffer_fill
                          << ", range=[" << fallback_stats.min_buffer_fill
                          << "-" << fallback_stats.max_buffer_fill << "]" << std::endl;
                std::cout << "  IDR: avg_interval=" << fallback_stats.avg_idr_interval_ms
                          << "ms, jitter=" << fallback_stats.idr_interval_jitter_ms
                          << "ms, count=" << fallback_stats.total_idr_frames << std::endl;
                std::cout << "  Total: " << fallback_stats.total_packets
                          << " packets, uptime=" << (int)fallback_stats.uptime_seconds << "s" << std::endl;
            }
            
            std::cout << "[Jitter Stats] ========================================\n" << std::endl;
            last_jitter_log = now;
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
                
                // Update timestamp monitoring PIDs to use camera stream
                if (rtmp_output_) {
                    rtmp_output_->setVideoPID(camera_info.video_pid);
                    rtmp_output_->setAudioPID(camera_info.audio_pid);
                    std::cout << "[Multiplexer] Updated timestamp monitoring for camera: Video PID="
                              << camera_info.video_pid << ", Audio PID=" << camera_info.audio_pid << std::endl;
                }
                
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
        // Update StreamSwitcher's privacy mode state
        switcher_->setPrivacyMode(enabled);
        
        // Set pending flag for main loop to handle the actual switch
        // This ensures local variables in processLoop are updated correctly
        pending_privacy_enabled_ = enabled;
        privacy_mode_change_pending_ = true;
        
        std::cout << "[Multiplexer] Privacy mode change pending - will be processed in main loop" << std::endl;
    }
}

bool Multiplexer::switchToLiveAtIDR() {
    // Use the current input source to determine which receiver to use
    TCPReceiver* live_receiver = (current_input_source_ == InputSource::CAMERA) ?
                                 camera_tcp_receiver_.get() :
                                 drone_tcp_receiver_.get();
    
    if (!live_receiver) {
        std::cerr << "[Multiplexer] No live receiver available for switch" << std::endl;
        return false;
    }
    
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] AUDIO-SAFE SWITCH TO LIVE: "
              << InputSourceManager::toString(current_input_source_) << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
    
    // Reset receiver for new loop - triggers new IDR AND audio sync detection
    live_receiver->resetForNewLoop();
    
    // Wait for fresh video IDR
    std::cout << "[Multiplexer] Waiting for fresh IDR frame..." << std::endl;
    live_receiver->waitForIDR();
    std::cout << "[Multiplexer] Fresh IDR frame detected" << std::endl;
    
    // AUDIO-SAFE: Wait for audio sync point
    std::cout << "[Multiplexer] Waiting for audio sync point..." << std::endl;
    live_receiver->waitForAudioSync();
    std::cout << "[Multiplexer] Audio sync point acquired" << std::endl;
    
    // Re-extract timestamp bases from current stream position
    if (!live_receiver->extractTimestampBases()) {
        std::cerr << "[Multiplexer] Warning: Could not extract timestamp bases" << std::endl;
    }
    
    // Update current bases for timestamp rebasing
    current_pts_base_ = live_receiver->getPTSBase();
    current_pcr_base_ = live_receiver->getPCRBase();
    current_pcr_pts_alignment_ = live_receiver->getPCRPTSAlignmentOffset();
    
    std::cout << "[Multiplexer] Updated bases: PTS=" << current_pts_base_
              << ", PCR=" << current_pcr_base_
              << ", alignment=" << current_pcr_pts_alignment_ << std::endl;
    
    // AUDIO-SAFE: Get buffered packets from audio sync point
    auto buffered = live_receiver->getBufferedPacketsFromAudioSync();
    std::cout << "[Multiplexer] Processing " << buffered.size() << " packets from audio sync point" << std::endl;
    
    // Validate first audio packet
    if (!live_receiver->validateFirstAudioADTS(buffered)) {
        std::cerr << "[Multiplexer] Warning: First audio ADTS validation failed" << std::endl;
    } else {
        std::cout << "[Multiplexer] Audio ADTS validation passed" << std::endl;
    }
    
    // Inject SPS/PPS if available
    std::vector<uint8_t> sps = live_receiver->getSPSData();
    std::vector<uint8_t> pps = live_receiver->getPPSData();
    if (!sps.empty() && !pps.empty()) {
        StreamInfo live_info = live_receiver->getStreamInfo();
        size_t injected = injectSPSPPS(Source::LIVE, live_info.video_pid,
                                       sps, pps, std::nullopt, std::nullopt);
        std::cout << "[Multiplexer] Injected " << injected << " SPS/PPS packets before IDR" << std::endl;
    } else {
        std::cerr << "[Multiplexer] WARNING: No SPS/PPS available for injection at LIVE switch!" << std::endl;
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
    live_receiver->initConsumptionFromIndex(live_receiver->getLastSnapshotEnd());
    
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] Audio-safe switch to LIVE complete" << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
    return true;
}

bool Multiplexer::switchToFallbackAtIDR() {
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] AUDIO-SAFE SWITCH TO FALLBACK" << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
    
    // Reset fallback receiver for new loop - triggers new IDR AND audio sync detection
    fallback_tcp_receiver_->resetForNewLoop();
    
    // Wait for fresh video IDR
    std::cout << "[Multiplexer] Waiting for fresh fallback IDR frame..." << std::endl;
    fallback_tcp_receiver_->waitForIDR();
    std::cout << "[Multiplexer] Fresh fallback IDR frame detected" << std::endl;
    
    // AUDIO-SAFE: Wait for audio sync point
    std::cout << "[Multiplexer] Waiting for fallback audio sync point..." << std::endl;
    fallback_tcp_receiver_->waitForAudioSync();
    std::cout << "[Multiplexer] Fallback audio sync point acquired" << std::endl;
    
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
    
    // AUDIO-SAFE: Get buffered packets from audio sync point
    auto buffered = fallback_tcp_receiver_->getBufferedPacketsFromAudioSync();
    std::cout << "[Multiplexer] Processing " << buffered.size() << " packets from fallback audio sync point" << std::endl;
    
    // Validate first audio packet
    if (!fallback_tcp_receiver_->validateFirstAudioADTS(buffered)) {
        std::cerr << "[Multiplexer] Warning: Fallback first audio ADTS validation failed" << std::endl;
    } else {
        std::cout << "[Multiplexer] Fallback audio ADTS validation passed" << std::endl;
    }
    
    // Inject PAT/PMT before processing buffered packets
    // TODO: Need to create PAT/PMT from StreamInfo
    
    // Inject SPS/PPS if available
    std::vector<uint8_t> sps = fallback_tcp_receiver_->getSPSData();
    std::vector<uint8_t> pps = fallback_tcp_receiver_->getPPSData();
    if (!sps.empty() && !pps.empty()) {
        StreamInfo fallback_info = fallback_tcp_receiver_->getStreamInfo();
        size_t injected = injectSPSPPS(Source::FALLBACK, fallback_info.video_pid,
                                       sps, pps, std::nullopt, std::nullopt);
        std::cout << "[Multiplexer] Injected " << injected << " SPS/PPS packets before fallback IDR" << std::endl;
    } else {
        std::cerr << "[Multiplexer] WARNING: No SPS/PPS available for injection at FALLBACK switch!" << std::endl;
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
    
    std::cout << "[Multiplexer] ========================================" << std::endl;
    std::cout << "[Multiplexer] Audio-safe switch to FALLBACK complete" << std::endl;
    std::cout << "[Multiplexer] ========================================" << std::endl;
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
    
    // Use the overload with SPS/PPS data
    return injectSPSPPS(source, video_pid, sps, pps, pts, dts);
}

size_t Multiplexer::injectSPSPPS(Source source, uint16_t video_pid,
                                  const std::vector<uint8_t>& sps,
                                  const std::vector<uint8_t>& pps,
                                  std::optional<uint64_t> pts, std::optional<uint64_t> dts) {
    if (!sps_pps_injector_) {
        std::cerr << "[Multiplexer] SPS/PPS injector not initialized!" << std::endl;
        return 0;
    }
    
    if (sps.empty() || pps.empty()) {
        std::cerr << "[Multiplexer] SPS or PPS is empty - cannot inject" << std::endl;
        return 0;
    }
    
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