#include "StreamingPipeline.h"

namespace ull_streamer {

StreamingPipeline::StreamingPipeline() 
    : frameQueue_(2)  // Max 2 frames buffer (Ultra Low Latency)
{
}

StreamingPipeline::~StreamingPipeline() {
    shutdown();
}

bool StreamingPipeline::initialize(const StreamConfig& config) {
    if (state_ != PipelineState::Stopped) {
        logError("Pipeline is not stopped");
        return false;
    }
    
    state_ = PipelineState::Initializing;
    config_ = config;
    
    logInfo("=== Ultra Low Latency Streamer ===");
    logInfo("Resolution: " + std::to_string(config_.width) + "x" + std::to_string(config_.height));
    logInfo("Target FPS: " + std::to_string(config_.targetFPS));
    logInfo("Bitrate: " + std::to_string(config_.bitrate / 1000000) + " Mbps");
    logInfo("GOP Size: " + std::to_string(config_.gopSize));
    
    try {
        // 1. Initialize screen capture
        logInfo("Initializing screen capture...");
        screenCapture_ = std::make_unique<ScreenCapture>();
        if (!screenCapture_->initialize(config_)) {
            throw std::runtime_error("Failed to initialize screen capture");
        }
        
        // 2. Initialize hardware encoder
        logInfo("Initializing hardware encoder...");
        encoder_ = std::make_unique<HardwareEncoder>();
        if (!encoder_->initialize(config_, HWEncoderType::QSV)) {
            throw std::runtime_error("Failed to initialize encoder");
        }
        logInfo("Using encoder: " + encoder_->getEncoderName());
        
        // 3. Initialize WebRTC manager
        logInfo("Initializing WebRTC...");
        webrtcManager_ = std::make_unique<WebRTCManager>();
        if (!webrtcManager_->initialize(config_, this)) {
            throw std::runtime_error("Failed to initialize WebRTC");
        }
        
        // Set SPS/PPS
        webrtcManager_->setVideoExtraData(encoder_->getExtraData());
        
        // 4. Initialize signaling client and connect
        logInfo("Connecting to signaling server...");
        signalingClient_ = std::make_unique<SignalingClient>();
        if (!signalingClient_->connect(config_.signalingServerUrl, 
                                        config_.roomId, 
                                        config_.peerId, 
                                        this)) {
            throw std::runtime_error("Failed to connect to signaling server");
        }
        
        state_ = PipelineState::WaitingForPeer;
        logInfo("Pipeline initialized. Waiting for peer...");
        return true;
        
    } catch (const std::exception& e) {
        logError("Initialization failed: " + std::string(e.what()));
        state_ = PipelineState::Error;
        shutdown();
        return false;
    }
}

bool StreamingPipeline::start() {
    if (state_ != PipelineState::WaitingForPeer && state_ != PipelineState::Stopped) {
        // Wait if still initializing
        if (state_ == PipelineState::Initializing) {
            logInfo("Waiting for initialization to complete...");
            return true;
        }
        return false;
    }
    
    if (running_) {
        return true;
    }
    
    running_ = true;
    frameQueue_.clear();
    
    // Reset stats
    startTime_ = std::chrono::high_resolution_clock::now();
    lastFpsTime_ = startTime_;
    frameCountForFps_ = 0;
    
    // Start capture thread
    captureThread_ = std::thread(&StreamingPipeline::captureThread, this);
    
    // Start encode/send thread
    encodeThread_ = std::thread(&StreamingPipeline::encodeAndSendThread, this);
    
    state_ = PipelineState::Streaming;
    logInfo("Streaming started");
    
    return true;
}

void StreamingPipeline::stop() {
    running_ = false;
    frameQueue_.stop();
    
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    
    if (encodeThread_.joinable()) {
        encodeThread_.join();
    }
    
    if (state_ == PipelineState::Streaming) {
        state_ = PipelineState::WaitingForPeer;
    }
    
    logInfo("Streaming stopped");
}

void StreamingPipeline::shutdown() {
    stop();
    
    if (signalingClient_) {
        signalingClient_->disconnect();
        signalingClient_.reset();
    }
    
    if (webrtcManager_) {
        webrtcManager_->shutdown();
        webrtcManager_.reset();
    }
    
    if (encoder_) {
        encoder_->shutdown();
        encoder_.reset();
    }
    
    if (screenCapture_) {
        screenCapture_->shutdown();
        screenCapture_.reset();
    }
    
    state_ = PipelineState::Stopped;
    logInfo("Pipeline shutdown complete");
}

void StreamingPipeline::captureThread() {
    logInfo("Capture thread started");
    
    const auto frameDuration = std::chrono::microseconds(1000000 / config_.targetFPS);
    auto nextFrameTime = std::chrono::high_resolution_clock::now();
    
    while (running_) {
        // Capture frame
        std::shared_ptr<CapturedFrame> frame;
        if (screenCapture_->captureFrame(frame, 16)) {
            // Debug: Dump first frame
            static bool frameDumped = false;
            if (!frameDumped) {
                std::ofstream file("captured_debug.ppm", std::ios::binary);
                file << "P6\n" << frame->width << " " << frame->height << "\n255\n";
                // BGRA -> RGB conv (accounting for pitch/stride)
                std::vector<uint8_t> rgb(frame->width * frame->height * 3);
                const uint8_t* src = frame->data.data();
                uint32_t pitch = frame->pitch;
                logInfo("Frame dump: " + std::to_string(frame->width) + "x" + 
                        std::to_string(frame->height) + ", pitch=" + std::to_string(pitch));
                
                size_t dstIdx = 0;
                for(size_t y = 0; y < frame->height; y++) {
                    const uint8_t* row = src + (y * pitch);
                    for(size_t x = 0; x < frame->width; x++) {
                        rgb[dstIdx++] = row[x*4 + 2]; // R
                        rgb[dstIdx++] = row[x*4 + 1]; // G
                        rgb[dstIdx++] = row[x*4 + 0]; // B
                    }
                }
                file.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
                file.close();
                logInfo("Dumped first frame to captured_debug.ppm");
                frameDumped = true;
            }

            // Add to queue (drop old frames if full)
            frameQueue_.push(std::move(frame));
        }
        
        // Calculate next frame timing
        nextFrameTime += frameDuration;
        auto now = std::chrono::high_resolution_clock::now();
        
        if (nextFrameTime > now) {
            std::this_thread::sleep_until(nextFrameTime);
        } else {
            // Reset timing if late
            nextFrameTime = now;
        }
    }
    
    logInfo("Capture thread stopped");
}

void StreamingPipeline::encodeAndSendThread() {
    logInfo("Encode/Send thread started");
    
    while (running_) {
        // Pop frame
        std::shared_ptr<CapturedFrame> frame;
        if (!frameQueue_.pop(frame, std::chrono::milliseconds(100))) {
            continue;
        }
        
        // Check WebRTC connection
        if (!webrtcManager_ || !webrtcManager_->isConnected()) {
            continue;
        }
        
        // Encoding
        std::vector<std::shared_ptr<EncodedPacket>> packets;
        if (encoder_->encodeFrame(frame, packets)) {
            // Send
            for (const auto& packet : packets) {
                webrtcManager_->sendVideoPacket(packet);
                
                // Calculate latency
                int64_t now = getCurrentTimestampUs();
                double latencyMs = (now - packet->captureTimestamp) / 1000.0;
                
                std::lock_guard<std::mutex> lock(statsMutex_);
                avgLatencyMs_ = avgLatencyMs_ * 0.9 + latencyMs * 0.1;
            }
            
            // Calculate FPS
            frameCountForFps_++;
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration<double>(now - lastFpsTime_).count();
            
            if (elapsed >= 1.0) {
                std::lock_guard<std::mutex> lock(statsMutex_);
                currentFps_ = frameCountForFps_ / elapsed;
                frameCountForFps_ = 0;
                lastFpsTime_ = now;
                
                // Print stats periodically
                // logInfo("FPS: " + std::to_string(static_cast<int>(currentFps_)) + 
                //         " | Latency: " + std::to_string(static_cast<int>(avgLatencyMs_)) + "ms" +
                //         " | Bitrate: " + std::to_string(encoder_->getAverageBitrate() / 1000000.0) + " Mbps");
            }
        }
    }
    
    logInfo("Encode/Send thread stopped");
}

StreamingStats StreamingPipeline::getStats() const {
    StreamingStats stats;
    
    if (screenCapture_) {
        stats.capturedFrames = screenCapture_->getCapturedFrameCount();
        stats.avgCaptureTimeMs = screenCapture_->getAverageCaptureTimeMs();
    }
    
    if (encoder_) {
        stats.encodedFrames = encoder_->getEncodedFrameCount();
        stats.avgEncodingTimeMs = encoder_->getAverageEncodingTimeMs();
        stats.currentBitrateMbps = encoder_->getAverageBitrate() / 1000000.0;
    }
    
    if (webrtcManager_) {
        stats.sentPackets = webrtcManager_->getPacketsSent();
        stats.sentBytes = webrtcManager_->getBytesSent();
    }
    
    stats.droppedFrames = frameQueue_.getDroppedCount();
    
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats.avgLatencyMs = avgLatencyMs_;
        stats.currentFps = currentFps_;
    }
    
