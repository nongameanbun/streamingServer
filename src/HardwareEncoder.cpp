#include "HardwareEncoder.h"
#include <algorithm>

namespace ull_streamer {

HardwareEncoder::HardwareEncoder() = default;

HardwareEncoder::~HardwareEncoder() {
    shutdown();
}

bool HardwareEncoder::initialize(const StreamConfig& config, HWEncoderType preferredType) {
    if (initialized_) {
        logError("HardwareEncoder already initialized");
        return false;
    }
    
    config_ = config;
    
    // Hardware encoder priority list
    std::vector<std::pair<HWEncoderType, std::string>> encoderList;
    
    switch (preferredType) {
        case HWEncoderType::NVENC:
            encoderList = {
                {HWEncoderType::NVENC, "h264_nvenc"},
                {HWEncoderType::QSV, "h264_qsv"},
                {HWEncoderType::AMF, "h264_amf"},
                {HWEncoderType::SOFTWARE, "libx264"}
            };
            break;
        case HWEncoderType::QSV:
            encoderList = {
                {HWEncoderType::QSV, "h264_qsv"},
                {HWEncoderType::NVENC, "h264_nvenc"},
                {HWEncoderType::AMF, "h264_amf"},
                {HWEncoderType::SOFTWARE, "libx264"}
            };
            break;
        case HWEncoderType::AMF:
            encoderList = {
                {HWEncoderType::AMF, "h264_amf"},
                {HWEncoderType::NVENC, "h264_nvenc"},
                {HWEncoderType::QSV, "h264_qsv"},
                {HWEncoderType::SOFTWARE, "libx264"}
            };
            break;
        default: // Default to software if no specific HW type is preferred or recognized
            encoderList = {
                {HWEncoderType::SOFTWARE, "libx264"},
                {HWEncoderType::NVENC, "h264_nvenc"},
                {HWEncoderType::QSV, "h264_qsv"},
                {HWEncoderType::AMF, "h264_amf"}
            };
            break;
    }
    
    // Find available encoder
    for (const auto& [type, name] : encoderList) {
        logInfo("Trying encoder: " + name);
        
        codec_ = avcodec_find_encoder_by_name(name.c_str());
        if (codec_) {
            encoderType_ = type;
            encoderName_ = name;
            
            if (initializeCodecContext(name)) {
                initialized_ = true;
                logInfo("Hardware encoder initialized: " + name);
                return true;
            }
        }
    }
    
    logError("Failed to initialize any encoder");
    return false;
}

void HardwareEncoder::shutdown() {
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    
    if (packet_) {
        av_packet_free(&packet_);
    }
    
    if (hwFrame_) {
        av_frame_free(&hwFrame_);
    }
    
    if (inputFrame_) {
        av_frame_free(&inputFrame_);
    }
    
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
    }
    
    if (hwFramesCtx_) {
        av_buffer_unref(&hwFramesCtx_);
    }
    
    if (hwDeviceCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
    }
    
    initialized_ = false;
    logInfo("HardwareEncoder shutdown complete");
}

