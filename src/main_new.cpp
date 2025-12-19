/*
 * Streamlined Multiplexer - Camera + Fallback Splicing via Named Pipes
 *
 * Based on multi2/src/tcp_main.cpp proven TCP splicing pattern.
 *
 * Architecture:
 * - FIFOInput for camera input (/pipe/camera.ts)
 * - FIFOInput for fallback input (/pipe/fallback.ts)
 * - StreamSplicer for timestamp rebasing and splice logic
 * - FIFOOutput to ffmpeg-rtmp-output (/pipe/ts_output.pipe)
 * - FFmpeg publishes to srs
 *
 * Switching logic:
 * - Start with fallback stream
 * - Auto-switch to camera when available
 * - Auto-switch back to fallback if camera drops
 * - All switches happen at IDR frames with audio sync
 */

#include "FIFOInput.h"
#include "FIFOOutput.h"
#include "StreamSplicer.h"
#include "HttpServer.h"
#include "InputSourceManager.h"
#include <iostream>
#include <csignal>
#include <atomic>
#include <memory>
#include <chrono>
#include <vector>
#include <thread>
#include <sstream>
#include <map>
#include <algorithm>
#include <cinttypes>
#include <cstring>

// Global running flag for signal handling
std::atomic<bool> g_running(true);

// Global privacy mode flag
std::atomic<bool> g_privacy_mode_enabled(false);

// Global current scene state ("fallback", "live-camera", or "live-drone")
std::atomic<std::string*> g_current_scene_ptr(nullptr);
std::mutex g_scene_mutex;

// Global scene change timestamp (milliseconds since epoch)
std::atomic<int64_t> g_scene_change_time_ms(0);

// Global controller URL for notifications
std::string g_controller_url;

// Global requested input source (camera or drone) - can be changed via HTTP
enum class RequestedLiveSource { CAMERA, DRONE };
std::atomic<RequestedLiveSource> g_requested_live_source(RequestedLiveSource::CAMERA);

void signal_handler(int signum) {
    std::cout << "\n[Main] Received signal " << signum << ", shutting down..." << std::endl;
    g_running = false;
}