    return stats;
}

void StreamingPipeline::updateBitrate(uint32_t bitrate) {
    config_.bitrate = bitrate;
    // TODO: Implement runtime bitrate update
    logInfo("Bitrate updated to " + std::to_string(bitrate / 1000000) + " Mbps");
}



// WebRTCCallback Implementation
void StreamingPipeline::onLocalDescription(const std::string& sdp, const std::string& type) {
    std::lock_guard<std::mutex> lock(connectionMutex_);
    
    if (signalingClient_ && !remotePeerId_.empty()) {
        if (type == "offer") {
            signalingClient_->sendOffer(remotePeerId_, sdp);
        } else {
            signalingClient_->sendAnswer(remotePeerId_, sdp);
        }
    }
}

void StreamingPipeline::onLocalCandidate(const IceCandidate& candidate) {
    std::lock_guard<std::mutex> lock(connectionMutex_);
    
    // Trickle ICE: Send as soon as generated
    if (signalingClient_ && !remotePeerId_.empty()) {
        signalingClient_->sendCandidate(remotePeerId_, candidate);
    }
}

void StreamingPipeline::onStateChange(WebRTCState state) {
    switch (state) {
        case WebRTCState::Connected:
            logInfo("WebRTC connected - starting stream");
            if (state_ == PipelineState::WaitingForPeer) {
                start();
            }
            // Force keyframe to ensure instant video
            requestKeyFrame();
            break;
            
        case WebRTCState::Disconnected:
            logInfo("WebRTC disconnected");
            if (state_ == PipelineState::Streaming) {
                stop();
                state_ = PipelineState::WaitingForPeer;
            }
            break;
            
        case WebRTCState::Failed:
            logError("WebRTC connection failed");
            state_ = PipelineState::Error;
            break;
            
        default:
            break;
    }
}

