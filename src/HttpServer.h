#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <memory>

// Forward declaration
class InputSourceManager;

/**
 * Health status structure returned by health callback
 */
struct HealthStatus {
    bool rtmp_connected = false;
    uint64_t packets_written = 0;
    int64_t ms_since_last_write = -1;  // -1 means no write yet
};

/**
 * Simple HTTP server for receiving callbacks from the controller.
 * Listens on a specified port and handles:
 * - POST /privacy - Privacy mode changes
 * - GET /health - Health status
 * - GET /input - Get current input source
 * - POST /input - Set input source
 */
class HttpServer {
public:
    using PrivacyCallback = std::function<void(bool enabled)>;
    using HealthCallback = std::function<HealthStatus()>;
    
    explicit HttpServer(uint16_t port);
    ~HttpServer();
    
    // Prevent copying
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    
    // Start the server in a background thread
    bool start();
    
    // Stop the server
    void stop();
    
    // Register callback for privacy mode changes
    void setPrivacyCallback(PrivacyCallback callback);
    
    // Register callback for health status
    void setHealthCallback(HealthCallback callback);
    
    // Set input source manager for /input endpoints
    void setInputSourceManager(std::shared_ptr<InputSourceManager> manager);
    
    // Check if server is running
    bool isRunning() const { return running_.load(); }
    
private:
    // Server main loop
    void serverLoop();
    
    // Parse HTTP request and extract path and body
    bool parseRequest(const std::string& request, std::string& method, std::string& path, std::string& body);
    
    // Handle incoming request
    std::string handleRequest(const std::string& method, const std::string& path, const std::string& body);
    
    uint16_t port_;
    std::atomic<bool> running_;
    std::thread server_thread_;
    int server_fd_;
    
    std::mutex callback_mutex_;
    PrivacyCallback privacy_callback_;
    HealthCallback health_callback_;
    std::shared_ptr<InputSourceManager> input_source_manager_;
};