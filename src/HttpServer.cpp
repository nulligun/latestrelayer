#include "HttpServer.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>

HttpServer::HttpServer(uint16_t port)
    : port_(port),
      running_(false),
      server_fd_(-1) {
}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start() {
    if (running_.load()) {
        std::cerr << "[HttpServer] Already running" << std::endl;
        return false;
    }
    
    // Create socket
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::cerr << "[HttpServer] Failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "[HttpServer] Failed to set SO_REUSEADDR: " << strerror(errno) << std::endl;
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }
    
    // Set non-blocking
    int flags = fcntl(server_fd_, F_GETFL, 0);
    fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);
    
    // Bind socket
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);
    
    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[HttpServer] Failed to bind to port " << port_ << ": " << strerror(errno) << std::endl;
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }
    
    // Listen
    if (listen(server_fd_, 5) < 0) {
        std::cerr << "[HttpServer] Failed to listen: " << strerror(errno) << std::endl;
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }
    
    running_ = true;
    server_thread_ = std::thread(&HttpServer::serverLoop, this);
    
    std::cout << "[HttpServer] Started on port " << port_ << std::endl;
    return true;
}

void HttpServer::stop() {
    if (!running_.load()) {
        return;
    }
    
    running_ = false;
    
    // Close server socket to unblock accept
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
    
    // Wait for server thread
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    
    std::cout << "[HttpServer] Stopped" << std::endl;
}

void HttpServer::setPrivacyCallback(PrivacyCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    privacy_callback_ = std::move(callback);
}

void HttpServer::setHealthCallback(HealthCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    health_callback_ = std::move(callback);
}

void HttpServer::setInputSourceCallback(InputSourceCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    input_source_callback_ = std::move(callback);
}

void HttpServer::setInputSourceGetCallback(InputSourceGetCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    input_source_get_callback_ = std::move(callback);
}

void HttpServer::serverLoop() {
    while (running_.load()) {
        // Use poll to wait for connections with timeout
        struct pollfd pfd;
        pfd.fd = server_fd_;
        pfd.events = POLLIN;
        
        int ret = poll(&pfd, 1, 500); // 500ms timeout
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            if (running_.load()) {
                std::cerr << "[HttpServer] Poll error: " << strerror(errno) << std::endl;
            }
            break;
        }
        
        if (ret == 0) {
            // Timeout, check running flag and continue
            continue;
        }
        
        // Accept connection
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (running_.load()) {
                std::cerr << "[HttpServer] Accept error: " << strerror(errno) << std::endl;
            }
            continue;
        }
        
        // Read request with timeout
        char buffer[4096];
        memset(buffer, 0, sizeof(buffer));
        
        struct pollfd read_pfd;
        read_pfd.fd = client_fd;
        read_pfd.events = POLLIN;
        
        if (poll(&read_pfd, 1, 1000) > 0) {
            ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
            if (bytes_read > 0) {
                std::string request(buffer, bytes_read);
                std::string method, path, body;
                
                if (parseRequest(request, method, path, body)) {
                    std::string response = handleRequest(method, path, body);
                    write(client_fd, response.c_str(), response.length());
                } else {
                    std::string response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
                    write(client_fd, response.c_str(), response.length());
                }
            }
        }
        
        close(client_fd);
    }
}

bool HttpServer::parseRequest(const std::string& request, std::string& method, std::string& path, std::string& body) {
    // Parse first line
    std::istringstream stream(request);
    std::string line;
    
    if (!std::getline(stream, line)) {
        return false;
    }
    
    // Parse method and path
    std::istringstream first_line(line);
    std::string http_version;
    if (!(first_line >> method >> path >> http_version)) {
        return false;
    }
    
    // Find body (after empty line)
    size_t body_start = request.find("\r\n\r\n");
    if (body_start != std::string::npos) {
        body = request.substr(body_start + 4);
    }
    
    return true;
}

