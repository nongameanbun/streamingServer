#pragma once

#ifndef STREAMING_PIPELINE_H
#define STREAMING_PIPELINE_H

#include "StreamCommon.h"
#include "ScreenCapture.h"
#include "HardwareEncoder.h"
#include "WebRTCManager.h"
#include "SignalingClient.h"

namespace ull_streamer {

/**
 * @brief Pipeline state
 */
enum class PipelineState {
    Stopped,
    Initializing,
    WaitingForPeer,
    Streaming,
    Error
};

/**
 * @brief Streaming statistics
 */
struct StreamingStats {
    // Capture
    uint64_t capturedFrames = 0;
    double avgCaptureTimeMs = 0.0;
    
    // Encoding
    uint64_t encodedFrames = 0;
    double avgEncodingTimeMs = 0.0;
    double currentBitrateMbps = 0.0;
    
    // Transmission
    uint64_t sentPackets = 0;
    uint64_t sentBytes = 0;
    
    // Latency
    double avgLatencyMs = 0.0;
    double currentFps = 0.0;
    
    // Drops
    uint64_t droppedFrames = 0;
};

/**
 * @brief Ultra-low latency streaming pipeline
 * 
 * Manages Capture -> Encoding -> WebRTC transmission.
 */
class StreamingPipeline : public WebRTCCallback, public SignalingCallback {
public:
    StreamingPipeline();
    ~StreamingPipeline();
    
    StreamingPipeline(const StreamingPipeline&) = delete;
    StreamingPipeline& operator=(const StreamingPipeline&) = delete;
    
    /**
     * @brief Initialize pipeline
     * @param config Streaming configuration
     * @return Success status
     */
    bool initialize(const StreamConfig& config);
    
    /**
     * @brief Start streaming
     * @return Success status
     */
    bool start();
    
    /**
     * @brief Stop streaming
     */
    void stop();
    
    /**
     * @brief Resources cleanup
     */
    void shutdown();
    
    /**
     * @brief Current state
     */
    PipelineState getState() const { return state_; }
    
    /**
     * @brief Current statistics
     */
    StreamingStats getStats() const;
    
    /**
     * @brief Update settings (runtime)
     */
    void updateBitrate(uint32_t bitrate);
    void requestKeyFrame();

private:
    // WebRTCCallback implementation
    void onLocalDescription(const std::string& sdp, const std::string& type) override;
    void onLocalCandidate(const IceCandidate& candidate) override;
    void onStateChange(WebRTCState state) override;
    void onError(const std::string& error) override;
    void onKeyFrameRequest() override;
    
    // SignalingCallback implementation
    void onConnected() override;
    void onDisconnected() override;
    void onPeerJoined(const std::string& peerId) override;
    void onPeerLeft(const std::string& peerId) override;
    void onOffer(const std::string& peerId, const std::string& sdp) override;
    void onAnswer(const std::string& peerId, const std::string& sdp) override;
    void onCandidate(const std::string& peerId, const IceCandidate& candidate) override;
    
    // Pipeline threads
    void captureThread();
    void encodeAndSendThread();
    
    // Components
    std::unique_ptr<ScreenCapture> screenCapture_;
    std::unique_ptr<HardwareEncoder> encoder_;
    std::unique_ptr<WebRTCManager> webrtcManager_;
    std::unique_ptr<SignalingClient> signalingClient_;
    
    // Frame queue (Capture -> Encode)
    ThreadSafeQueue<std::shared_ptr<CapturedFrame>> frameQueue_;
    
    // Configuration
    StreamConfig config_;
    
    // State
    std::atomic<PipelineState> state_{PipelineState::Stopped};
    std::atomic<bool> running_{false};
    
    // Threads
    std::thread captureThread_;
    std::thread encodeThread_;
    
    // Connection management
    std::string remotePeerId_;
    std::mutex connectionMutex_;
    
    // Statistics
    mutable std::mutex statsMutex_;
    std::chrono::high_resolution_clock::time_point startTime_;
    uint64_t frameCountForFps_ = 0;
    std::chrono::high_resolution_clock::time_point lastFpsTime_;
    double currentFps_ = 0.0;
    double avgLatencyMs_ = 0.0;
};

} // namespace ull_streamer

#endif // STREAMING_PIPELINE_H
