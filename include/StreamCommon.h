#pragma once

#ifndef ULL_STREAMER_COMMON_H
#define ULL_STREAMER_COMMON_H

#include <cstdint>
#include <memory>
#include <functional>
#include <chrono>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <queue>
#include <condition_variable>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <fstream>

// Windows headers
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

// Use ComPtr for safe COM object management
using Microsoft::WRL::ComPtr;

namespace ull_streamer {

// ============================================================================
// Configuration Constants - Ultra Low Latency Optimized
// ============================================================================

struct StreamConfig {
    // Capture region (screen coordinates)
    uint32_t captureX = 0;            // X offset for region capture
    uint32_t captureY = 0;            // Y offset for region capture
    uint32_t width = 1366;            // Capture width (original)
    uint32_t height = 768;            // Capture height (original)
    
    // Framerate
    uint32_t targetFPS = 30;          // 30fps for stable encoding on i5-4690
    
    // Encoder settings for ultra-low latency
    uint32_t bitrate = 4000000;       // 4 Mbps CBR (Better quality default)
    uint32_t gopSize = 60;            // 2 seconds @ 30fps (Stable GOP)
    uint32_t maxBFrames = 0;          // NO B-frames for lowest latency
    
    // Preset and tuning
    std::string preset = "p4";        // NVENC balanced preset (p4 = balanced quality/speed)
    std::string tune = "ull";         // Ultra low latency tuning
    
    // Buffer settings
    uint32_t vbvBufferSize = 1;       // Minimal VBV buffer (in frames)
    uint32_t lookahead = 0;           // No lookahead for zero latency
    
    // Rate control
    bool useCBR = true;               // Constant bitrate for network stability
    
    // WebRTC settings
    std::string signalingServerUrl = "ws://localhost:8765/ws";
    std::string roomId = "default";
    std::string peerId = "sender";
    
    std::vector<std::string> stunServers = {
        "stun:stun.l.google.com:19302",
        "stun:stun1.l.google.com:19302",
        "stun:stun2.l.google.com:19302",
        "stun:stun3.l.google.com:19302",
        "stun:stun4.l.google.com:19302"
    };

    // TURN servers
    std::vector<std::string> turnServers = {};
    std::string turnUsername = "";
    std::string turnPassword = "";
};

// ============================================================================
// Frame Data Structure
// ============================================================================

struct CapturedFrame {
    std::vector<uint8_t> data;        // Raw frame data (NV12 or BGRA)
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    int64_t timestamp = 0;            // Microseconds
    bool isKeyFrame = false;
    
    // GPU texture reference (for zero-copy path)
    ComPtr<ID3D11Texture2D> gpuTexture;
    bool hasGpuTexture = false;
};

struct EncodedPacket {
    std::vector<uint8_t> data;
    int64_t pts = 0;
    int64_t dts = 0;
    bool isKeyFrame = false;
    int64_t captureTimestamp = 0;     // For latency measurement
};

// ============================================================================
// Thread-safe Queue for Pipeline
// ============================================================================

template<typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(size_t maxSize = 2) : maxSize_(maxSize) {}
    
    bool push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // Drop oldest frame if queue is full (low latency strategy)
        while (queue_.size() >= maxSize_) {
            queue_.pop();
            droppedCount_++;
        }
        
        queue_.push(std::move(item));
        lock.unlock();
        cv_.notify_one();
        return true;
    }
    
    bool pop(T& item, std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || stopped_; })) {
            return false;
        }
        
        if (stopped_ && queue_.empty()) {
            return false;
        }
        
        if (queue_.empty()) return false;

        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }
    
    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        cv_.notify_all();
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) queue_.pop();
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
    
    uint64_t getDroppedCount() const { return droppedCount_; }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    size_t maxSize_;
    std::atomic<bool> stopped_{false};
    std::atomic<uint64_t> droppedCount_{0};
};

// ============================================================================
// Callback Types
// ============================================================================

using FrameCapturedCallback = std::function<void(std::shared_ptr<CapturedFrame>)>;
using PacketEncodedCallback = std::function<void(std::shared_ptr<EncodedPacket>)>;
using ConnectionStateCallback = std::function<void(const std::string& state)>;
using ErrorCallback = std::function<void(const std::string& error)>;

// ============================================================================
// Utility Functions
// ============================================================================

inline int64_t getCurrentTimestampUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

inline void logInfo(const std::string& msg) {
    // Disabled for release
}

inline void logError(const std::string& msg) {
    // Disabled for release
}

inline void logDebug(const std::string& msg) {
    // Disabled for release
}

// ============================================================================
// HRESULT Check Macro
// ============================================================================

#define HR_CHECK(hr, msg) \
    if (FAILED(hr)) { \
        throw std::runtime_error(std::string(msg) + " HRESULT: " + std::to_string(hr)); \
    }

} // namespace ull_streamer

#endif // ULL_STREAMER_COMMON_H