std::string HttpServer::handleRequest(const std::string& method, const std::string& path, const std::string& body) {
    std::cout << "[HttpServer] " << method << " " << path << std::endl;
    
    // Handle POST /privacy
    if (method == "POST" && path == "/privacy") {
        // Parse JSON body for "enabled" field
        bool enabled = false;
        if (body.find("\"enabled\": true") != std::string::npos ||
            body.find("\"enabled\":true") != std::string::npos) {
            enabled = true;
        }
        
        std::cout << "[HttpServer] Privacy mode callback: enabled=" << (enabled ? "true" : "false") << std::endl;
        
        // Call privacy callback
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (privacy_callback_) {
                privacy_callback_(enabled);
            }
        }
        
        std::string response_body = "{\"status\": \"ok\"}";
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "Content-Type: application/json\r\n"
                 << "Content-Length: " << response_body.length() << "\r\n"
                 << "\r\n"
                 << response_body;
        return response.str();
    }
    
    // Handle POST /input
    if (method == "POST" && path == "/input") {
        // Parse JSON body for "source" field
        std::string source;
        
        // Simple JSON parsing for "source": "camera" or "source": "drone"
        size_t source_pos = body.find("\"source\"");
        if (source_pos != std::string::npos) {
            size_t colon_pos = body.find(':', source_pos);
            if (colon_pos != std::string::npos) {
                size_t quote_start = body.find('"', colon_pos);
                if (quote_start != std::string::npos) {
                    size_t quote_end = body.find('"', quote_start + 1);
                    if (quote_end != std::string::npos) {
                        source = body.substr(quote_start + 1, quote_end - quote_start - 1);
                    }
                }
            }
        }
        
        // Validate source
        if (source != "camera" && source != "drone") {
            std::string response_body = "{\"error\": \"Invalid source. Must be 'camera' or 'drone'\"}";
            std::ostringstream response;
            response << "HTTP/1.1 400 Bad Request\r\n"
                     << "Content-Type: application/json\r\n"
                     << "Content-Length: " << response_body.length() << "\r\n"
                     << "\r\n"
                     << response_body;
            return response.str();
        }
        
        std::cout << "[HttpServer] Input source change request: source=" << source << std::endl;
        
        // Call input source callback
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (input_source_callback_) {
                input_source_callback_(source);
            }
        }
        
        std::string response_body = "{\"status\": \"ok\", \"source\": \"" + source + "\"}";
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "Content-Type: application/json\r\n"
                 << "Content-Length: " << response_body.length() << "\r\n"
                 << "\r\n"
                 << response_body;
        return response.str();
    }
    
    // Handle GET /input
    if (method == "GET" && path == "/input") {
        std::ostringstream response_body;
        
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (input_source_get_callback_) {
                InputSourceStatus status = input_source_get_callback_();
                response_body << "{\"source\": \"" << status.current_source << "\"}";
            } else {
                response_body << "{\"source\": \"camera\"}";  // Default
            }
        }
        
        std::string body_str = response_body.str();
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "Content-Type: application/json\r\n"
                 << "Content-Length: " << body_str.length() << "\r\n"
                 << "\r\n"
                 << body_str;
        return response.str();
    }
    
    // Handle GET /health
    if (method == "GET" && path == "/health") {
        std::ostringstream response_body;
        std::string http_status = "200 OK";
        
        // Query health status from callback
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (health_callback_) {
                HealthStatus status = health_callback_();
                
                // Determine overall health
                // Healthy if connected and wrote packets recently (within 5 seconds)
                bool is_healthy = status.rtmp_connected &&
                                  status.ms_since_last_write >= 0 &&
                                  status.ms_since_last_write < 5000;
                
                if (!is_healthy) {
                    http_status = "503 Service Unavailable";
                }
                
                response_body << "{"
                              << "\"status\": \"" << (is_healthy ? "healthy" : "unhealthy") << "\", "
                              << "\"rtmp\": {"
                              << "\"connected\": " << (status.rtmp_connected ? "true" : "false") << ", "
                              << "\"packets_written\": " << status.packets_written << ", "
                              << "\"ms_since_last_write\": " << status.ms_since_last_write
                              << "}"
                              << "}";
            } else {
                // No callback set - return basic status
                response_body << "{\"status\": \"ok\", \"rtmp\": null}";
            }
        }
        
        std::string body = response_body.str();
        std::ostringstream response;
        response << "HTTP/1.1 " << http_status << "\r\n"
                 << "Content-Type: application/json\r\n"
                 << "Content-Length: " << body.length() << "\r\n"
                 << "\r\n"
                 << body;
        return response.str();
    }
    
    // 404 for other paths
    std::string response_body = "{\"error\": \"Not found\"}";
    std::ostringstream response;
    response << "HTTP/1.1 404 Not Found\r\n"
             << "Content-Type: application/json\r\n"
             << "Content-Length: " << response_body.length() << "\r\n"
             << "\r\n"
             << response_body;
    return response.str();
}