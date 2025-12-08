#ifndef TS_STREAM_REASSEMBLER_H
#define TS_STREAM_REASSEMBLER_H

#include <deque>
#include <vector>
#include <atomic>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <tsduck.h>

/**
 * TSStreamReassembler - MPEG-TS Stream Reassembly from Arbitrary Byte Chunks
 * 
 * Handles reassembly of MPEG-TS packets from UDP datagrams that may:
 * - Split TS packets across datagram boundaries
 * - Contain partial packets at the start/end
 * - Arrive with arbitrary sizes
 * 
 * Uses a 3-state sync verification algorithm:
 * 1. SEARCHING: Scan for sync byte (0x47)
 * 2. VERIFYING: Verify 3 consecutive packets at 188-byte intervals
 * 3. SYNCED: Extract aligned packets, verify each sync byte
 * 
 * Thread-safety: addData/getPackets should be called from single thread.
 * Statistics are atomic for safe cross-thread reads.
 */
class TSStreamReassembler {
public:
    enum class State {
        SEARCHING,  // Scanning for initial sync byte
        VERIFYING,  // Verifying sync pattern (need 3 consecutive packets)
        SYNCED      // Locked on, extracting packets
    };

    /**
     * Constructor
     * @param requiredSyncPackets Number of consecutive packets to verify (default: 3)
     * @param maxBufferSize Maximum buffer size in bytes (default: 1MB)
     */
    explicit TSStreamReassembler(size_t requiredSyncPackets = 3, 
                                  size_t maxBufferSize = 1024 * 1024)
        : requiredSyncPackets_(requiredSyncPackets)
        , maxBufferSize_(maxBufferSize) {}

    /**
     * Add raw bytes from UDP datagram
     * @param data Pointer to raw bytes
     * @param length Number of bytes
     */
    void addData(const uint8_t* data, size_t length) {
        totalBytesReceived_ += length;
        datagramCount_++;
        
        // Append data to buffer
        buffer_.insert(buffer_.end(), data, data + length);
        
        // Enforce buffer size limit
        if (buffer_.size() > maxBufferSize_) {
            size_t rawDiscard = buffer_.size() - maxBufferSize_;
            
            // CRITICAL FIX: Discard in multiples of TS_PACKET_SIZE to maintain alignment!
            // If we were synced, round UP to next packet boundary to avoid losing sync
            size_t toDiscard;
            if (state_ == State::SYNCED) {
                // Round UP to maintain packet alignment
                toDiscard = ((rawDiscard + TS_PACKET_SIZE - 1) / TS_PACKET_SIZE) * TS_PACKET_SIZE;
            } else {
                // Not synced anyway, just discard what we need
                toDiscard = rawDiscard;
            }
            
            // Make sure we don't discard more than buffer size
            if (toDiscard > buffer_.size()) {
                toDiscard = (buffer_.size() / TS_PACKET_SIZE) * TS_PACKET_SIZE;
            }
            
            // DEBUG: Log overflow event with context
            if (overflowEvents_ < 5 || overflowEvents_ % 100 == 0) {
                std::cerr << "[REASSEMBLER] OVERFLOW #" << (overflowEvents_ + 1)
                          << ": buffer=" << buffer_.size() << " bytes"
                          << ", rawDiscard=" << rawDiscard
                          << ", alignedDiscard=" << toDiscard
                          << " (" << (toDiscard / TS_PACKET_SIZE) << " packets)"
                          << ", state=" << stateToString(state_)
                          << ", packetsOutput=" << packetsOutput_.load() << "\n";
            }
            overflowEvents_++;
            
            if (toDiscard > 0) {
                buffer_.erase(buffer_.begin(), buffer_.begin() + toDiscard);
                bytesDiscarded_ += toDiscard;
                packetsDiscarded_ += toDiscard / TS_PACKET_SIZE;
            }
            
            // If we were synced and discarded aligned packets, we should STAY synced!
            // Only lose sync if we couldn't maintain alignment
            if (state_ == State::SYNCED && (toDiscard % TS_PACKET_SIZE) != 0) {
                syncLosses_++;
                state_ = State::SEARCHING;
                syncOffset_ = 0;
            }
            // If synced and aligned discard, stay synced - the next byte should still be 0x47
        }
        
        // Process buffer through state machine
        processBuffer();
        
        // DEBUG: Periodically log processing stats
        if (datagramCount_ % 500 == 0) {
            std::cerr << "[REASSEMBLER] Stats after " << datagramCount_ << " datagrams:\n"
                      << "  - Total bytes received: " << totalBytesReceived_ << "\n"
                      << "  - Pending buffer: " << buffer_.size() << " bytes\n"
                      << "  - State: " << stateToString(state_) << "\n"
                      << "  - Packets output: " << packetsOutput_.load() << "\n"
                      << "  - Overflow events: " << overflowEvents_ << "\n"
                      << "  - Sync losses: " << syncLosses_.load() << "\n";
        }
    }

