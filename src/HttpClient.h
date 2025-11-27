#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>

/**
 * Simple HTTP client using libcurl for making REST API calls to the controller.
 * Supports async operations to avoid blocking the main multiplexer loop.
 */
class HttpClient {
public:
    explicit HttpClient(const std::string& controller_url);
    ~HttpClient();
    
    // Prevent copying
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    
    // Scene notification methods (async, fire-and-forget)
    void notifySceneLive();
    void notifySceneFallback();
    
    // Privacy mode query (blocking - used at startup)
    bool queryPrivacyMode();
    
    // Set the controller URL
    void setControllerUrl(const std::string& url);
    
private:
    // Generic HTTP POST (async)
    void postAsync(const std::string& path, const std::string& body = "");
    
    // Generic HTTP POST (blocking)
    bool postSync(const std::string& path, const std::string& body, std::string& response);
    
    // Generic HTTP GET (blocking)
    bool getSync(const std::string& path, std::string& response);
    
    std::string controller_url_;
    std::mutex url_mutex_;
};