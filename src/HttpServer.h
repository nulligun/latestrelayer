#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>

/**
 * Health status structure returned by health callback
 */
struct HealthStatus {
    bool rtmp_connected = false;
    uint64_t packets_written = 0;
    int64_t ms_since_last_write = -1;  // -1 means no write yet
    bool stream_incompatible = false;  // True if camera stream codec is incompatible
};

/**
 * Input source status structure returned by input source callback
 */
struct InputSourceStatus {
    std::string current_source;  // "camera" or "drone"
};

/**
 * Simple HTTP server for receiving callbacks from the controller.
 * Listens on a specified port and handles POST /privacy and POST /input endpoints.
 */
class HttpServer {
public:
    using PrivacyCallback = std::function<void(bool enabled)>;
    using HealthCallback = std::function<HealthStatus()>;
    using InputSourceCallback = std::function<void(const std::string& source)>;
    using InputSourceGetCallback = std::function<InputSourceStatus()>;
    
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
    
    // Register callback for input source changes (POST /input)
    void setInputSourceCallback(InputSourceCallback callback);
    
    // Register callback to get current input source (GET /input)
    void setInputSourceGetCallback(InputSourceGetCallback callback);
    
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
    InputSourceCallback input_source_callback_;
    InputSourceGetCallback input_source_get_callback_;
};