    /**
     * Get complete TS packets extracted from stream
     * Clears internal output queue
     * @return Vector of complete TS packets
     */
    std::vector<ts::TSPacket> getPackets() {
        std::vector<ts::TSPacket> result;
        result.swap(outputQueue_);
        return result;
    }

    // Statistics (thread-safe atomic reads)
    size_t getBytesDiscarded() const { return bytesDiscarded_.load(); }
    size_t getSyncLosses() const { return syncLosses_.load(); }
    size_t getPacketsOutput() const { return packetsOutput_.load(); }
    size_t getPendingBytes() const { return buffer_.size(); }
    State getCurrentState() const { return state_; }

    /**
     * Reset reassembler state (for testing or error recovery)
     */
    void reset() {
        buffer_.clear();
        outputQueue_.clear();
        state_ = State::SEARCHING;
        syncOffset_ = 0;
        verifyCount_ = 0;
    }

private:
    static constexpr size_t TS_PACKET_SIZE = 188;
    static constexpr uint8_t TS_SYNC_BYTE = 0x47;

    std::deque<uint8_t> buffer_;            // Raw byte accumulator
    std::vector<ts::TSPacket> outputQueue_; // Ready packets
    State state_ = State::SEARCHING;
    size_t syncOffset_ = 0;                 // Current sync position in buffer
    size_t verifyCount_ = 0;                // Packets verified in VERIFYING state
    const size_t requiredSyncPackets_;
    const size_t maxBufferSize_;

    // Atomic statistics
    std::atomic<size_t> bytesDiscarded_{0};
    std::atomic<size_t> syncLosses_{0};
    std::atomic<size_t> packetsOutput_{0};
    
    // Debug counters
    size_t datagramCount_{0};
    size_t totalBytesReceived_{0};
    size_t overflowEvents_{0};
    size_t falseVerifyAttempts_{0};
    size_t packetsDiscarded_{0};
    
    const char* stateToString(State s) const {
        switch (s) {
            case State::SEARCHING: return "SEARCHING";
            case State::VERIFYING: return "VERIFYING";
            case State::SYNCED: return "SYNCED";
            default: return "UNKNOWN";
        }
    }

    /**
     * Check if byte at offset is a sync byte
     */
    bool isSyncByte(size_t offset) const {
        return offset < buffer_.size() && buffer_[offset] == TS_SYNC_BYTE;
    }
    
    /**
     * Extract PID from packet header at given offset
     * PID is 13 bits: 5 bits from byte 1 (bits 4-0) + 8 bits from byte 2
     */
    uint16_t extractPID(size_t offset) const {
        if (offset + 2 >= buffer_.size()) return 0xFFFF; // Invalid
        uint16_t pid = ((buffer_[offset + 1] & 0x1F) << 8) | buffer_[offset + 2];
        return pid;
    }
    
    /**
     * Check if a packet at offset has a valid TS header (sync byte + valid PID)
     * Valid PIDs are 0x0000-0x1FFF (8191)
     */
    bool isValidTSHeader(size_t offset) const {
        if (!isSyncByte(offset)) return false;
        if (offset + 2 >= buffer_.size()) return false;
        uint16_t pid = extractPID(offset);
        // PID must be in valid range (0x0000-0x1FFF)
        // Note: PID 0x1FFF is null packet, which is valid
        return pid <= 0x1FFF;
    }

