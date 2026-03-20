#pragma once

#ifndef HARDWARE_ENCODER_H
#define HARDWARE_ENCODER_H

#include "StreamCommon.h"

// Forward declarations for FFmpeg
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace ull_streamer {

/**
 * @brief Hardware accelerated encoder type
 */
enum class HWEncoderType {
    NVENC,      // NVIDIA NVENC
    QSV,        // Intel QuickSync
    AMF,        // AMD AMF
    SOFTWARE    // Software fallback (x264)
};

/**
 * @brief H.264 encoder based on FFmpeg with hardware acceleration
 * 
 * Supports NVENC, QuickSync, and AMF with ultra-low latency settings.
 * - Zero latency tuning
 * - No B-frames
 * - CBR rate control
 * - Minimal GOP size
 */
class HardwareEncoder {
public:
    HardwareEncoder();
    ~HardwareEncoder();
    
    HardwareEncoder(const HardwareEncoder&) = delete;
    HardwareEncoder& operator=(const HardwareEncoder&) = delete;
    
    /**
     * @brief Initialize encoder
     * @param config Streaming configuration
     * @param preferredType Preferred hardware encoder (default: NVENC)
     * @return Success status
     */
    bool initialize(const StreamConfig& config, 
                    HWEncoderType preferredType = HWEncoderType::NVENC);
    
    /**
     * @brief Resources cleanup
     */
    void shutdown();
    
    /**
     * @brief Encode frame
     * @param frame Captured frame
     * @param packets Encoded packets (output)
     * @return Success status
     */
    bool encodeFrame(const std::shared_ptr<CapturedFrame>& frame,
                     std::vector<std::shared_ptr<EncodedPacket>>& packets);
    
    /**
     * @brief Flush remaining frames in buffer
     * @param packets Encoded packets (output)
     */
    bool flush(std::vector<std::shared_ptr<EncodedPacket>>& packets);
    
    /**
     * @brief Get currently used encoder type
     */
    HWEncoderType getEncoderType() const { return encoderType_; }
    std::string getEncoderName() const { return encoderName_; }
    
    /**
     * @brief SPS/PPS data (for WebRTC initialization)
     */
    const std::vector<uint8_t>& getExtraData() const { return extraData_; }
    
    /**
     * @brief Encoding statistics
     */
    uint64_t getEncodedFrameCount() const { return encodedFrameCount_; }
    double getAverageEncodingTimeMs() const { return avgEncodingTimeMs_; }
    double getAverageBitrate() const { return avgBitrate_; }
    
    /**
     * @brief Request an immediate keyframe
     */
    void forceKeyFrame() { forceKeyFrame_ = true; }

private:
    bool initializeCodecContext(const std::string& codecName);
    bool configureEncoder();
    bool setupHardwareContext(ID3D11Device* d3dDevice = nullptr);
    bool initializeSwsContext();
    
    AVFrame* createFrame(const std::shared_ptr<CapturedFrame>& capturedFrame);
    bool receivePackets(std::vector<std::shared_ptr<EncodedPacket>>& packets);
    
    // FFmpeg contexts
    const AVCodec* codec_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    AVBufferRef* hwDeviceCtx_ = nullptr;
    AVBufferRef* hwFramesCtx_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    
    // Input/output frames
    AVFrame* inputFrame_ = nullptr;
    AVFrame* hwFrame_ = nullptr;
    AVPacket* packet_ = nullptr;
    
    // Configuration
    StreamConfig config_;
    HWEncoderType encoderType_ = HWEncoderType::SOFTWARE;
    std::string encoderName_;
    
    // Extra data (SPS/PPS)
    std::vector<uint8_t> extraData_;
    
    // State
    bool initialized_ = false;
    int64_t frameCount_ = 0;
    int64_t ptsCounter_ = 0;
    
    // Keyframe control
    std::atomic<bool> forceKeyFrame_{false};
    
    // Statistics
    std::atomic<uint64_t> encodedFrameCount_{0};
    double avgEncodingTimeMs_ = 0.0;
    double avgBitrate_ = 0.0;
    int64_t totalBytesEncoded_ = 0;
    std::chrono::high_resolution_clock::time_point lastStatTime_;
};

} // namespace ull_streamer

#endif // HARDWARE_ENCODER_H
