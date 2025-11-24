#include "Config.h"
#include "Multiplexer.h"
#include <iostream>
#include <csignal>
#include <atomic>

// Global flag for shutdown signal
static std::atomic<bool> g_shutdown(false);
static Multiplexer* g_multiplexer = nullptr;

// Signal handler for graceful shutdown
void signalHandler(int signal) {
    std::cout << "\n[Main] Received signal " << signal << ", shutting down..." << std::endl;
    g_shutdown = true;
    
    if (g_multiplexer) {
        g_multiplexer->stop();
    }
}

int main(int argc, char* argv[]) {
    std::cout << "=== TSDuck MPEG-TS Multiplexer ===" << std::endl;
    std::cout << "Version 1.0.0" << std::endl;
    std::cout << std::endl;
    
    // Check arguments
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <config.yaml>" << std::endl;
        return 1;
    }
    
    std::string config_file = argv[1];
    
    // Load configuration
    Config config;
    if (!config.loadFromFile(config_file)) {
        std::cerr << "[Main] Failed to load configuration from " << config_file << std::endl;
        return 1;
    }
    
    std::cout << "[Main] Configuration loaded successfully" << std::endl;
    config.print();
    std::cout << std::endl;
    
    // Setup signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    // Create multiplexer
    Multiplexer multiplexer(config);
    g_multiplexer = &multiplexer;
    
    // Initialize multiplexer
    if (!multiplexer.initialize()) {
        std::cerr << "[Main] Failed to initialize multiplexer" << std::endl;
        return 1;
    }
    
    std::cout << "[Main] Multiplexer initialized successfully" << std::endl;
    std::cout << "[Main] Starting multiplexing (press Ctrl+C to stop)..." << std::endl;
    std::cout << std::endl;
    
    // Run multiplexer
    multiplexer.run();
    
    // Clean shutdown
    std::cout << "[Main] Shutdown complete" << std::endl;
    return 0;
}