bool HardwareEncoder::initializeCodecContext(const std::string& codecName) {
    // Allocate codec context
    codecCtx_ = avcodec_alloc_context3(codec_);
    if (!codecCtx_) {
        logError("Failed to allocate codec context");
        return false;
    }
    
    // Default settings
    codecCtx_->width = config_.width;
    codecCtx_->height = config_.height;
    codecCtx_->time_base = AVRational{1, static_cast<int>(config_.targetFPS)};
    codecCtx_->framerate = AVRational{static_cast<int>(config_.targetFPS), 1};
    codecCtx_->pix_fmt = AV_PIX_FMT_NV12;  // Standard for HW encoders
    codecCtx_->bit_rate = config_.bitrate;
    codecCtx_->gop_size = config_.gopSize;
    codecCtx_->max_b_frames = 0;  // Remove B-frames for low latency
    
    // IMPORTANT: Set global header flag to populate extradata with SPS/PPS
    codecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    
    // Threading settings
    codecCtx_->thread_count = 1;  // Single thread to minimize latency
    
    // Encoder-specific low latency configuration
    if (!configureEncoder()) {
        return false;
    }
    
    // Open codec
    int ret = avcodec_open2(codecCtx_, codec_, nullptr);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_make_error_string(errBuf, AV_ERROR_MAX_STRING_SIZE, ret);
        logError("Failed to open codec: " + std::string(errBuf));
        return false;
    }
    
    // Save extra data (SPS/PPS)
    if (codecCtx_->extradata && codecCtx_->extradata_size > 0) {
        extraData_.assign(codecCtx_->extradata, 
                          codecCtx_->extradata + codecCtx_->extradata_size);
    }
    
    // Allocate input frame
    inputFrame_ = av_frame_alloc();
    if (!inputFrame_) {
        logError("Failed to allocate input frame");
        return false;
    }
    
    inputFrame_->format = AV_PIX_FMT_NV12;
    inputFrame_->width = config_.width;
    inputFrame_->height = config_.height;
    
    ret = av_frame_get_buffer(inputFrame_, 32);
    if (ret < 0) {
        logError("Failed to allocate input frame buffer");
        return false;
    }
    
    // Allocate packet
    packet_ = av_packet_alloc();
    if (!packet_) {
        logError("Failed to allocate packet");
        return false;
    }
    
    // Initialize SwsContext (BGRA -> NV12)
    if (!initializeSwsContext()) {
        return false;
    }
    
    lastStatTime_ = std::chrono::high_resolution_clock::now();
    return true;
}

