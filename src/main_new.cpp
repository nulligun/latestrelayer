/*
 * Streamlined Multiplexer - Camera + Fallback Splicing via TCP
 * 
 * Based on multi2/src/tcp_main.cpp proven TCP splicing pattern.
 * 
 * Architecture:
 * - TCPReader for camera input (port 10000)
 * - TCPReader for fallback input (port 10001)
 * - StreamSplicer for timestamp rebasing and splice logic
 * - TCPOutput connects to ffmpeg-rtmp-output (port 10004)
 * - FFmpeg publishes to nginx-rtmp
 * 
 * Switching logic:
 * - Start with fallback stream
 * - Auto-switch to camera when available
 * - Auto-switch back to fallback if camera drops
 * - All switches happen at IDR frames with audio sync
 */

#include "TCPReader.h"
#include "TCPOutput.h"
#include "StreamSplicer.h"
#include "HttpServer.h"
#include <iostream>
#include <csignal>
#include <atomic>

// Global running flag for signal handling
std::atomic<bool> g_running(true);

// Global privacy mode flag
std::atomic<bool> g_privacy_mode_enabled(false);

// Global current scene state ("fallback" or "live")
std::atomic<std::string*> g_current_scene_ptr(nullptr);
std::mutex g_scene_mutex;

// Global controller URL for notifications
std::string g_controller_url;

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
    const std::string CAMERA_HOST = "ffmpeg-srt-input";
    const uint16_t CAMERA_PORT = 10000;
    const std::string FALLBACK_HOST = "ffmpeg-fallback";
    const uint16_t FALLBACK_PORT = 10001;
    const std::string OUTPUT_HOST = "ffmpeg-rtmp-output";
    const uint16_t OUTPUT_PORT = 10004;
    
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
    }
    
    // Create components
    std::cout << "[Main] Creating TCP readers..." << std::endl;
    TCPReader camera_reader("Camera", CAMERA_HOST, CAMERA_PORT);
    TCPReader fallback_reader("Fallback", FALLBACK_HOST, FALLBACK_PORT);
    
    std::cout << "[Main] Creating TCP output..." << std::endl;
    TCPOutput tcp_output(OUTPUT_HOST, OUTPUT_PORT, g_running);
    
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
    
    if (!http_server.start()) {
        std::cerr << "[Main] Failed to start HTTP server" << std::endl;
        return 1;
    }
    
    // Start TCP readers
    std::cout << "[Main] Starting TCP readers..." << std::endl;
    if (!camera_reader.start()) {
        std::cerr << "[Main] Failed to start camera reader" << std::endl;
        return 1;
    }
    if (!fallback_reader.start()) {
        std::cerr << "[Main] Failed to start fallback reader" << std::endl;
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
    
    // Connect to output
    std::cout << "[Main] Connecting to ffmpeg-rtmp-output..." << std::endl;
    if (!tcp_output.connect()) {
        std::cerr << "[Main] Failed to connect to output" << std::endl;
        return 1;
    }
    
    // Write initial PAT/PMT
    std::cout << "[Main] Writing initial PAT/PMT..." << std::endl;
    ts::TSPacket pat = splicer.createPAT(fallback_info.program_number > 0 ? fallback_info.program_number : 1,
                                         ts::PID(4096));
    splicer.fixContinuityCounter(pat);
    tcp_output.writePacket(pat);
    
    ts::TSPacket pmt = splicer.createPMT(fallback_info.program_number > 0 ? fallback_info.program_number : 1,
                                         fallback_info.video_pid,
                                         fallback_info.video_pid,
                                         fallback_info.audio_pid,
                                         fallback_info.video_stream_type,
                                         fallback_info.audio_stream_type);
    splicer.fixContinuityCounter(pmt);
    tcp_output.writePacket(pmt);
    
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
        std::cout << "[Main] Injecting " << sps_pps_packets.size() << " SPS/PPS packets" << std::endl;
        for (auto& pkt : sps_pps_packets) {
            splicer.fixContinuityCounter(pkt);
            tcp_output.writePacket(pkt);
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
        tcp_output.writePacket(pkt);
        
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
                                  ((uint64_t)(payload[11] & 0xFE) << 14) |
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
    enum class Mode { FALLBACK, CAMERA };
    Mode current_mode = Mode::FALLBACK;
    TCPReader* active_reader = &fallback_reader;
    
    std::cout << "[Main] Entering main processing loop..." << std::endl;
    
    uint64_t packets_processed = 0;
    auto last_log = std::chrono::steady_clock::now();
    
    while (g_running.load()) {
        // Check for mode switch
        if (current_mode == Mode::FALLBACK) {
            // Check if camera became ready (only switch if privacy is NOT enabled)
            if (!g_privacy_mode_enabled.load() &&
                camera_reader.isConnected() && camera_reader.isStreamReady()) {
                std::cout << "[Main] ========================================" << std::endl;
                std::cout << "[Main] Camera became available - switching!" << std::endl;
                std::cout << "[Main] ========================================" << std::endl;
                
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
                        tcp_output.writePacket(pkt);
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
                    tcp_output.writePacket(pkt);
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
                std::cout << "[Main] Switched to CAMERA mode" << std::endl;
                
                // Update global scene and notify controller
                {
                    std::lock_guard<std::mutex> lock(g_scene_mutex);
                    std::string* old_scene = g_current_scene_ptr.load();
                    g_current_scene_ptr.store(new std::string("live"));
                    delete old_scene;
                }
                http_server.notifySceneChange("live", g_controller_url);
            }
        } else if (current_mode == Mode::CAMERA) {
            // Check if privacy mode enabled (force switch to fallback) OR camera disconnected
            if (g_privacy_mode_enabled.load() ||
                !camera_reader.isConnected() || !camera_reader.isStreamReady()) {
                
                if (g_privacy_mode_enabled.load()) {
                    std::cout << "[Main] ========================================" << std::endl;
                    std::cout << "[Main] Privacy mode enabled - switching to fallback!" << std::endl;
                    std::cout << "[Main] ========================================" << std::endl;
                } else {
                    std::cout << "[Main] ========================================" << std::endl;
                    std::cout << "[Main] Camera lost - switching to fallback!" << std::endl;
                    std::cout << "[Main] ========================================" << std::endl;
                }
                
                fallback_reader.resetForNewLoop();
                fallback_reader.waitForIDR();
                fallback_reader.waitForAudioSync();
                
                if (!fallback_reader.extractTimestampBases()) {
                    std::cerr << "[Main] Failed to extract fallback timestamp bases" << std::endl;
                    continue;
                }
                
                // Get buffered packets from fallback
                auto fb_packets = fallback_reader.getBufferedPacketsFromAudioSync();
                std::cout << "[Main] Processing " << fb_packets.size() << " fallback packets from audio sync" << std::endl;
                
                // Inject SPS/PPS
                std::vector<uint8_t> fb_sps = fallback_reader.getSPSData();
                std::vector<uint8_t> fb_pps = fallback_reader.getPPSData();
                
                if (!fb_sps.empty() && !fb_pps.empty()) {
                    auto sps_pps_pkt = splicer.createSPSPPSPackets(fb_sps, fb_pps, fallback_info.video_pid,
                                                                    splicer.getGlobalPTSOffset());
                    std::cout << "[Main] Injecting " << sps_pps_pkt.size() << " fallback SPS/PPS packets" << std::endl;
                    for (auto& pkt : sps_pps_pkt) {
                        splicer.fixContinuityCounter(pkt);
                        tcp_output.writePacket(pkt);
                    }
                }
                
                // Update bases for fallback stream
                pts_base = fallback_reader.getPTSBase();
                pcr_base = fallback_reader.getPCRBase();
                pcr_pts_alignment = fallback_reader.getPCRPTSAlignmentOffset();
                
                // Process fallback packets
                uint64_t seg_max_pts = 0;
                uint64_t seg_max_pcr = 0;
                for (auto& pkt : fb_packets) {
                    splicer.rebasePacket(pkt, pts_base, pcr_base, pcr_pts_alignment);
                    splicer.fixContinuityCounter(pkt);
                    tcp_output.writePacket(pkt);
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
                fallback_reader.initConsumptionFromIndex(fallback_reader.getLastSnapshotEnd());
                
                current_mode = Mode::FALLBACK;
                active_reader = &fallback_reader;
                std::cout << "[Main] Switched to FALLBACK mode" << std::endl;
                
// Update global scene and notify controller
                {
                    std::lock_guard<std::mutex> lock(g_scene_mutex);
                    std::string* old_scene = g_current_scene_ptr.load();
                    g_current_scene_ptr.store(new std::string("fallback"));
                    delete old_scene;
                }
                http_server.notifySceneChange("fallback", g_controller_url);
            }
        }
        
        // Receive and process packets from active stream
        auto packets = active_reader->receivePackets(100, 100);
        
        if (!packets.empty()) {
            for (auto& pkt : packets) {
                splicer.rebasePacket(pkt, pts_base, pcr_base, pcr_pts_alignment);
                splicer.fixContinuityCounter(pkt);
                tcp_output.writePacket(pkt);
                packets_processed++;
            }
        }
        
        // Periodic logging
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log).count() >= 10) {
            std::cout << "[Main] Processed " << packets_processed << " packets, Mode: "
                      << (current_mode == Mode::CAMERA ? "CAMERA" : "FALLBACK") << std::endl;
            last_log = now;
        }
    }
    
    std::cout << "\n[Main] Shutting down..." << std::endl;
    
    // Cleanup
    http_server.stop();
    camera_reader.stop();
    fallback_reader.stop();
    tcp_output.disconnect();
    
    std::cout << "[Main] Final statistics:" << std::endl;
    std::cout << "  Packets processed: " << packets_processed << std::endl;
    std::cout << "  Camera packets received: " <<  camera_reader.getPacketsReceived() << std::endl;
    std::cout << "  Fallback packets received: " << fallback_reader.getPacketsReceived() << std::endl;
    std::cout << "  Output packets written: " << tcp_output.getPacketsWritten() << std::endl;
    std::cout << "[Main] Shutdown complete" << std::endl;
    
    return 0;
}