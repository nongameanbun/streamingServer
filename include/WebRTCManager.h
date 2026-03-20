#pragma once

#ifndef WEBRTC_MANAGER_H
#define WEBRTC_MANAGER_H

#include "StreamCommon.h"
#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

namespace ull_streamer {

using json = nlohmann::json;

/**
 * @brief ICE candidate information
 */
struct IceCandidate {
    std::string candidate;
    std::string sdpMid;
    int sdpMLineIndex = 0;
};

/**
 * @brief WebRTC connection state
 */
enum class WebRTCState {
    Disconnected,
    Connecting,
    Connected,
    Failed
};

/**
 * @brief WebRTC callback interface
 */
class WebRTCCallback {
public:
    virtual ~WebRTCCallback() = default;
    virtual void onLocalDescription(const std::string& sdp, const std::string& type) = 0;
    virtual void onLocalCandidate(const IceCandidate& candidate) = 0;
    virtual void onStateChange(WebRTCState state) = 0;
    virtual void onError(const std::string& error) = 0;
    virtual void onKeyFrameRequest() = 0;
};

/**
 * @brief WebRTC manager based on libdatachannel
 */
class WebRTCManager {
public:
    WebRTCManager();
    ~WebRTCManager();
    
    WebRTCManager(const WebRTCManager&) = delete;
    WebRTCManager& operator=(const WebRTCManager&) = delete;
    
    /**
     * @brief Initialize WebRTC
     * @param config Streaming configuration
     * @param callback Callback interface
     * @return Success status
     */
    bool initialize(const StreamConfig& config, WebRTCCallback* callback);
    
    /**
     * @brief Resources cleanup
     */
    void shutdown();
    
    /**
     * @brief Create Offer and setup PeerConnection
     * @return Success status
     */
    bool createOffer();
    
    /**
     * @brief Set remote Answer SDP
     * @param sdp SDP string
     * @return Success status
     */
    bool setRemoteAnswer(const std::string& sdp);
    
    /**
     * @brief Add remote ICE candidate
     * @param candidate ICE candidate information
     * @return Success status
     */
    bool addRemoteCandidate(const IceCandidate& candidate);
    
    /**
     * @brief Send H.264 packet
     * @param packet Encoded packet
     * @return Success status
     */
    bool sendVideoPacket(const std::shared_ptr<EncodedPacket>& packet);
    
    /**
     * @brief Set SPS/PPS (required before first transmission)
     * @param extraData SPS/PPS data from FFmpeg
     */
    void setVideoExtraData(const std::vector<uint8_t>& extraData);
    
    /**
     * @brief Current connection state
     */
    WebRTCState getState() const { return state_; }
    bool isConnected() const { return state_ == WebRTCState::Connected; }
    
    /**
     * @brief Transmission statistics
     */
    uint64_t getBytesSent() const { return bytesSent_; }
    uint64_t getPacketsSent() const { return packetsSent_; }
    double getRTT() const;

private:
    void setupPeerConnection();
    void setupVideoTrack();
    std::vector<std::vector<uint8_t>> packetizeH264(const std::shared_ptr<EncodedPacket>& packet);
    
    // Configuration
    StreamConfig config_;
    WebRTCCallback* callback_ = nullptr;
    
    // libdatachannel objects
    std::unique_ptr<rtc::PeerConnection> peerConnection_;
    std::shared_ptr<rtc::Track> videoTrack_;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig_;
    std::shared_ptr<rtc::H264RtpPacketizer> h264Packetizer_;
    std::shared_ptr<rtc::RtcpSrReporter> rtcpSrReporter_;
    
    // RTP configuration
    uint32_t videoSSRC_ = 1;
    uint16_t sequenceNumber_ = 0;
    uint32_t rtpTimestamp_ = 0;
    uint32_t timestampIncrement_ = 0;
    
    // Video extra data (SPS/PPS)
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
    bool extraDataSet_ = false;
    bool sentKeyframe_ = false;
    
    // State
    WebRTCState state_ = WebRTCState::Disconnected;
    bool initialized_ = false;
    bool hasEverConnected_ = false;  // Track if we've ever had a connection
    bool isRecreating_ = false;      // Suppress callbacks during recreation
    
    // Statistics
    std::atomic<uint64_t> bytesSent_{0};
    std::atomic<uint64_t> packetsSent_{0};
    
    // Thread safety
    mutable std::recursive_mutex mutex_;
};

} // namespace ull_streamer

#endif // WEBRTC_MANAGER_H