    /**
     * Process buffer through state machine
     */
    void processBuffer() {
        bool continueProcessing = true;
        
        while (continueProcessing) {
            switch (state_) {
                case State::SEARCHING:
                    continueProcessing = processSearching();
                    break;
                
                case State::VERIFYING:
                    continueProcessing = processVerifying();
                    break;
                
                case State::SYNCED:
                    continueProcessing = processSynced();
                    break;
            }
        }
    }

    /**
     * SEARCHING state: Scan for sync byte with valid PID
     * @return true if state changed and processing should continue
     */
    bool processSearching() {
        // Scan buffer for sync byte with valid PID
        while (buffer_.size() >= TS_PACKET_SIZE) {
            if (isValidTSHeader(0)) {
                // Found potential sync with valid PID
                syncOffset_ = 0;
                verifyCount_ = 0;
                state_ = State::VERIFYING;
                
                // DEBUG: Log valid sync candidate found
                static size_t validCandidates = 0;
                validCandidates++;
                if (validCandidates <= 5 || validCandidates % 100 == 0) {
                    uint16_t pid = extractPID(0);
                    std::cerr << "[REASSEMBLER] Found valid sync candidate #" << validCandidates
                              << ": PID=" << pid << " (0x" << std::hex << pid << std::dec << ")\n";
                }
                
                return true; // State changed, continue processing
            }
            
            // Not a valid sync position (either not 0x47 or invalid PID)
            // DEBUG: Log rejected candidates
            if (buffer_[0] == TS_SYNC_BYTE) {
                // It was 0x47 but invalid PID - this is the false sync we're catching!
                static size_t rejectedFalseSync = 0;
                rejectedFalseSync++;
                if (rejectedFalseSync <= 10 || rejectedFalseSync % 500 == 0) {
                    uint16_t badPid = extractPID(0);
                    std::cerr << "[REASSEMBLER] Rejected false sync #" << rejectedFalseSync
                              << ": byte=0x47 but PID=" << badPid
                              << " (0x" << std::hex << badPid << std::dec << ") > 8191\n";
                }
            }
            
            buffer_.pop_front();
            bytesDiscarded_++;
        }
        
        return false; // Need more data
    }

    /**
     * VERIFYING state: Verify consecutive sync bytes with valid PIDs at 188-byte intervals
     * @return true if state changed and processing should continue
     */
    bool processVerifying() {
        // Check if we have enough data to verify next packet
        size_t nextPacketOffset = syncOffset_ + (verifyCount_ * TS_PACKET_SIZE);
        
        if (nextPacketOffset + TS_PACKET_SIZE > buffer_.size()) {
            return false; // Need more data
        }
        
        // Check for valid TS header (sync byte + valid PID) at expected position
        if (!isValidTSHeader(nextPacketOffset)) {
            // Verification failed - not a valid sync pattern
            falseVerifyAttempts_++;
            
            // DEBUG: Log verification failures (first few and periodically)
            if (falseVerifyAttempts_ <= 5 || falseVerifyAttempts_ % 100 == 0) {
                uint8_t byte0 = buffer_[nextPacketOffset];
                uint16_t pid = extractPID(nextPacketOffset);
                std::cerr << "[REASSEMBLER] Verify FAILED #" << falseVerifyAttempts_
                          << ": at offset " << nextPacketOffset
                          << ", byte=0x" << std::hex << std::setfill('0') << std::setw(2) << (int)byte0
                          << ", PID=" << std::dec << pid << " (0x" << std::hex << pid << std::dec << ")"
                          << ", verifyCount=" << verifyCount_
                          << ", bufferSize=" << buffer_.size() << "\n";
            }
            
            // Discard the candidate sync byte and search again
            buffer_.pop_front();
            bytesDiscarded_++;
            state_ = State::SEARCHING;
            return true; // State changed, continue processing
        }
        
        // Valid TS header verified at this position
        verifyCount_++;
        
        if (verifyCount_ >= requiredSyncPackets_) {
            // Successfully verified required number of packets
            // DEBUG: Log successful sync lock
            static size_t syncLockCount = 0;
            syncLockCount++;
            if (syncLockCount <= 3 || syncLockCount % 50 == 0) {
                std::cerr << "[REASSEMBLER] SYNC LOCKED #" << syncLockCount
                          << " after verifying " << verifyCount_ << " packets"
                          << ", buffer=" << buffer_.size() << " bytes\n";
            }
            state_ = State::SYNCED;
            return true; // State changed, continue processing
        }
        
        return false; // Need more data to verify next packet
    }