int main(int argc, char* argv[]) {
    std::cout << "=== Streamlined Multiplexer (Camera + Fallback) ===" << std::endl;
    std::cout << "Based on multi2 TCP splicing pattern" << std::endl;
    std::cout << std::endl;
    
    // Install signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Configuration
    const std::string CAMERA_PIPE = "/pipe/camera.ts";
    const std::string FALLBACK_PIPE = "/pipe/fallback.ts";
    const std::string DRONE_PIPE = "/pipe/drone.ts";
    const std::string OUTPUT_PIPE = "/pipe/ts_output.pipe";
    
    // Load health monitoring configuration from environment
    StreamHealthConfig health_config;
    if (const char* env = std::getenv("MAX_DATA_AGE_MS")) {
        health_config.max_data_age_ms = std::stoll(env);
    }
    if (const char* env = std::getenv("MIN_BITRATE_BPS")) {
        health_config.min_bitrate_bps = std::stoull(env);
    }
    if (const char* env = std::getenv("BITRATE_WINDOW_SECONDS")) {
        health_config.bitrate_window_seconds = std::stoi(env);
    }
    
    std::cout << "[Main] Stream health config:" << std::endl;
    std::cout << "  max_data_age_ms: " << health_config.max_data_age_ms << std::endl;
    std::cout << "  min_bitrate_bps: " << health_config.min_bitrate_bps << std::endl;
    std::cout << "  bitrate_window_seconds: " << health_config.bitrate_window_seconds << std::endl;
    
    // Get controller URL from environment variable
    const char* controller_url_env = std::getenv("CONTROLLER_URL");
    if (controller_url_env) {
        g_controller_url = controller_url_env;
        std::cout << "[Main] Controller URL: " << g_controller_url << std::endl;
    } else {
        g_controller_url = "http://controller:8089";
        std::cout << "[Main] Controller URL not set, using default: " << g_controller_url << std::endl;
    }
    
    // Initialize current scene
    {
        std::lock_guard<std::mutex> lock(g_scene_mutex);
        g_current_scene_ptr.store(new std::string("fallback"));
        // Initialize scene change timestamp
        auto now = std::chrono::system_clock::now();
        auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        g_scene_change_time_ms.store(ms_since_epoch);
    }
    
    // Create components
    std::cout << "[Main] Creating FIFO readers..." << std::endl;
    FIFOInput camera_reader("Camera", CAMERA_PIPE);
    FIFOInput fallback_reader("Fallback", FALLBACK_PIPE);
    FIFOInput drone_reader("Drone", DRONE_PIPE);
    
    std::cout << "[Main] Creating FIFO output..." << std::endl;
    FIFOOutput fifo_output(OUTPUT_PIPE, g_running);
    
    std::cout << "[Main] Creating stream splicer..." << std::endl;
    StreamSplicer splicer;
    
    // Create and start HTTP server for controller integration
    std::cout << "[Main] Creating HTTP server on port 8091..." << std::endl;
    HttpServer http_server(8091);
    
    // Register privacy mode callback
    http_server.setPrivacyCallback([](bool enabled) {
        std::cout << "[Main] Privacy mode " << (enabled ? "ENABLED" : "DISABLED")
                  << " - will " << (enabled ? "switch to" : "allow switching from")
                  << " fallback" << std::endl;
        g_privacy_mode_enabled.store(enabled);
    });
    
    // Register get current scene callback
    http_server.setGetCurrentSceneCallback([]() -> std::string {
        std::lock_guard<std::mutex> lock(g_scene_mutex);
        std::string* scene_ptr = g_current_scene_ptr.load();
        if (scene_ptr) {
            return *scene_ptr;
        }
        return "unknown";
    });
    
    // Register get scene timestamp callback
    http_server.setGetSceneTimestampCallback([]() -> int64_t {
        return g_scene_change_time_ms.load();
    });
    
    // CRITICAL: Set up InputSourceManager and callback for /input endpoint
    const std::string STATE_FILE_PATH = "/app/shared/input_state.json";
    auto input_manager = std::make_shared<InputSourceManager>(STATE_FILE_PATH);
    http_server.setInputSourceManager(input_manager);
    
    // Load persisted input source state and sync with g_requested_live_source
    input_manager->load();
    if (input_manager->getInputSource() == InputSource::DRONE) {
        g_requested_live_source.store(RequestedLiveSource::DRONE);
        std::cout << "[Main] Restored input source preference: DRONE" << std::endl;
    } else {
        g_requested_live_source.store(RequestedLiveSource::CAMERA);
        std::cout << "[Main] Restored input source preference: CAMERA" << std::endl;
    }
    
    // Register input source callback
    http_server.setInputSourceCallback([](InputSource source) {
        if (source == InputSource::CAMERA) {
            g_requested_live_source.store(RequestedLiveSource::CAMERA);
            std::cout << "[Main] User requested camera input" << std::endl;
        } else if (source == InputSource::DRONE) {
            g_requested_live_source.store(RequestedLiveSource::DRONE);
            std::cout << "[Main] User requested drone input" << std::endl;
        }
    });
    
    // Register input metrics callback
    http_server.setGetInputMetricsCallback([&camera_reader, &fallback_reader, &drone_reader]() -> HttpServer::AllInputMetrics {
        HttpServer::AllInputMetrics metrics;
        
        // Fallback metrics
        metrics.fallback.connected = fallback_reader.isConnected();
        metrics.fallback.data_age_ms = fallback_reader.getMsSinceLastData();
        metrics.fallback.bitrate_bps = fallback_reader.getCurrentBitrateBps();
        
        // Camera metrics
        metrics.camera.connected = camera_reader.isConnected();
        metrics.camera.data_age_ms = camera_reader.getMsSinceLastData();
        metrics.camera.bitrate_bps = camera_reader.getCurrentBitrateBps();
        
        // Drone metrics
        metrics.drone.connected = drone_reader.isConnected();
        metrics.drone.data_age_ms = drone_reader.getMsSinceLastData();
        metrics.drone.bitrate_bps = drone_reader.getCurrentBitrateBps();
        
        return metrics;
    });
    
    if (!http_server.start()) {
        std::cerr << "[Main] Failed to start HTTP server" << std::endl;
        return 1;
    }
    
    // Start FIFO readers
    std::cout << "[Main] Starting FIFO readers..." << std::endl;
    if (!camera_reader.start()) {
        std::cerr << "[Main] Failed to start camera reader" << std::endl;
        return 1;
    }
    if (!fallback_reader.start()) {
        std::cerr << "[Main] Failed to start fallback reader" << std::endl;
        return 1;
    }
    if (!drone_reader.start()) {
        std::cerr << "[Main] Failed to start drone reader" << std::endl;
        return 1;
    }
    
    // Wait for fallback stream (required)
    std::cout << "[Main] Waiting for fallback stream..." << std::endl;
    fallback_reader.waitForStreamInfo();
    fallback_reader.waitForIDR();
    fallback_reader.waitForAudioSync();
    
    StreamInfo fallback_info = fallback_reader.getStreamInfo();
    std::cout << "[Main] Fallback stream ready!" << std::endl;
    std::cout << "  Video PID: " << fallback_info.video_pid << std::endl;
    std::cout << "  Audio PID: " << fallback_info.audio_pid << std::endl;
    std::cout << "  PMT PID: " << fallback_info.pmt_pid << std::endl;
    
    // Extract fallback timestamp bases
    if (!fallback_reader.extractTimestampBases()) {
        std::cerr << "[Main] Failed to extract fallback timestamp bases" << std::endl;
        return 1;
    }
    
    // Initialize splicer with PCR/PTS alignment offset
    splicer.initializeWithAlignmentOffset(fallback_reader.getPCRPTSAlignmentOffset());
    
    // Open named pipe output (will block until ffmpeg opens it for reading)
    std::cout << "[Main] Opening named pipe for output..." << std::endl;
    if (!fifo_output.open()) {
        std::cerr << "[Main] Failed to open output pipe" << std::endl;
        return 1;
    }
    
    // Write initial PAT/PMT
    std::cout << "[Main] Writing initial PAT/PMT..." << std::endl;
    ts::TSPacket pat = splicer.createPAT(fallback_info.program_number > 0 ? fallback_info.program_number : 1,
                                         ts::PID(4096));
    splicer.fixContinuityCounter(pat);
    fifo_output.writePacket(pat);
    
    ts::TSPacket pmt = splicer.createPMT(fallback_info.program_number > 0 ? fallback_info.program_number : 1,
                                         fallback_info.video_pid,
                                         fallback_info.video_pid,
                                         fallback_info.audio_pid,
                                         fallback_info.video_stream_type,
                                         fallback_info.audio_stream_type);
    splicer.fixContinuityCounter(pmt);
    fifo_output.writePacket(pmt);
    
    // Get initial buffered packets from fallback
    auto initial_packets = fallback_reader.getBufferedPacketsFromAudioSync();
    std::cout << "[Main] Processing " << initial_packets.size() << " initial fallback packets" << std::endl;
    
    // Inject SPS/PPS before first IDR
    std::vector<uint8_t> sps = fallback_reader.getSPSData();
    std::vector<uint8_t> pps = fallback_reader.getPPSData();
    if (!sps.empty() && !pps.empty()) {
        // Use global PTS offset as PTS for SPS/PPS
        auto sps_pps_packets = splicer.createSPSPPSPackets(sps, pps, fallback_info.video_pid,
                                                           splicer.getGlobalPTSOffset());
        std::cout << "[Main] Injecting " << sps_pps_packets.size() << " camera SPS/PPS packets" << std::endl;
        for (auto& pkt : sps_pps_packets) {
            splicer.fixContinuityCounter(pkt);
            fifo_output.writePacket(pkt);
        }
    }
    
    // Process initial fallback packets
    uint64_t max_pts = 0;
    uint64_t max_pcr = 0;
    uint64_t pts_base = fallback_reader.getPTSBase();
    uint64_t pcr_base = fallback_reader.getPCRBase();
    int64_t pcr_pts_alignment = fallback_reader.getPCRPTSAlignmentOffset();
    
    for (auto& pkt : initial_packets) {
        splicer.rebasePacket(pkt, pts_base, pcr_base, pcr_pts_alignment);
        splicer.fixContinuityCounter(pkt);
        fifo_output.writePacket(pkt);
        
        // Track max timestamps
        if (pkt.hasPCR()) {
            max_pcr = std::max(max_pcr, pkt.getPCR());
        }
        if (pkt.getPUSI() && pkt.hasPayload()) {
            size_t header_size = pkt.getHeaderSize();
            const uint8_t* payload = pkt.b + header_size;
            size_t payload_size = ts::PKT_SIZE - header_size;
            if (payload_size >= 14 && payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
                uint8_t pts_dts_flags = (payload[7] >> 6) & 0x03;
                if (pts_dts_flags == 0x02 || pts_dts_flags == 0x03) {
                    uint64_t pts = ((uint64_t)(payload[9] & 0x0E) << 29) |
                                  ((uint64_t)(payload[10]) << 22) |
                                  ((uint8_t)(payload[11] & 0xFE) << 14) |
                                  ((uint64_t)(payload[12]) << 7) |
                                  ((uint64_t)(payload[13] >> 1));
                    max_pts = std::max(max_pts, pts);
                }
            }
        }
    }
    
    // Update splicer offsets
    splicer.updateOffsetsFromMaxTimestamps(max_pts, max_pcr);
    
    // Start consuming from end of snapshot
    fallback_reader.initConsumptionFromIndex(fallback_reader.getLastSnapshotEnd());
    
    // Main loop state
    enum class Mode { FALLBACK, CAMERA, DRONE };
    Mode current_mode = Mode::FALLBACK;
    FIFOInput* active_reader = &fallback_reader;
    
    std::cout << "[Main] Entering main processing loop..." << std::endl;
    
    uint64_t packets_processed = 0;
    auto last_log = std::chrono::steady_clock::now();
    auto last_drone_check_log = std::chrono::steady_clock::now();

    // CC verification tracking (like tcp_main.cpp)
    std::map<ts::PID, uint8_t> last_cc_values;
    size_t cc_discontinuities = 0;
    size_t cc_verifications = 0;
    
    while (g_running.load()) {
        // Check for mode switch
        if (current_mode == Mode::FALLBACK) {
            // Determine which live source to try based on user selection
            RequestedLiveSource requested = g_requested_live_source.load();
            
            if (!g_privacy_mode_enabled.load()) {
                if (requested == RequestedLiveSource::CAMERA) {
                    // Try camera first
                    if (camera_reader.isConnected() && camera_reader.isStreamReady() && camera_reader.isHealthy()) {
                        std::cout << "[Main] =======================================" << std::endl;
                        std::cout << "[Main] Camera became available - switching!" << std::endl;
                        std::cout << "[Main] =======================================" << std::endl;
                        
                        camera_reader.waitForStreamInfo();
                        camera_reader.waitForIDR();
                        camera_reader.waitForAudioSync();
                        
                        if (!camera_reader.extractTimestampBases()) {
                            std::cerr << "[Main] Failed to extract camera timestamp bases" << std::endl;
                            continue;
                        }
                        
                        // Get buffered packets from camera
                        auto camera_packets = camera_reader.getBufferedPacketsFromAudioSync();
                        std::cout << "[Main] Processing " << camera_packets.size() << " camera packets from audio sync" << std::endl;
                        
                        // Inject SPS/PPS
                        std::vector<uint8_t> cam_sps = camera_reader.getSPSData();
                        std::vector<uint8_t> cam_pps = camera_reader.getPPSData();
                        StreamInfo camera_info = camera_reader.getStreamInfo();
                        
                        if (!cam_sps.empty() && !cam_pps.empty()) {
                            auto sps_pps_pkt = splicer.createSPSPPSPackets(cam_sps, cam_pps, camera_info.video_pid,
                                                                            splicer.getGlobalPTSOffset());
                            std::cout << "[Main] Injecting " << sps_pps_pkt.size() << " camera SPS/PPS packets" << std::endl;
                            for (auto& pkt : sps_pps_pkt) {
                                splicer.fixContinuityCounter(pkt);
                                fifo_output.writePacket(pkt);
                            }
                        }
                        
                        // Update bases for camera stream
                        pts_base = camera_reader.getPTSBase();
                        pcr_base = camera_reader.getPCRBase();
                        pcr_pts_alignment = camera_reader.getPCRPTSAlignmentOffset();
                        
                        // Process camera packets
                        uint64_t seg_max_pts = 0;
                        uint64_t seg_max_pcr = 0;
                        for (auto& pkt : camera_packets) {
                            splicer.rebasePacket(pkt, pts_base, pcr_base, pcr_pts_alignment);
                            splicer.fixContinuityCounter(pkt);
                            fifo_output.writePacket(pkt);
                            packets_processed++;
                            
                            // Track max timestamps
                            if (pkt.hasPCR()) seg_max_pcr = std::max(seg_max_pcr, pkt.getPCR());
                            if (pkt.getPUSI() && pkt.hasPayload()) {
                                size_t header_size = pkt.getHeaderSize();
                                const uint8_t* payload = pkt.b + header_size;
                                size_t payload_size = ts::PKT_SIZE - header_size;
                                if (payload_size >= 14 && payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
                                    uint8_t pts_dts_flags = (payload[7] >> 6) & 0x03;
                                    if (pts_dts_flags == 0x02 || pts_dts_flags == 0x03) {
                                        uint64_t pts = ((uint64_t)(payload[9] & 0x0E) << 29) |
                                                      ((uint64_t)(payload[10]) << 22) |
                                                      ((uint64_t)(payload[11] & 0xFE) << 14) |
                                                      ((uint64_t)(payload[12]) << 7) |
                                                      ((uint64_t)(payload[13] >> 1));
                                        seg_max_pts = std::max(seg_max_pts, pts);
                                    }
                                }
                            }
                        }
                        
                        splicer.updateOffsetsFromMaxTimestamps(seg_max_pts, seg_max_pcr);
                        camera_reader.initConsumptionFromIndex(camera_reader.getLastSnapshotEnd());
                        
                        current_mode = Mode::CAMERA;
                        active_reader = &camera_reader;
                        
                        // Track timestamp update when switching from fallback to camera
                        {
                            std::lock_guard<std::mutex> lock(g_scene_mutex);
                            delete g_current_scene_ptr.load();
                            g_current_scene_ptr.store(new std::string("live-camera")); // cameras are live
                            
                            // Update scene change timestamp
                            auto now = std::chrono::system_clock::now();
                            auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                            g_scene_change_time_ms.store(ms_since_epoch);
                        }
                        
                        std::cout << "[Main] Switched to CAMERA mode" << std::endl;
                        std::cout << "[Main] Camera packets received: " << camera_reader.getPacketsReceived() << std::endl;
                        http_server.notifySceneChange("live-camera", g_controller_url);
                    }
                } else if (requested == RequestedLiveSource::DRONE) {
                    // Try drone
                    if (drone_reader.isConnected() && drone_reader.isStreamReady() && drone_reader.isHealthy()) {
                        std::cout << "[Main] =======================================" << std::endl;
                        std::cout << "[Main] Drone became available - switching!" << std::endl;
                        std::cout << "[Main] =======================================" << std::endl;
                        
                        drone_reader.resetForNewLoop();
                        drone_reader.waitForStreamInfo();
                        drone_reader.waitForIDR();
                        drone_reader.waitForAudioSync();
                        
                        if (!drone_reader.extractTimestampBases()) {
                            std::cerr << "[Main] Failed to extract drone timestamp bases" << std::endl;
                            continue;
                        }
                        
                        // Get buffered packets from drone
                        auto drone_packets = drone_reader.getBufferedPacketsFromAudioSync();
                        std::cout << "[Main] Processing " << drone_packets.size() << " drone packets from audio sync" << std::endl;
                        
                        // Inject SPS/PPS
                        std::vector<uint8_t> drone_sps = drone_reader.getSPSData();
                        std::vector<uint8_t> drone_pps = drone_reader.getPPSData();
                        StreamInfo drone_info = drone_reader.getStreamInfo();
                        
                        if (!drone_sps.empty() && !drone_pps.empty()) {
                            auto sps_pps_pkt = splicer.createSPSPPSPackets(drone_sps, drone_pps, drone_info.video_pid,
                                                                            splicer.getGlobalPTSOffset());
                            std::cout << "[Main] Injecting " << sps_pps_pkt.size() << " drone SPS/PPS packets" << std::endl;
                            for (auto& pkt : sps_pps_pkt) {
                                splicer.fixContinuityCounter(pkt);
                                fifo_output.writePacket(pkt);
                            }
                        }
                        
                        // Update bases for drone stream
                        pts_base = drone_reader.getPTSBase();
                        pcr_base = drone_reader.getPCRBase();
                        pcr_pts_alignment = drone_reader.getPCRPTSAlignmentOffset();
                        
                        // Process drone packets
                        uint64_t seg_max_pts = 0;
                        uint64_t seg_max_pcr = 0;
                        for (auto& pkt : drone_packets) {
                            splicer.rebasePacket(pkt, pts_base, pcr_base, pcr_pts_alignment);
                            splicer.fixContinuityCounter(pkt);
                            fifo_output.writePacket(pkt);
                            packets_processed++;
                            
                            // Track max timestamps
                            if (pkt.hasPCR()) seg_max_pcr = std::max(seg_max_pcr, pkt.getPCR());
                            if (pkt.getPUSI() && pkt.hasPayload()) {
                                size_t header_size = pkt.getHeaderSize();
                                const uint8_t* payload = pkt.b + header_size;
                                size_t payload_size = ts::PKT_SIZE - header_size;
                                if (payload_size >= 14 && payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
                                    uint8_t pts_dts_flags = (payload[7] >> 6) & 0x03;
                                    if (pts_dts_flags == 0x02 || pts_dts_flags == 0x03) {
                                        uint64_t pts = ((uint64_t)(payload[9] & 0x0E) << 29) |
                                                      ((uint64_t)(payload[10]) << 22) |
                                                      ((uint64_t)(payload[11] & 0xFE) << 14) |
                                                      ((uint64_t)(payload[12]) << 7) |
                                                      ((uint64_t)(payload[13] >> 1));
                                        seg_max_pts = std::max(seg_max_pts, pts);
                                    }
                                }
                            }
                        }
                        
                        splicer.updateOffsetsFromMaxTimestamps(seg_max_pts, seg_max_pcr);
                        drone_reader.initConsumptionFromIndex(drone_reader.getLastSnapshotEnd());
                        
                        current_mode = Mode::DRONE;
                        active_reader = &drone_reader;
                        
                        // Track timestamp update when switching from fallback to drone
                        {
                            std::lock_guard<std::mutex> lock(g_scene_mutex);
                            delete g_current_scene_ptr.load();
                            g_current_scene_ptr.store(new std::string("live-drone"));
                            
                            // Update scene change timestamp
                            auto now = std::chrono::system_clock::now();
                            auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                            g_scene_change_time_ms.store(ms_since_epoch);
                        }
                        
                        std::cout << "[Main] Switched to DRONE mode" << std::endl;
                        std::cout << "[Main] Drone packets received: " << drone_reader.getPacketsReceived() << std::endl;
                        http_server.notifySceneChange("live-drone", g_controller_url);
                    }
                }
            }
        }
        else if (current_mode == Mode::CAMERA) {
            // Check if privacy mode enabled (force switch to fallback) OR camera unhealthy
            bool camera_unhealthy = !camera_reader.isConnected() || !camera_reader.isStreamReady() || !camera_reader.isHealthy();
            
            if (g_privacy_mode_enabled.load() || camera_unhealthy) {
                if (g_privacy_mode_enabled.load()) {
                    std::cout << "[Main] =======================================" << std::endl;
                    std::cout << "[Main] Camera became unavailable (privacy mode) - switching back to fallback!" << std::endl;
                    std::cout << "[Main] =======================================" << std::endl;
                } else if (!camera_reader.isConnected()) {
                    std::cout << "[Main] =======================================" << std::endl;
                    std::cout << "[Main] Camera lost (disconnected) - switching back to fallback!" << std::endl;
                    std::cout << "[Main] =======================================" << std::endl;
                } else if (!camera_reader.isDataFresh()) {
                    std::cout << "[Main] =======================================" << std::endl;
                    std::cout << "[Main] Camera data stale (" << camera_reader.getMsSinceLastData()
                              << "ms since last data) - switching back to fallback!" << std::endl;
                    std::cout << "[Main] =======================================" << std::endl;
                } else if (health_config.min_bitrate_bps > 0 && camera_reader.getCurrentBitrateBps() < health_config.min_bitrate_bps) {
                    std::cout << "[Main] =======================================" << std::endl;
                    std::cout << "[Main] Camera bitrate too low (" << camera_reader.getCurrentBitrateBps()
                              << " bps < " << health_config.min_bitrate_bps << " bps) - switching back to fallback!" << std::endl;
                    std::cout << "[Main] =======================================" << std::endl;
                } else {
                    std::cout << "[Main] =======================================" << std::endl;
                    std::cout << "[Main] Camera unhealthy - switching back to fallback!" << std::endl;
                    std::cout << "[Main] =======================================" << std::endl;
                }
                
                // Track timestamp update when switching from camera to fallback
                {
                    std::lock_guard<std::mutex> lock(g_scene_mutex);
                    delete g_current_scene_ptr.load();
                    g_current_scene_ptr.store(new std::string("fallback"));
                    
                    // Update scene change timestamp
                    auto now = std::chrono::system_clock::now();
                    auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                    g_scene_change_time_ms.store(ms_since_epoch);
                    
                    std::cout << "[Main] Switched to FALLBACK mode" << std::endl;
                    std::cout << "[Main] Camera packets received: " << camera_reader.getPacketsReceived() << std::endl;
                    http_server.notifySceneChange("fallback", g_controller_url);
                }
                
                // Reset for new loop
                fallback_reader.resetForNewLoop();
                fallback_reader.waitForStreamInfo();
                fallback_reader.waitForIDR();
                fallback_reader.waitForAudioSync();
                
                if (!fallback_reader.extractTimestampBases()) {
                    std::cerr << "[Main] Failed to extract fallback timestamp bases" << std::endl;
                    continue;
                }
                
                std::cout << "[Main] Switched to FALLBACK mode" << std::endl;
                std::cout << "[Main] Camera packets received: " << camera_reader.getPacketsReceived() << std::endl;
                http_server.notifySceneChange("fallback", g_controller_url);
                
                current_mode = Mode::FALLBACK;
                active_reader = &fallback_reader;
            }
            // NEW: Check if user wants DRONE and drone is available (direct switch from CAMERA to DRONE)
            else if (g_requested_live_source.load() == RequestedLiveSource::DRONE &&
                     drone_reader.isConnected() && 
                     drone_reader.isStreamReady() && 
                     drone_reader.isHealthy()) {
                std::cout << "[Main] =======================================" << std::endl;
                std::cout << "[Main] User requested DRONE - switching from CAMERA!" << std::endl;
                std::cout << "[Main] =======================================" << std::endl;
                
                drone_reader.resetForNewLoop();
                drone_reader.waitForStreamInfo();
                drone_reader.waitForIDR();
                drone_reader.waitForAudioSync();
                
                if (!drone_reader.extractTimestampBases()) {
                    std::cerr << "[Main] Failed to extract drone timestamp bases" << std::endl;
                    continue;
                }
                
                // Get buffered packets from drone
                auto drone_packets = drone_reader.getBufferedPacketsFromAudioSync();
                std::cout << "[Main] Processing " << drone_packets.size() << " drone packets from audio sync" << std::endl;
                
                // Inject SPS/PPS
                std::vector<uint8_t> drone_sps = drone_reader.getSPSData();
                std::vector<uint8_t> drone_pps = drone_reader.getPPSData();
                StreamInfo drone_info = drone_reader.getStreamInfo();
                
                if (!drone_sps.empty() && !drone_pps.empty()) {
                    auto sps_pps_pkt = splicer.createSPSPPSPackets(drone_sps, drone_pps, drone_info.video_pid,
                                                                    splicer.getGlobalPTSOffset());
                    std::cout << "[Main] Injecting " << sps_pps_pkt.size() << " drone SPS/PPS packets" << std::endl;
                    for (auto& pkt : sps_pps_pkt) {
                        splicer.fixContinuityCounter(pkt);
                        fifo_output.writePacket(pkt);
                    }
                }
                
                // Update bases for drone stream
                pts_base = drone_reader.getPTSBase();
                pcr_base = drone_reader.getPCRBase();
                pcr_pts_alignment = drone_reader.getPCRPTSAlignmentOffset();
                
                // Process drone packets
                uint64_t seg_max_pts = 0;
                uint64_t seg_max_pcr = 0;
                for (auto& pkt : drone_packets) {
                    splicer.rebasePacket(pkt, pts_base, pcr_base, pcr_pts_alignment);
                    splicer.fixContinuityCounter(pkt);
                    fifo_output.writePacket(pkt);
                    packets_processed++;
                    
                    // Track max timestamps
                    if (pkt.hasPCR()) seg_max_pcr = std::max(seg_max_pcr, pkt.getPCR());
                    if (pkt.getPUSI() && pkt.hasPayload()) {
                        size_t header_size = pkt.getHeaderSize();
                        const uint8_t* payload = pkt.b + header_size;
                        size_t payload_size = ts::PKT_SIZE - header_size;
                        if (payload_size >= 14 && payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
                            uint8_t pts_dts_flags = (payload[7] >> 6) & 0x03;
                            if (pts_dts_flags == 0x02 || pts_dts_flags == 0x03) {
                                uint64_t pts = ((uint64_t)(payload[9] & 0x0E) << 29) |
                                              ((uint64_t)(payload[10]) << 22) |
                                              ((uint64_t)(payload[11] & 0xFE) << 14) |
                                              ((uint64_t)(payload[12]) << 7) |
                                              ((uint64_t)(payload[13] >> 1));
                                seg_max_pts = std::max(seg_max_pts, pts);
                            }
                        }
                    }
                }
                
                splicer.updateOffsetsFromMaxTimestamps(seg_max_pts, seg_max_pcr);
                drone_reader.initConsumptionFromIndex(drone_reader.getLastSnapshotEnd());
                
                current_mode = Mode::DRONE;
                active_reader = &drone_reader;
                std::cout << "[Main] Switched to DRONE mode" << std::endl;
                std::cout << "[Main] Drone packets received: " << drone_reader.getPacketsReceived() << std::endl;
                
                // Track timestamp update when switching from camera to drone
                {
                    std::lock_guard<std::mutex> lock(g_scene_mutex);
                    delete g_current_scene_ptr.load();
                    g_current_scene_ptr.store(new std::string("live-drone"));
                    
                    // Update scene change timestamp
                    auto now = std::chrono::system_clock::now();
                    auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                    g_scene_change_time_ms.store(ms_since_epoch);
                    
                    std::cout << "[Main] Switched to LIVE-DRONE mode" << std::endl;
                    std::cout << "[Main] Drone packets received: " << drone_reader.getPacketsReceived() << std::endl;
                    http_server.notifySceneChange("live-drone", g_controller_url);
                }
            }
        } else if (current_mode == Mode::DRONE) {
            // Check if privacy mode enabled (force switch to fallback) OR drone unhealthy
            bool drone_unhealthy = !drone_reader.isConnected() || !drone_reader.isStreamReady() || !drone_reader.isHealthy();
            
            if (g_privacy_mode_enabled.load() || drone_unhealthy) {
                
                if (g_privacy_mode_enabled.load()) {
                    std::cout << "[Main] =======================================" << std::endl;
                    std::cout << "[Main] Privacy mode enabled - switching to fallback!" << std::endl;
                    std::cout << "[Main] =======================================" << std::endl;
                } else if (!drone_reader.isConnected()) {
                    std::cout << "[Main] =======================================" << std::endl;
                    std::cout << "[Main] Drone lost (disconnected) - switching to fallback!" << std::endl;
                    std::cout << "[Main] =======================================" << std::endl;
                } else if (!drone_reader.isDataFresh()) {
                    std::cout << "[Main] =======================================" << std::endl;
                    std::cout << "[Main] Drone data stale (" << drone_reader.getMsSinceLastData()
                              << "ms since last data) - switching to fallback!" << std::endl;
                    std::cout << "[Main] =======================================" << std::endl;
                } else if (health_config.min_bitrate_bps > 0 && drone_reader.getCurrentBitrateBps() < health_config.min_bitrate_bps) {
                    std::cout << "[Main] =======================================" << std::endl;
                    std::cout << "[Main] Drone bitrate too low (" << drone_reader.getCurrentBitrateBps()
                              << " bps < " << health_config.min_bitrate_bps << " bps) - switching to fallback!" << std::endl;
                    std::cout << "[Main] =======================================" << std::endl;
                } else {
                    std::cout << "[Main] =======================================" << std::endl;
                    std::cout << "[Main] Drone unhealthy - switching to fallback!" << std::endl;
                    std::cout << "[Main] =======================================" << std::endl;
                }
                
                // Track timestamp update when switching from camera to fallback
                {
                    std::lock_guard<std::mutex> lock(g_scene_mutex);
                    delete g_current_scene_ptr.load();
                    g_current_scene_ptr.store(new std::string("fallback"));
                    
                    // Update scene change timestamp
                    auto now = std::chrono::system_clock::now();
                    auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                    g_scene_change_time_ms.store(ms_since_epoch);
                    
                    std::cout << "[Main] Switched to FALLBACK mode" << std::endl;
                    std::cout << "[Main] Camera packets received: " << camera_reader.getPacketsReceived() << std::endl;
                    http_server.notifySceneChange("fallback", g_controller_url);
                }
                
                // Reset for new loop
                fallback_reader.resetForNewLoop();
                fallback_reader.waitForStreamInfo();
                fallback_reader.waitForIDR();
                fallback_reader.waitForAudioSync();
                
                if (!fallback_reader.extractTimestampBases()) {
                    std::cerr << "[Main] Failed to extract fallback timestamp bases" << std::endl;
                    continue;
                }
                
                std::cout << "[Main] Switched to FALLBACK mode" << std::endl;
                std::cout << "[Main] Camera packets received: " << camera_reader.getPacketsReceived() << std::endl;
                http_server.notifySceneChange("fallback", g_controller_url);

                current_mode = Mode::FALLBACK;
                active_reader = &fallback_reader;
            }
        }
        
        // Read and process packets from active reader
        auto packets = active_reader->receivePackets(100, 10);
        for (auto& pkt : packets) {
            splicer.fixContinuityCounter(pkt);
            fifo_output.writePacket(pkt);
            packets_processed++;
        }
        
        // Periodic logging
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log).count() >= 5) {
            std::cout << "[Main] Packets processed: " << packets_processed << std::endl;
            
            // Log health metrics for all inputs
            std::cout << "[Main] Input Health Metrics:" << std::endl;
            std::cout << "  Fallback: connected=" << fallback_reader.isConnected() 
                      << ", bitrate=" << (fallback_reader.getCurrentBitrateBps() / 1024) << " Kbps"
                      << ", data_age=" << fallback_reader.getMsSinceLastData() << " ms" << std::endl;
            std::cout << "  Camera: connected=" << camera_reader.isConnected() 
                      << ", bitrate=" << (camera_reader.getCurrentBitrateBps() / 1024) << " Kbps"
                      << ", data_age=" << camera_reader.getMsSinceLastData() << " ms" << std::endl;
            std::cout << "  Drone: connected=" << drone_reader.isConnected() 
                      << ", bitrate=" << (drone_reader.getCurrentBitrateBps() / 1024) << " Kbps"
                      << ", data_age=" << drone_reader.getMsSinceLastData() << " ms" << std::endl;
            
            last_log = now;
        }
    }
    
    std::cout << "[Main] Shutting down..." << std::endl;
    return 0;
}