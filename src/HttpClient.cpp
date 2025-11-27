#include "HttpClient.h"
#include <curl/curl.h>
#include <iostream>
#include <sstream>

// Callback function for libcurl to write received data
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    size_t total_size = size * nmemb;
    userp->append(static_cast<char*>(contents), total_size);
    return total_size;
}

HttpClient::HttpClient(const std::string& controller_url)
    : controller_url_(controller_url) {
    // Initialize libcurl globally (thread-safe when called once at startup)
    static bool curl_initialized = false;
    if (!curl_initialized) {
        curl_global_init(CURL_GLOBAL_ALL);
        curl_initialized = true;
    }
    
    std::cout << "[HttpClient] Initialized with controller URL: " << controller_url_ << std::endl;
}

HttpClient::~HttpClient() {
    // Note: We don't call curl_global_cleanup() as it affects the whole process
    // and other threads might still be using curl
}

void HttpClient::setControllerUrl(const std::string& url) {
    std::lock_guard<std::mutex> lock(url_mutex_);
    controller_url_ = url;
}

void HttpClient::notifySceneLive() {
    postAsync("/scene/live", "");
}

void HttpClient::notifySceneFallback() {
    postAsync("/scene/fallback", "");
}

bool HttpClient::queryPrivacyMode() {
    std::string response;
    if (getSync("/privacy", response)) {
        // Parse JSON response: {"privacy_enabled": true/false}
        // Simple parsing without JSON library
        if (response.find("\"privacy_enabled\": true") != std::string::npos ||
            response.find("\"privacy_enabled\":true") != std::string::npos) {
            std::cout << "[HttpClient] Privacy mode is ENABLED" << std::endl;
            return true;
        }
        std::cout << "[HttpClient] Privacy mode is DISABLED" << std::endl;
        return false;
    }
    
    std::cerr << "[HttpClient] Failed to query privacy mode, assuming disabled" << std::endl;
    return false;
}

void HttpClient::postAsync(const std::string& path, const std::string& body) {
    // Make a copy of the URL for the thread
    std::string url;
    {
        std::lock_guard<std::mutex> lock(url_mutex_);
        url = controller_url_;
    }
    
    // Fire and forget in a detached thread
    std::thread([url, path, body]() {
        CURL* curl = curl_easy_init();
        if (!curl) {
            std::cerr << "[HttpClient] Failed to initialize curl for async POST" << std::endl;
            return;
        }
        
        std::string full_url = url + path;
        std::string response;
        
        curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.length());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
        
        // Set headers
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        CURLcode res = curl_easy_perform(curl);
        
        if (res != CURLE_OK) {
            std::cerr << "[HttpClient] Async POST " << path << " failed: " 
                      << curl_easy_strerror(res) << std::endl;
        } else {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            std::cout << "[HttpClient] POST " << path << " completed (HTTP " << http_code << ")" << std::endl;
        }
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }).detach();
}

bool HttpClient::postSync(const std::string& path, const std::string& body, std::string& response) {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(url_mutex_);
        url = controller_url_;
    }
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[HttpClient] Failed to initialize curl for sync POST" << std::endl;
        return false;
    }
    
    std::string full_url = url + path;
    
    curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.length());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    
    CURLcode res = curl_easy_perform(curl);
    bool success = (res == CURLE_OK);
    
    if (!success) {
        std::cerr << "[HttpClient] Sync POST " << path << " failed: " 
                  << curl_easy_strerror(res) << std::endl;
    }
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    return success;
}

bool HttpClient::getSync(const std::string& path, std::string& response) {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(url_mutex_);
        url = controller_url_;
    }
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[HttpClient] Failed to initialize curl for GET" << std::endl;
        return false;
    }
    
    std::string full_url = url + path;
    
    curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    
    CURLcode res = curl_easy_perform(curl);
    bool success = (res == CURLE_OK);
    
    if (!success) {
        std::cerr << "[HttpClient] GET " << path << " failed: " 
                  << curl_easy_strerror(res) << std::endl;
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code >= 400) {
            std::cerr << "[HttpClient] GET " << path << " returned HTTP " << http_code << std::endl;
            success = false;
        }
    }
    
    curl_easy_cleanup(curl);
    
    return success;
}