#pragma once

#include <string>
#include <mutex>
#include <atomic>
#include <functional>

/**
 * InputSource enum - represents the active input source for the multiplexer
 */
enum class InputSource {
    CAMERA,  // SRT camera input (via ffmpeg-srt-live UDP)
    DRONE    // Drone RTMP input (via RTMPReceiver)
};

/**
 * Callback type for input source changes
 * @param old_source The previous input source
 * @param new_source The new input source
 */
using InputSourceChangeCallback = std::function<void(InputSource old_source, InputSource new_source)>;

/**
 * InputSourceManager - Manages which input source (camera/drone) is active
 *
 * This class handles:
 * - Persisting the input source selection to a JSON file
 * - Loading the input source selection on startup
 * - Providing thread-safe access to the current selection
 * - Notifying registered callbacks when input source changes
 *
 * Runtime switching is supported via the callback mechanism. When setInputSource()
 * is called, registered callbacks are notified to perform the actual switch.
 */
class InputSourceManager {
public:
    /**
     * Constructor
     * @param state_file_path Path to the JSON file for persisting state
     */
    explicit InputSourceManager(const std::string& state_file_path);
    ~InputSourceManager() = default;
    
    // Prevent copying
    InputSourceManager(const InputSourceManager&) = delete;
    InputSourceManager& operator=(const InputSourceManager&) = delete;
    
    /**
     * Load state from file. If file doesn't exist, defaults to CAMERA.
     * @return true if loaded successfully (or defaulted), false on error
     */
    bool load();
    
    /**
     * Get the current input source
     * @return Current InputSource value
     */
    InputSource getInputSource() const;
    
    /**
     * Set the input source and persist to file
     * Note: This only saves the value. The actual switch happens on restart.
     * @param source New input source
     * @return true if saved successfully, false on error
     */
    bool setInputSource(InputSource source);
    
    /**
     * Get the input source as a string ("camera" or "drone")
     */
    std::string getInputSourceString() const;
    
    /**
     * Set input source from string
     * @param source_str "camera" or "drone"
     * @return true if valid and saved, false if invalid string or save error
     */
    bool setInputSourceFromString(const std::string& source_str);
    
    /**
     * Check if input source is camera
     */
    bool isCamera() const { return current_source_.load() == InputSource::CAMERA; }
    
    /**
     * Check if input source is drone
     */
    bool isDrone() const { return current_source_.load() == InputSource::DRONE; }
    
    /**
     * Set callback for input source changes
     * The callback will be invoked whenever setInputSource() changes the source.
     * The callback is invoked AFTER the source value is updated.
     * @param callback Function to call on source change
     */
    void setInputSourceChangeCallback(InputSourceChangeCallback callback);
    
    /**
     * Convert InputSource enum to string
     */
    static std::string toString(InputSource source);
    
    /**
     * Convert string to InputSource enum
     * @param str "camera" or "drone"
     * @param source Output parameter
     * @return true if valid string, false otherwise
     */
    static bool fromString(const std::string& str, InputSource& source);
    
private:
    /**
     * Save current state to file
     * @return true if saved successfully
     */
    bool save();
    
    std::string state_file_path_;
    std::atomic<InputSource> current_source_;
    mutable std::mutex file_mutex_;  // Protects file operations
    
    // Callback for input source changes
    InputSourceChangeCallback change_callback_;
    std::mutex callback_mutex_;  // Protects callback invocation
};