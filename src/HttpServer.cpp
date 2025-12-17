#include "HttpServer.h"
#include "InputSourceManager.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <algorithm>
#include <thread>
#include <sys/types.h>
#include <netdb.h>
#include <chrono>
#include <time.h>

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

void HttpServer::setInputSourceManager(std::shared_ptr<InputSourceManager> manager) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    input_source_manager_ = manager;
}

void HttpServer::setInputSourceCallback(InputSourceCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    input_source_callback_ = std::move(callback);
}

void HttpServer::setGetCurrentSceneCallback(GetCurrentSceneCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    get_current_scene_callback_ = std::move(callback);
    std::cout << "[HttpServer][startup-debug] setGetCurrentSceneCallback called - callback is " << (get_current_scene_callback_ ? "SET" : "NULL") << std::endl;
}

void HttpServer::setGetSceneTimestampCallback(GetSceneTimestampCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    get_scene_timestamp_callback_ = std::move(callback);
    std::cout << "[HttpServer] setGetSceneTimestampCallback called - callback is " << (get_scene_timestamp_callback_ ? "SET" : "NULL") << std::endl;
}

void HttpServer::notifySceneChange(const std::string& scene, const std::string& controllerUrl) {
    // Send HTTP POST in a background thread to avoid blocking
    // Capture scene timestamp callback by reference
    std::thread([this, scene, controllerUrl]() {
        try {
            std::cout << "[HttpServer] Notifying controller of scene change: " << scene << std::endl;
            
            // Get scene timestamp
            int64_t timestamp_ms = 0;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                if (get_scene_timestamp_callback_) {
                    timestamp_ms = get_scene_timestamp_callback_();
                }
            }
            
            // Convert timestamp to ISO8601 format
            std::string iso_timestamp;
            if (timestamp_ms > 0) {
                auto tp = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestamp_ms));
                std::time_t time = std::chrono::system_clock::to_time_t(tp);
                std::tm tm;
                gmtime_r(&time, &tm);
                char buffer[32];
                std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm);
                int millis = timestamp_ms % 1000;
                std::ostringstream oss;
                oss << buffer << "." << std::setfill('0') << std::setw(3) << millis << "Z";
                iso_timestamp = oss.str();
            } else {
                // Fallback to current time if timestamp not available
                auto now = std::chrono::system_clock::now();
                std::time_t time = std::chrono::system_clock::to_time_t(now);
                std::tm tm;
                gmtime_r(&time, &tm);
                char buffer[32];
                std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
                iso_timestamp = buffer;
            }
            
            // Parse controller URL to get host and port
            std::string host;
            int port = 8089;  // Default port
            
            // Simple URL parsing - expect format http://host:port or http://host
            size_t protocol_end = controllerUrl.find("://");
            std::string url_part = controllerUrl;
            if (protocol_end != std::string::npos) {
                url_part = controllerUrl.substr(protocol_end + 3);
            }
            
            size_t port_pos = url_part.find(':');
            if (port_pos != std::string::npos) {
                host = url_part.substr(0, port_pos);
                port = std::stoi(url_part.substr(port_pos + 1));
            } else {
                host = url_part;
            }
            
            std::cout << "[HttpServer] Connecting to " << host << ":" << port << std::endl;
            
            // Create socket
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) {
                std::cerr << "[HttpServer] Failed to create socket: " << strerror(errno) << std::endl;
                return;
            }
            
            // Set timeout
            struct timeval timeout;
            timeout.tv_sec = 5;
            timeout.tv_usec = 0;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            
            // Resolve hostname
            struct hostent *server = gethostbyname(host.c_str());
            if (server == nullptr) {
                std::cerr << "[HttpServer] Failed to resolve host: " << host << std::endl;
                close(sock);
                return;
            }
            
            // Connect to server
            struct sockaddr_in serv_addr;
            memset(&serv_addr, 0, sizeof(serv_addr));
            serv_addr.sin_family = AF_INET;
            memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
            serv_addr.sin_port = htons(port);
            
            if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
                std::cerr << "[HttpServer] Failed to connect to controller: " << strerror(errno) << std::endl;
                close(sock);
                return;
            }
            
            // Build JSON body
            std::ostringstream body_stream;
            body_stream << "{\"scene\": \"" << scene << "\", \"timestamp\": \"" << iso_timestamp << "\"}";
            std::string body = body_stream.str();
            
            // Build HTTP POST request with JSON body
            std::ostringstream request;
            request << "POST /scene HTTP/1.1\r\n"
                    << "Host: " << host << "\r\n"
                    << "Content-Type: application/json\r\n"
                    << "Content-Length: " << body.length() << "\r\n"
                    << "Connection: close\r\n"
                    << "\r\n"
                    << body;
            
            std::string request_str = request.str();
            
            // Send request
            ssize_t sent = send(sock, request_str.c_str(), request_str.length(), 0);
            if (sent < 0) {
                std::cerr << "[HttpServer] Failed to send request: " << strerror(errno) << std::endl;
                close(sock);
                return;
            }
            
            // Read response (just check if we get something back)
            char buffer[1024];
            ssize_t received = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (received > 0) {
                buffer[received] = '\0';
                std::cout << "[HttpServer] Controller response received" << std::endl;
            }
            
            close(sock);
            std::cout << "[HttpServer] Scene change notification sent successfully" << std::endl;
            
        } catch (const std::exception& e) {
            std::cerr << "[HttpServer] Error notifying controller: " << e.what() << std::endl;
        }
    }).detach();
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
        
        // Read request with timeout - may need multiple reads for body
        char buffer[4096];
        memset(buffer, 0, sizeof(buffer));
        std::string full_request;
        
        struct pollfd read_pfd;
        read_pfd.fd = client_fd;
        read_pfd.events = POLLIN;
        
        // Read initial chunk (headers + possibly body)
        if (poll(&read_pfd, 1, 1000) > 0) {
            ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
            if (bytes_read > 0) {
                full_request.append(buffer, bytes_read);
                
                // Check if we have the complete request by looking for \r\n\r\n
                size_t header_end = full_request.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    // Parse Content-Length to see if we need more data
                    size_t cl_pos = full_request.find("Content-Length:");
                    if (cl_pos != std::string::npos && cl_pos < header_end) {
                        size_t cl_start = cl_pos + 15; // strlen("Content-Length:")
                        size_t cl_end = full_request.find("\r\n", cl_start);
                        if (cl_end != std::string::npos) {
                            std::string cl_str = full_request.substr(cl_start, cl_end - cl_start);
                            // Trim whitespace
                            size_t first = cl_str.find_first_not_of(" \t");
                            if (first != std::string::npos) {
                                cl_str = cl_str.substr(first);
                            }
                            int content_length = std::atoi(cl_str.c_str());
                            
                            // Check if we have all the body data
                            size_t body_start = header_end + 4;
                            int body_received = full_request.length() - body_start;
                            
                            // Read more if needed
                            while (body_received < content_length && poll(&read_pfd, 1, 500) > 0) {
                                memset(buffer, 0, sizeof(buffer));
                                bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
                                if (bytes_read > 0) {
                                    full_request.append(buffer, bytes_read);
                                    body_received = full_request.length() - body_start;
                                } else {
                                    break;
                                }
                            }
                        }
                    }
                }
                
                std::string method, path, body;
                if (parseRequest(full_request, method, path, body)) {
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
        // Debug: Log the raw body for troubleshooting
        std::cout << "[HttpServer] POST /privacy - Body: " << body << std::endl;
        
        // Parse JSON body for "enabled" field
        // More robust parsing that handles various whitespace and case variations
        bool enabled = false;
        
        // Look for "enabled" key and check if value is true
        size_t enabled_pos = body.find("\"enabled\"");
        if (enabled_pos != std::string::npos) {
            // Find the colon after "enabled"
            size_t colon_pos = body.find(':', enabled_pos);
            if (colon_pos != std::string::npos) {
                // Look for 'true' after the colon (case-insensitive, handles whitespace)
                std::string after_colon = body.substr(colon_pos + 1);
                
                // Remove leading whitespace
                size_t value_start = after_colon.find_first_not_of(" \t\r\n");
                if (value_start != std::string::npos) {
                    // Check if it starts with 'true' or 'True' (handles Python True or JSON true)
                    if (after_colon.substr(value_start, 4) == "true" ||
                        after_colon.substr(value_start, 4) == "True") {
                        enabled = true;
                    }
                }
            }
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
    
    // Handle GET /input - get current input source
    if (method == "GET" && path == "/input") {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        
        if (!input_source_manager_) {
            std::string response_body = "{\"error\": \"Input source manager not initialized\"}";
            std::ostringstream response;
            response << "HTTP/1.1 503 Service Unavailable\r\n"
                     << "Content-Type: application/json\r\n"
                     << "Content-Length: " << response_body.length() << "\r\n"
                     << "\r\n"
                     << response_body;
            return response.str();
        }
        
        std::string source = input_source_manager_->getInputSourceString();
        std::cout << "[HttpServer] GET /input: source=" << source << std::endl;
        
        std::ostringstream response_body;
        response_body << "{\"source\": \"" << source << "\"}";
        
        std::string body_str = response_body.str();
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "Content-Type: application/json\r\n"
                 << "Content-Length: " << body_str.length() << "\r\n"
                 << "\r\n"
                 << body_str;
        return response.str();
    }
    
    // Handle POST /input - set input source
    if (method == "POST" && path == "/input") {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        
        if (!input_source_manager_) {
            std::string response_body = "{\"error\": \"Input source manager not initialized\"}";
            std::ostringstream response;
            response << "HTTP/1.1 503 Service Unavailable\r\n"
                     << "Content-Type: application/json\r\n"
                     << "Content-Length: " << response_body.length() << "\r\n"
                     << "\r\n"
                     << response_body;
            return response.str();
        }
        
        // Parse JSON body for "source" field
        // Simple parsing - look for "source": "camera" or "source": "drone"
        std::string source_value;
        
        // Find "source" key
        size_t pos = body.find("\"source\"");
        if (pos != std::string::npos) {
            // Find the colon and then the value
            pos = body.find(':', pos);
            if (pos != std::string::npos) {
                // Find the opening quote of the value
                pos = body.find('"', pos);
                if (pos != std::string::npos) {
                    size_t start = pos + 1;
                    size_t end = body.find('"', start);
                    if (end != std::string::npos) {
                        source_value = body.substr(start, end - start);
                    }
                }
            }
        }
        
        if (source_value.empty()) {
            std::cout << "[HttpServer] POST /input: missing or invalid 'source' field" << std::endl;
            std::string response_body = "{\"error\": \"Missing or invalid 'source' field. Expected: {\\\"source\\\": \\\"camera\\\"} or {\\\"source\\\": \\\"drone\\\"}\"}";
            std::ostringstream response;
            response << "HTTP/1.1 400 Bad Request\r\n"
                     << "Content-Type: application/json\r\n"
                     << "Content-Length: " << response_body.length() << "\r\n"
                     << "\r\n"
                     << response_body;
            return response.str();
        }
        
        // Convert to lowercase
        std::transform(source_value.begin(), source_value.end(), source_value.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        
        std::cout << "[HttpServer] POST /input: setting source to " << source_value << std::endl;
        
        if (!input_source_manager_->setInputSourceFromString(source_value)) {
            std::string response_body = "{\"error\": \"Invalid source value. Must be 'camera' or 'drone'\"}";
            std::ostringstream response;
            response << "HTTP/1.1 400 Bad Request\r\n"
                     << "Content-Type: application/json\r\n"
                     << "Content-Length: " << response_body.length() << "\r\n"
                     << "\r\n"
                     << response_body;
            return response.str();
        }
        
        // Trigger real-time switch via callback (if registered)
        InputSource new_source = input_source_manager_->getInputSource();
        if (input_source_callback_) {
            std::cout << "[HttpServer] Triggering real-time input source switch to " << source_value << std::endl;
            input_source_callback_(new_source);
        }
        
        std::ostringstream response_body;
        response_body << "{\"status\": \"ok\", \"source\": \"" << source_value
                      << "\", \"message\": \"Input source switch initiated to " << source_value
                      << ". Switch will occur at next IDR frame.\"}";
        
        std::string body_str = response_body.str();
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "Content-Type: application/json\r\n"
                 << "Content-Length: " << body_str.length() << "\r\n"
                 << "\r\n"
                 << body_str;
        return response.str();
    }
    
    // Handle GET /scene
    if (method == "GET" && path == "/scene") {
        std::cout << "[HttpServer][startup-debug] GET /scene request received" << std::endl;
        std::ostringstream response_body;
        
        // Query current scene from callback
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (get_current_scene_callback_) {
                std::cout << "[HttpServer][startup-debug] Calling get_current_scene_callback_" << std::endl;
                std::string current_scene = get_current_scene_callback_();
                std::cout << "[HttpServer][startup-debug] get_current_scene_callback_ returned: " << current_scene << std::endl;
                
                // Get scene timestamp if available
                int64_t timestamp_ms = 0;
                if (get_scene_timestamp_callback_) {
                    timestamp_ms = get_scene_timestamp_callback_();
                }
                
                // Convert timestamp to ISO8601 format
                std::string iso_timestamp;
                if (timestamp_ms > 0) {
                    auto tp = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestamp_ms));
                    std::time_t time = std::chrono::system_clock::to_time_t(tp);
                    std::tm tm;
                    gmtime_r(&time, &tm);
                    char buffer[32];
                    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm);
                    int millis = timestamp_ms % 1000;
                    std::ostringstream oss;
                    oss << buffer << "." << std::setfill('0') << std::setw(3) << millis << "Z";
                    iso_timestamp = oss.str();
                } else {
                    // Fallback to current time if timestamp not available
                    auto now = std::chrono::system_clock::now();
                    std::time_t time = std::chrono::system_clock::to_time_t(now);
                    std::tm tm;
                    gmtime_r(&time, &tm);
                    char buffer[32];
                    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
                    iso_timestamp = buffer;
                }
                
                response_body << "{\"scene\": \"" << current_scene << "\", \"scene_started_at\": \"" << iso_timestamp << "\"}";
            } else {
                // No callback set - return unknown  
                std::cout << "[HttpServer][startup-debug] WARNING: get_current_scene_callback_ is NULL, returning unknown" << std::endl;
                response_body << "{\"scene\": \"unknown\", \"scene_started_at\": null}";
            }
        }
        
        std::string body = response_body.str();
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "Content-Type: application/json\r\n"
                 << "Content-Length: " << body.length() << "\r\n"
                 << "\r\n"
                 << body;
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