bool HardwareEncoder::configureEncoder() {
    // Common low latency settings
    av_opt_set(codecCtx_->priv_data, "b_ref_mode", "disabled", 0);
    
    if (encoderType_ == HWEncoderType::NVENC) {
        // NVENC ultra-low latency settings
        av_opt_set(codecCtx_->priv_data, "preset", "p1", 0);          // Fastest preset
        av_opt_set(codecCtx_->priv_data, "tune", "ull", 0);           // Ultra Low Latency
        av_opt_set(codecCtx_->priv_data, "profile", "baseline", 0);   // Simple profile
        av_opt_set(codecCtx_->priv_data, "rc", "cbr", 0);             // CBR mode
        av_opt_set(codecCtx_->priv_data, "delay", "0", 0);            // No encoding delay
        av_opt_set(codecCtx_->priv_data, "zerolatency", "1", 0);      // Zero latency
        av_opt_set(codecCtx_->priv_data, "strict_gop", "1", 0);       // Strict GOP
        av_opt_set(codecCtx_->priv_data, "no-scenecut", "1", 0);      // Disable scenecut
        av_opt_set_int(codecCtx_->priv_data, "bf", 0, 0);             // No B-frames
        av_opt_set_int(codecCtx_->priv_data, "g", config_.gopSize, 0);
        av_opt_set_int(codecCtx_->priv_data, "rc-lookahead", 0, 0);   // Disable lookahead
        
        // Force IDR frames (VERY IMPORTANT for browser rendering)
        av_opt_set(codecCtx_->priv_data, "forced_idr", "1", 0);
        
        logInfo("NVENC encoder configured for ultra-low latency");
        
    } else if (encoderType_ == HWEncoderType::QSV) {
        // Intel QSV - balanced quality/latency settings
        av_opt_set(codecCtx_->priv_data, "preset", "medium", 0);  // Changed from veryfast for better quality
        av_opt_set(codecCtx_->priv_data, "profile", "baseline", 0); // Force baseline
        codecCtx_->profile = 578; // Constrained Baseline
        codecCtx_->level = 31; // Level 3.1
        
        // Quality settings
        av_opt_set(codecCtx_->priv_data, "global_quality", "18", 0);  // Lower = sharper (18-28 range)
        
        // Low latency settings (but not extreme)
        av_opt_set(codecCtx_->priv_data, "look_ahead", "0", 0);
        av_opt_set(codecCtx_->priv_data, "look_ahead_depth", "0", 0);
        av_opt_set(codecCtx_->priv_data, "async_depth", "1", 0);
        av_opt_set_int(codecCtx_->priv_data, "bf", 0, 0); 
        av_opt_set_int(codecCtx_->priv_data, "g", config_.gopSize, 0);
        av_opt_set_int(codecCtx_->priv_data, "idr_interval", 1, 0); // IDR on every GOP start
        
        // Match libx264 format: SPS/PPS prepended by WebRTCManager, NOT by encoder
        av_opt_set(codecCtx_->priv_data, "repeat_headers", "0", 0); 
        av_opt_set(codecCtx_->priv_data, "aud", "0", 0); // Disable AUD for WebRTC
        
        // Force IDR frames (VERY IMPORTANT for browser rendering)
        av_opt_set(codecCtx_->priv_data, "forced_idr", "1", 0);
        av_opt_set(codecCtx_->priv_data, "extbrc", "0", 0); // Use standard BRC for better IDR control
        
        logInfo("QuickSync encoder configured for ultra-low latency (Baseline)");
        
    } else if (encoderType_ == HWEncoderType::AMF) {
        // AMD AMF ultra-low latency settings
        av_opt_set(codecCtx_->priv_data, "usage", "ultralowlatency", 0);
        av_opt_set(codecCtx_->priv_data, "profile", "baseline", 0);
        av_opt_set(codecCtx_->priv_data, "rc", "cbr", 0);
        av_opt_set_int(codecCtx_->priv_data, "g", config_.gopSize, 0);
        
        // Force IDR frames (VERY IMPORTANT for browser rendering)
        // Note: AMF uses different option names sometimes, but FFmpeg amf wrapper supports 'forced_idr'
        av_opt_set(codecCtx_->priv_data, "forced_idr", "1", 0);
        
        logInfo("AMF encoder configured for ultra-low latency");
        
    } else {
        // libx264 software fallback
        av_opt_set(codecCtx_->priv_data, "preset", "ultrafast", 0);
        av_opt_set(codecCtx_->priv_data, "tune", "zerolatency", 0);
        av_opt_set(codecCtx_->priv_data, "profile", "baseline", 0);
        av_opt_set_int(codecCtx_->priv_data, "bf", 0, 0);
        
        av_opt_set(codecCtx_->priv_data, "x264-params", 
                   "bframes=0:force-cfr=1:no-mbtree=1:sync-lookahead=0:"
                   "sliced-threads=0:rc-lookahead=0", 0);
        
        logInfo("libx264 software encoder configured for ultra-low latency");
    }
    
    return true;
}

bool HardwareEncoder::initializeSwsContext() {
    // BGRA -> NV12 conversion context
    swsCtx_ = sws_getContext(
        config_.width, config_.height, AV_PIX_FMT_BGRA,
        config_.width, config_.height, AV_PIX_FMT_NV12,
        SWS_FAST_BILINEAR,
        nullptr, nullptr, nullptr
    );
    
    if (!swsCtx_) {
        logError("Failed to create SwsContext");
        return false;
    }
    
    return true;
}

