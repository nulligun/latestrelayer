#include "TCPOutput.h"
#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <thread>
#include <chrono>

TCPOutput::TCPOutput(const std::string& host, uint16_t port, const std::atomic<bool>& running)
    : host_(host),
      port_(port),
      sockfd_(-1),
      running_(running),
      connected_(false),
      packets_written_(0),
      bytes_written_(0) {
}

TCPOutput::~TCPOutput() {
    disconnect();
}

bool TCPOutput::connect() {
    if (connected_.load()) {
        std::cout << "[TCPOutput] Already connected" << std::endl;
        return true;
    }
    
    std::cout << "[TCPOutput] Connecting to " << host_ << ":" << port_ << "..." << std::endl;
    
    // Retry loop - continue until connected or shutdown requested
    while (running_.load()) {
        // Create TCP socket
        sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd_ < 0) {
            std::cerr << "[TCPOutput] Failed to create socket: " << strerror(errno) << std::endl;
            std::cerr << "[TCPOutput] Retrying in " << (TCP_RECONNECT_DELAY_MS / 1000) << " seconds..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(TCP_RECONNECT_DELAY_MS));
            continue;
        }
        
        // Set send buffer size (2MB to match ffmpeg-fallback)
        int bufsize = TCP_SEND_BUFFER_SIZE;
        if (setsockopt(sockfd_, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize)) < 0) {
            std::cerr << "[TCPOutput] Warning: Failed to set send buffer size: " << strerror(errno) << std::endl;
        }
        
        // Set TCP_NODELAY to reduce latency
        int flag = 1;
        if (setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
            std::cerr << "[TCPOutput] Warning: Failed to set TCP_NODELAY: " << strerror(errno) << std::endl;
        }
        
        // Resolve hostname using getaddrinfo (supports DNS and IP addresses)
        struct addrinfo hints, *result, *rp;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;        // IPv4
        hints.ai_socktype = SOCK_STREAM;  // TCP
        
        std::string port_str = std::to_string(port_);
        int ret = getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &result);
        if (ret != 0) {
            std::cerr << "[TCPOutput] Failed to resolve hostname '" << host_ << "': "
                      << gai_strerror(ret) << std::endl;
            close(sockfd_);
            sockfd_ = -1;
            
            if (!running_.load()) {
                return false;
            }
            
            std::cerr << "[TCPOutput] Retrying in " << (TCP_RECONNECT_DELAY_MS / 1000) << " seconds..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(TCP_RECONNECT_DELAY_MS));
            continue;
        }
        
        // Try each address until we successfully connect
        bool connection_success = false;
        for (rp = result; rp != nullptr; rp = rp->ai_next) {
            if (::connect(sockfd_, rp->ai_addr, rp->ai_addrlen) == 0) {
                connection_success = true;
                break;
            }
        }
        
        freeaddrinfo(result);
        
        if (!connection_success) {
            std::cerr << "[TCPOutput] Connection failed: " << strerror(errno) << std::endl;
            close(sockfd_);
            sockfd_ = -1;
            
            if (!running_.load()) {
                return false;
            }
            
            std::cerr << "[TCPOutput] Retrying in " << (TCP_RECONNECT_DELAY_MS / 1000) << " seconds..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(TCP_RECONNECT_DELAY_MS));
            continue;
        }
        
        // Successfully connected!
        connected_ = true;
        std::cout << "[TCPOutput] Connected to " << host_ << ":" << port_ << std::endl;
        std::cout << "[TCPOutput] TCP send buffer: " << TCP_SEND_BUFFER_SIZE << " bytes" << std::endl;
        
        return true;
    }
    
    // Shutdown requested during connection attempts
    return false;
}

void TCPOutput::disconnect() {
    if (sockfd_ >= 0) {
        std::cout << "[TCPOutput] Disconnecting..." << std::endl;
        shutdown(sockfd_, SHUT_RDWR);
        close(sockfd_);
        sockfd_ = -1;
    }
    connected_ = false;
    
    std::cout << "[TCPOutput] Statistics:" << std::endl;
    std::cout << "  Packets written: " << packets_written_.load() << std::endl;
    std::cout << "  Bytes written: " << bytes_written_.load() << std::endl;
}

bool TCPOutput::writePacket(const ts::TSPacket& packet) {
    // Auto-reconnect if not connected
    if (!connected_.load()) {
        std::cout << "[TCPOutput] Not connected - attempting to reconnect..." << std::endl;
        if (!connect()) {
            // connect() failed (likely shutdown requested)
            return false;
        }
    }
    
    // Write 188-byte TS packet
    ssize_t written = write(sockfd_, packet.b, ts::PKT_SIZE);
    
    if (written != ts::PKT_SIZE) {
        int err = errno;
        std::cerr << "[TCPOutput] Write failed - expected " << ts::PKT_SIZE
                  << " bytes, wrote " << written << " bytes, errno=" << err
                  << " (" << strerror(err) << ")" << std::endl;
        
        // Connection lost - cleanup and reconnect
        std::cout << "[TCPOutput] Connection lost - attempting to reconnect..." << std::endl;
        disconnect();
        
        if (!connect()) {
            // connect() failed (likely shutdown requested)
            return false;
        }
        
        // Successfully reconnected but this packet is lost
        std::cout << "[TCPOutput] Reconnected - packet dropped during reconnection" << std::endl;
        return false;
    }
    
    packets_written_++;
    bytes_written_ += ts::PKT_SIZE;
    
    return true;
}

bool TCPOutput::writePackets(const std::vector<ts::TSPacket>& packets) {
    for (const auto& packet : packets) {
        if (!writePacket(packet)) {
            return false;
        }
    }
    return true;
}