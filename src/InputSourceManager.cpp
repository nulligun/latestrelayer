#include "InputSourceManager.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

InputSourceManager::InputSourceManager(const std::string& state_file_path)
    : state_file_path_(state_file_path),
      current_source_(InputSource::CAMERA) {  // Default to camera
}

bool InputSourceManager::load() {
    std::lock_guard<std::mutex> lock(file_mutex_);
    
    std::ifstream file(state_file_path_);
    if (!file.is_open()) {
        // File doesn't exist - use default (CAMERA)
        std::cout << "[InputSourceManager] No state file found at " << state_file_path_
                  << ", defaulting to CAMERA" << std::endl;
        current_source_ = InputSource::CAMERA;
        return true;
    }
    
    try {
        // Read entire file
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();
        file.close();
        
        // Simple JSON parsing - look for "source": "camera" or "source": "drone"
        // We're doing minimal JSON parsing to avoid adding a dependency
        std::string source_str;
        
        // Find "source" key
        size_t pos = content.find("\"source\"");
        if (pos == std::string::npos) {
            std::cerr << "[InputSourceManager] Invalid state file: missing 'source' key" << std::endl;
            current_source_ = InputSource::CAMERA;
            return true;  // Return true but use default
        }
        
        // Find the colon and then the value
        pos = content.find(':', pos);
        if (pos == std::string::npos) {
            std::cerr << "[InputSourceManager] Invalid state file: malformed JSON" << std::endl;
            current_source_ = InputSource::CAMERA;
            return true;
        }
        
        // Find the opening quote of the value
        pos = content.find('"', pos);
        if (pos == std::string::npos) {
            std::cerr << "[InputSourceManager] Invalid state file: missing value" << std::endl;
            current_source_ = InputSource::CAMERA;
            return true;
        }
        
        // Extract the value
        size_t start = pos + 1;
        size_t end = content.find('"', start);
        if (end == std::string::npos) {
            std::cerr << "[InputSourceManager] Invalid state file: unterminated string" << std::endl;
            current_source_ = InputSource::CAMERA;
            return true;
        }
        
        source_str = content.substr(start, end - start);
        
        // Convert to lowercase for comparison
        std::transform(source_str.begin(), source_str.end(), source_str.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        
        InputSource source;
        if (fromString(source_str, source)) {
            current_source_ = source;
            std::cout << "[InputSourceManager] Loaded input source: " << toString(source) << std::endl;
        } else {
            std::cerr << "[InputSourceManager] Unknown source value: " << source_str
                      << ", defaulting to CAMERA" << std::endl;
            current_source_ = InputSource::CAMERA;
        }
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "[InputSourceManager] Error reading state file: " << e.what() << std::endl;
        current_source_ = InputSource::CAMERA;
        return true;  // Return true but use default
    }
}

bool InputSourceManager::save() {
    // Called with file_mutex_ held
    
    try {
        std::ofstream file(state_file_path_);
        if (!file.is_open()) {
            std::cerr << "[InputSourceManager] Failed to open state file for writing: "
                      << state_file_path_ << std::endl;
            return false;
        }
        
        // Write simple JSON
        file << "{\n";
        file << "  \"source\": \"" << getInputSourceString() << "\"\n";
        file << "}\n";
        
        file.close();
        
        std::cout << "[InputSourceManager] Saved input source: " << getInputSourceString()
                  << " to " << state_file_path_ << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "[InputSourceManager] Error writing state file: " << e.what() << std::endl;
        return false;
    }
}

InputSource InputSourceManager::getInputSource() const {
    return current_source_.load();
}

bool InputSourceManager::setInputSource(InputSource source) {
    std::lock_guard<std::mutex> lock(file_mutex_);
    
    InputSource old_source = current_source_.load();
    current_source_ = source;
    
    if (!save()) {
        // Revert on save failure
        current_source_ = old_source;
        return false;
    }
    
    if (old_source != source) {
        std::cout << "[InputSourceManager] Input source changed: " << toString(old_source)
                  << " -> " << toString(source) << std::endl;
        std::cout << "[InputSourceManager] Note: Change will take effect after multiplexer restart"
                  << std::endl;
    }
    
    return true;
}

std::string InputSourceManager::getInputSourceString() const {
    return toString(current_source_.load());
}

bool InputSourceManager::setInputSourceFromString(const std::string& source_str) {
    InputSource source;
    if (!fromString(source_str, source)) {
        std::cerr << "[InputSourceManager] Invalid source string: " << source_str << std::endl;
        return false;
    }
    return setInputSource(source);
}

std::string InputSourceManager::toString(InputSource source) {
    switch (source) {
        case InputSource::CAMERA:
            return "camera";
        case InputSource::DRONE:
            return "drone";
        default:
            return "unknown";
    }
}

bool InputSourceManager::fromString(const std::string& str, InputSource& source) {
    // Convert to lowercase for comparison
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    
    if (lower == "camera") {
        source = InputSource::CAMERA;
        return true;
    } else if (lower == "drone") {
        source = InputSource::DRONE;
        return true;
    }
    
    return false;
}