bool HardwareEncoder::encodeFrame(const std::shared_ptr<CapturedFrame>& frame,
                                   std::vector<std::shared_ptr<EncodedPacket>>& packets) {
    if (!initialized_ || !frame) {
        return false;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    packets.clear();
    
    // BGRA -> NV12 conversion
    const uint8_t* srcSlice[1] = { frame->data.data() };
    int srcStride[1] = { static_cast<int>(frame->pitch) };
    
    int ret = sws_scale(
        swsCtx_,
        srcSlice, srcStride,
        0, config_.height,
        inputFrame_->data, inputFrame_->linesize
    );
    
    if (ret <= 0) {
        logError("Failed to convert frame");
        return false;
    }
    
    // Set PTS
    inputFrame_->pts = ptsCounter_++;
    
    // Use flags for keyframe
    if (forceKeyFrame_) {
        inputFrame_->pict_type = AV_PICTURE_TYPE_I;
        inputFrame_->flags |= AV_FRAME_FLAG_KEY;
        forceKeyFrame_ = false;
        logInfo("Forcing IDR frame (HardwareEncoder)");
    } else if (frameCount_ % config_.gopSize == 0) {
        inputFrame_->pict_type = AV_PICTURE_TYPE_I;
        inputFrame_->flags |= AV_FRAME_FLAG_KEY;
    } else {
        inputFrame_->pict_type = AV_PICTURE_TYPE_NONE;
        // inputFrame_->flags &= ~AV_FRAME_FLAG_KEY; // Optional: clear flag
    }
    
    // Encode
    ret = avcodec_send_frame(codecCtx_, inputFrame_);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_make_error_string(errBuf, AV_ERROR_MAX_STRING_SIZE, ret);
        logError("Failed to send frame: " + std::string(errBuf));
        return false;
    }
    
    // Receive packets
    if (!receivePackets(packets)) {
        return false;
    }
    
    // Update stats
    encodedFrameCount_++;
    
    auto endTime = std::chrono::high_resolution_clock::now();
    double encodingTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    avgEncodingTimeMs_ = avgEncodingTimeMs_ * 0.9 + encodingTimeMs * 0.1;
    
    // Propagate capture timestamp
    for (auto& pkt : packets) {
        pkt->captureTimestamp = frame->timestamp;
    }
    
    return true;
}

bool HardwareEncoder::flush(std::vector<std::shared_ptr<EncodedPacket>>& packets) {
    if (!initialized_) {
        return false;
    }
    
    packets.clear();
    
    // NULL frame signals flush
    int ret = avcodec_send_frame(codecCtx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        return false;
    }
    
    return receivePackets(packets);
}

bool HardwareEncoder::receivePackets(std::vector<std::shared_ptr<EncodedPacket>>& packets) {
    while (true) {
        int ret = avcodec_receive_packet(codecCtx_, packet_);
        
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        
        if (ret < 0) {
            char errBuf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(errBuf, AV_ERROR_MAX_STRING_SIZE, ret);
            logError("Failed to receive packet: " + std::string(errBuf));
            return false;
        }
        
        auto encodedPacket = std::make_shared<EncodedPacket>();
        encodedPacket->data.assign(packet_->data, packet_->data + packet_->size);
        encodedPacket->pts = packet_->pts;
        encodedPacket->dts = packet_->dts;
        
        // Manual keyframe detection (sometimes FFmpeg QSV doesn't set the flag correctly)
        bool isKey = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
        if (!isKey) {
            // Scan for SPS (7) or IDR (5) NAL units
            for (size_t i = 0; i + 4 < packet_->size; ++i) {
                if (packet_->data[i] == 0 && packet_->data[i+1] == 0 && 
                    (packet_->data[i+2] == 1 || (packet_->data[i+2] == 0 && packet_->data[i+3] == 1))) {
                    
                    size_t nalStart = (packet_->data[i+2] == 1) ? i + 3 : i + 4;
                    if (nalStart < (size_t)packet_->size) {
                        uint8_t nalType = packet_->data[nalStart] & 0x1F;
                        if (nalType == 7 || nalType == 5) { // SPS or IDR
                            isKey = true;
                            break;
                        }
                    }
                }
            }
        }
        encodedPacket->isKeyFrame = isKey;
        
        packets.push_back(encodedPacket);
        
        totalBytesEncoded_ += packet_->size;
        
        av_packet_unref(packet_);
    }
    
    // Bitrate calculation
    auto now = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration<double>(now - lastStatTime_).count();
    if (elapsed >= 1.0) {
        avgBitrate_ = (totalBytesEncoded_ * 8.0) / elapsed;
        totalBytesEncoded_ = 0;
        lastStatTime_ = now;
    }
    
    return true;
}

} // namespace ull_streamer
