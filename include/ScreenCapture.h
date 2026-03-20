#pragma once

#ifndef ULL_STREAMER_SCREEN_CAPTURE_H
#define ULL_STREAMER_SCREEN_CAPTURE_H

#include "StreamCommon.h"

namespace ull_streamer {

/**
 * @brief Ultra-low latency screen capture class using DXGI Desktop Duplication API
 * 
 * Captures frames directly from GPU memory to minimize CPU copies.
 * Captured textures can be passed directly to hardware encoders.
 */
class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();
    
    // Disable copy and move
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;
    
    /**
     * @brief Initialize Desktop Duplication
     * @param config Streaming configuration
     * @return Success status
     */
    bool initialize(const StreamConfig& config);
    
    /**
     * @brief Resources cleanup
     */
    void shutdown();
    
    /**
     * @brief Capture one frame (blocking)
     * @param frame Captured frame data (output)
     * @param timeoutMs Timeout in milliseconds
     * @return Success status
     */
    bool captureFrame(std::shared_ptr<CapturedFrame>& frame, uint32_t timeoutMs = 16);
    
    /**
     * @brief Get last captured GPU texture (zero-copy path)
     * @return D3D11 texture pointer
     */
    ID3D11Texture2D* getLastCapturedTexture() const { return lastTexture_.Get(); }
    
    /**
     * @brief Access D3D11 device
     */
    ID3D11Device* getDevice() const { return device_.Get(); }
    ID3D11DeviceContext* getContext() const { return context_.Get(); }
    
    /**
     * @brief Current capture resolution
     */
    uint32_t getWidth() const { return width_; }
    uint32_t getHeight() const { return height_; }
    
    /**
     * @brief Capture statistics
     */
    uint64_t getCapturedFrameCount() const { return capturedFrameCount_; }
    double getAverageCaptureTimeMs() const { return avgCaptureTimeMs_; }

private:
    bool initializeD3D11();
    bool initializeDesktopDuplication();
    bool createStagingTexture();
    void releaseFrame();
    
    // D3D11 resources
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> desktopDuplication_;
    ComPtr<ID3D11Texture2D> stagingTexture_;
    ComPtr<ID3D11Texture2D> lastTexture_;
    
    // Configuration
    StreamConfig config_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    
    // State
    bool initialized_ = false;
    bool frameAcquired_ = false;
    
    // Statistics
    std::atomic<uint64_t> capturedFrameCount_{0};
    double avgCaptureTimeMs_ = 0.0;
    std::chrono::high_resolution_clock::time_point lastCaptureTime_;
};

} // namespace ull_streamer

#endif // ULL_STREAMER_SCREEN_CAPTURE_H