void StreamingPipeline::onError(const std::string& error) {
    logError("WebRTC error: " + error);
}

void StreamingPipeline::onKeyFrameRequest() {
    requestKeyFrame();
}

// SignalingCallback Implementation
void StreamingPipeline::onConnected() {
    logInfo("Connected to signaling server");
}

void StreamingPipeline::onDisconnected() {
    logInfo("Disconnected from signaling server");
    if (running_) {
        stop();
    }
}

void StreamingPipeline::onPeerJoined(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(connectionMutex_);
    
    logInfo("Peer joined: " + peerId);
    
    // Save remote peer ID
    remotePeerId_ = peerId;
    
    // Create offer as sender
    if (webrtcManager_) {
        webrtcManager_->createOffer();
        
        // Start 3-second watchdog timer for auto-reconnect
        std::thread([this, peerId]() {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            
            // Check if still not connected after 3 seconds
            if (state_ != PipelineState::Streaming && 
                remotePeerId_ == peerId && 
                webrtcManager_ && 
                !webrtcManager_->isConnected()) {
                
                logInfo("Connection timeout after 3s - auto-retrying offer...");
                webrtcManager_->createOffer();
            }
        }).detach();
    }
}

void StreamingPipeline::onPeerLeft(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(connectionMutex_);
    
    logInfo("Peer left: " + peerId);
    
    if (remotePeerId_ == peerId) {
        remotePeerId_.clear();
        
        // Stop streaming first
        if (running_) {
            stop();
        }
        
        // Async WebRTC re-initialization to prevent race condition
        std::thread([this]() {
            // Add delay to let any pending operations complete
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            if (webrtcManager_) {
                logInfo("Re-initializing WebRTC for next connection...");
                webrtcManager_->shutdown();
                
                // Small delay after shutdown
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                
                webrtcManager_->initialize(config_, this);
                webrtcManager_->setVideoExtraData(encoder_->getExtraData());
                logInfo("WebRTC ready for new connection");
            }
        }).detach();
    }
}

void StreamingPipeline::onOffer(const std::string& peerId, const std::string& sdp) {
    // Senders usually don't receive offers
    logDebug("Received offer from " + peerId + " (unexpected for sender)");
}

void StreamingPipeline::onAnswer(const std::string& peerId, const std::string& sdp) {
    std::lock_guard<std::mutex> lock(connectionMutex_);
    
    logInfo("Received answer from " + peerId);
    
    if (webrtcManager_) {
        webrtcManager_->setRemoteAnswer(sdp);
    }
}

void StreamingPipeline::onCandidate(const std::string& peerId, const IceCandidate& candidate) {
    // Trickle ICE: Add as soon as received
    if (webrtcManager_) {
        webrtcManager_->addRemoteCandidate(candidate);
    }
}

void StreamingPipeline::requestKeyFrame() {
    // Burst strategy: Send multiple keyframes to ensure client receives one
    std::thread([this]() {
        for (int i = 0; i < 5; i++) {
            if (encoder_) {
                logInfo("Pipeline: Requesting IDR frame (Burst " + std::to_string(i+1) + "/5)");
                encoder_->forceKeyFrame();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }).detach();
}

} // namespace ull_streamer
