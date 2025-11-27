#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>

/**
 * Simple HTTP server for receiving callbacks from the controller.
 * Listens on a specified port and handles POST /privacy endpoint.
 */
class HttpServer {
public:
    using PrivacyCallback = std::function<void(bool enabled)>;
    
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
};