    /**
     * SYNCED state: Extract aligned packets
     * @return true if state changed and processing should continue
     */
    bool processSynced() {
        // Check if we have a complete packet
        if (buffer_.size() < TS_PACKET_SIZE) {
            return false; // Need more data
        }
        
        // Verify we still have a valid TS header (sync byte + valid PID)
        if (!isValidTSHeader(0)) {
            // Lost sync!
            size_t prevSyncLosses = syncLosses_.load();
            syncLosses_++;
            
            // DEBUG: Log sync loss with context
            if (prevSyncLosses < 10 || prevSyncLosses % 100 == 0) {
                uint16_t pid = extractPID(0);
                std::cerr << "[REASSEMBLER] SYNC LOST #" << (prevSyncLosses + 1)
                          << ": byte0=0x" << std::hex << std::setfill('0') << std::setw(2)
                          << (int)buffer_[0] << ", PID=" << std::dec << pid
                          << " (0x" << std::hex << pid << std::dec << ")"
                          << ", packetsOutput=" << packetsOutput_.load()
                          << ", bufferSize=" << buffer_.size() << "\n";
                
                // Show context: first 20 bytes of buffer
                std::cerr << "[REASSEMBLER] Buffer context: ";
                for (size_t i = 0; i < std::min(buffer_.size(), size_t(20)); i++) {
                    std::cerr << std::hex << std::setfill('0') << std::setw(2)
                              << (int)buffer_[i] << " ";
                }
                std::cerr << std::dec << "\n";
                
                // Check where next valid TS header is
                for (size_t i = 1; i < std::min(buffer_.size(), size_t(600)); i++) {
                    if (isValidTSHeader(i)) {
                        uint16_t nextPid = extractPID(i);
                        std::cerr << "[REASSEMBLER] Next valid header at offset " << i
                                  << " (" << (i % TS_PACKET_SIZE) << " bytes from packet boundary)"
                                  << ", PID=" << nextPid << "\n";
                        break;
                    }
                }
            }
            
            state_ = State::SEARCHING;
            return true; // State changed, continue processing
        }
        
        // Extract complete packet
        ts::TSPacket pkt;
        // Deque doesn't have .data(), so copy element by element
        for (size_t i = 0; i < TS_PACKET_SIZE; i++) {
            pkt.b[i] = buffer_[i];
        }
        buffer_.erase(buffer_.begin(), buffer_.begin() + TS_PACKET_SIZE);
        
        // DEBUG: Validate extracted packet
        size_t pktCount = packetsOutput_.load();
        if (pkt.b[0] != TS_SYNC_BYTE) {
            std::cerr << "[REASSEMBLER] ERROR: Extracted packet #" << (pktCount + 1)
                      << " has invalid sync byte 0x" << std::hex << (int)pkt.b[0] << std::dec << "!\n";
        }
        
        // DEBUG: Log packet extraction periodically
        if (pktCount < 5 || (pktCount < 200 && pktCount % 50 == 0) || pktCount % 1000 == 0) {
            ts::PID pid = pkt.getPID();
            std::cerr << "[REASSEMBLER] Extracted packet #" << (pktCount + 1)
                      << ": sync=0x" << std::hex << (int)pkt.b[0]
                      << ", PID=" << std::dec << pid
                      << ", CC=" << (int)(pkt.b[3] & 0x0F)
                      << ", pending=" << buffer_.size() << " bytes\n";
        }
        
        outputQueue_.push_back(pkt);
        packetsOutput_++;
        
        return true; // Extracted packet, continue to check for more
    }
};

#endif // TS_STREAM_REASSEMBLER_H