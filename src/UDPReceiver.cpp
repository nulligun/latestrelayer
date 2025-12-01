#include "UDPReceiver.h"
#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

UDPReceiver::UDPReceiver(const std::string& name, uint16_t port, TSPacketQueue& queue,
                         uint32_t buffer_size)
    : name_(name),
      port_(port),
      queue_(queue),
      buffer_size_(buffer_size),
      running_(false),
      should_stop_(false),
      socket_fd_(-1),
      packets_received_(0),
      packets_dropped_(0),
      datagrams_received_(0) {
}

UDPReceiver::~UDPReceiver() {
    stop();
}

bool UDPReceiver::start() {
    if (running_.load()) {
        std::cerr << "[" << name_ << "] Already running" << std::endl;
        return false;
    }
    
    if (!createSocket()) {
        return false;
    }
    
    should_stop_ = false;
    receiver_thread_ = std::thread(&UDPReceiver::receiveLoop, this);
    
    std::cout << "[" << name_ << "] Started on UDP port " << port_ << std::endl;
    return true;
}

void UDPReceiver::stop() {
    if (!running_.load() && !receiver_thread_.joinable()) {
        return;
    }
    
    auto shutdown_start = std::chrono::steady_clock::now();
    std::cout << "[" << name_ << "] Stopping..." << std::endl;
    
    // Set stop flag first
    should_stop_ = true;
    
    // Close socket immediately to unblock recvfrom()
    closeSocket();
    std::cout << "[" << name_ << "] Socket closed, unblocking receive loop" << std::endl;
    
    // Wait for thread to finish
    if (receiver_thread_.joinable()) {
        receiver_thread_.join();
        auto shutdown_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - shutdown_start
        );
        std::cout << "[" << name_ << "] Thread joined (" << shutdown_duration.count() << "ms)" << std::endl;
    }
    
    std::cout << "[" << name_ << "] Stopped. Datagrams: "
              << datagrams_received_.load() << ", TS packets: "
              << packets_received_.load() << ", dropped: "
              << packets_dropped_.load() << std::endl;
}

bool UDPReceiver::createSocket() {
    // Create UDP socket
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        std::cerr << "[" << name_ << "] Failed to create socket: " 
                  << strerror(errno) << std::endl;
        return false;
    }
    
    // Set socket options to reuse address
    int reuse = 1;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        std::cerr << "[" << name_ << "] Failed to set SO_REUSEADDR: " 
                  << strerror(errno) << std::endl;
        closeSocket();
        return false;
    }
    
    // Set receive buffer size from configuration
    int rcvbuf = static_cast<int>(buffer_size_);
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        std::cerr << "[" << name_ << "] Warning: Failed to set receive buffer size to "
                  << buffer_size_ << " bytes" << std::endl;
    } else {
        std::cout << "[" << name_ << "] Set UDP receive buffer to " << buffer_size_ << " bytes" << std::endl;
    }
    
    // Bind to port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);
    
    if (bind(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[" << name_ << "] Failed to bind to port " << port_ 
                  << ": " << strerror(errno) << std::endl;
        closeSocket();
        return false;
    }
    
    return true;
}

void UDPReceiver::closeSocket() {
    if (socket_fd_ >= 0) {
        // Shutdown the socket first to interrupt any blocking recvfrom() calls
        // This is critical - close() alone does NOT reliably unblock recvfrom()
        shutdown(socket_fd_, SHUT_RDWR);
        close(socket_fd_);
        socket_fd_ = -1;
    }
}

void UDPReceiver::receiveLoop() {
    running_ = true;
    
    // Buffer size to accommodate multiple TS packets per UDP datagram
    // 2048 bytes allows up to 10 TS packets (10 * 188 = 1880 bytes)
    const size_t MAX_DATAGRAM_SIZE = 2048;
    uint8_t buffer[MAX_DATAGRAM_SIZE];
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);
    
    std::cout << "[" << name_ << "] Receive loop started (max datagram size: "
              << MAX_DATAGRAM_SIZE << " bytes)" << std::endl;
    
    // Reduced stats logging frequency to avoid log spam
    uint64_t stats_interval = 10000; // Log stats every 10000 datagrams (was 1000)
    uint64_t total_packets_in_datagrams = 0;
    
    while (!should_stop_.load()) {
        // Receive UDP datagram (may contain multiple TS packets)
        ssize_t bytes_received = recvfrom(
            socket_fd_,
            buffer,
            MAX_DATAGRAM_SIZE,
            0,
            (struct sockaddr*)&sender_addr,
            &sender_len
        );
        
        if (should_stop_.load()) {
            break;
        }
        
        if (bytes_received < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            // EBADF or ECONNRESET means socket was shutdown - this is expected during stop
            if (errno == EBADF || errno == ECONNRESET || errno == ENOTCONN) {
                if (should_stop_.load()) {
                    std::cout << "[" << name_ << "] Socket shutdown detected, exiting cleanly" << std::endl;
                } else {
                    std::cerr << "[" << name_ << "] Unexpected socket error: "
                              << strerror(errno) << std::endl;
                }
                break;
            }
            std::cerr << "[" << name_ << "] recvfrom error: "
                      << strerror(errno) << std::endl;
            break;
        }
        
        if (bytes_received == 0) {
            continue;
        }
        
        datagrams_received_++;
        
        // Check if datagram size is a multiple of TS packet size
        if (bytes_received % ts::PKT_SIZE != 0) {
            std::cerr << "[" << name_ << "] Warning: Datagram size " << bytes_received
                      << " is not a multiple of TS packet size (" << ts::PKT_SIZE
                      << "). Data may be incomplete." << std::endl;
        }
        
        // Extract TS packets from the datagram
        size_t num_packets = bytes_received / ts::PKT_SIZE;
        size_t packets_extracted = 0;
        
        for (size_t i = 0; i < num_packets; i++) {
            size_t offset = i * ts::PKT_SIZE;
            
            // Validate sync byte for each packet
            if (buffer[offset] != ts::SYNC_BYTE) {
                std::cerr << "[" << name_ << "] Invalid sync byte at packet " << i
                          << " in datagram: 0x" << std::hex
                          << static_cast<int>(buffer[offset]) << std::dec << std::endl;
                continue;
            }
            
            // Create TS packet and push to queue
            ts::TSPacket packet;
            memcpy(packet.b, buffer + offset, ts::PKT_SIZE);
            
            if (!queue_.push(packet)) {
                packets_dropped_++;
            } else {
                packets_received_++;
                packets_extracted++;
            }
        }
        
        total_packets_in_datagrams += packets_extracted;
        
        // Periodic statistics logging
        if (datagrams_received_ % stats_interval == 0) {
            double avg_packets_per_datagram = static_cast<double>(total_packets_in_datagrams) /
                                               static_cast<double>(datagrams_received_.load());
            std::cout << "[" << name_ << "] Stats: Datagrams: " << datagrams_received_.load()
                      << ", TS packets: " << packets_received_.load()
                      << ", Avg packets/datagram: " << avg_packets_per_datagram
                      << ", Dropped: " << packets_dropped_.load() << std::endl;
        }
    }
    
    running_ = false;
    std::cout << "[" << name_ << "] Receive loop ended" << std::endl;
}