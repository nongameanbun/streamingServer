#pragma once

#ifndef SIGNALING_CLIENT_H
#define SIGNALING_CLIENT_H

#include "StreamCommon.h"
#include "WebRTCManager.h"
#include <rtc/websocket.hpp>
#include <nlohmann/json.hpp>

namespace ull_streamer {

using json = nlohmann::json;

/**
 * @brief Signaling message types
 */
enum class SignalingMessageType {
    Join,           // Join room
    Leave,          // Leave room
    Offer,          // SDP Offer
    Answer,         // SDP Answer
    Candidate,      // ICE Candidate
    Ready,          // Peer ready
    Error           // Error
};

/**
 * @brief Signaling event callback interface
 */
class SignalingCallback {
public:
    virtual ~SignalingCallback() = default;
    virtual void onConnected() = 0;
    virtual void onDisconnected() = 0;
    virtual void onPeerJoined(const std::string& peerId) = 0;
    virtual void onPeerLeft(const std::string& peerId) = 0;
    virtual void onOffer(const std::string& peerId, const std::string& sdp) = 0;
    virtual void onAnswer(const std::string& peerId, const std::string& sdp) = 0;
    virtual void onCandidate(const std::string& peerId, const IceCandidate& candidate) = 0;
    virtual void onError(const std::string& error) = 0;
};

/**
 * @brief WebSocket based signaling client
 * 
 * Communicates with FastAPI signaling server to set up WebRTC connection.
 */
class SignalingClient {
public:
    SignalingClient();
    ~SignalingClient();
    
    SignalingClient(const SignalingClient&) = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;
    
    /**
     * @brief Connect to signaling server
     * @param serverUrl WebSocket server URL
     * @param roomId Room ID
     * @param peerId Local peer ID
     * @param callback Callback interface
     * @return Success status
     */
    bool connect(const std::string& serverUrl,
                 const std::string& roomId,
                 const std::string& peerId,
                 SignalingCallback* callback);
    
    /**
     * @brief Close connection
     */
    void disconnect();
    
    /**
     * @brief Join room
     */
    bool joinRoom();
    
    /**
     * @brief Send SDP Offer
     * @param targetPeerId Target peer ID (empty for broadcast)
     * @param sdp SDP string
     */
    bool sendOffer(const std::string& targetPeerId, const std::string& sdp);
    
    /**
     * @brief Send SDP Answer
     * @param targetPeerId Target peer ID
     * @param sdp SDP string
     */
    bool sendAnswer(const std::string& targetPeerId, const std::string& sdp);
    
    /**
     * @brief Send ICE Candidate
     * @param targetPeerId Target peer ID
     * @param candidate ICE candidate
     */
    bool sendCandidate(const std::string& targetPeerId, const IceCandidate& candidate);
    
    /**
     * @brief Check connection status
     */
    bool isConnected() const { return connected_; }
    
    /**
     * @brief Current peer ID
     */
    const std::string& getPeerId() const { return peerId_; }
    
    /**
     * @brief Current room ID
     */
    const std::string& getRoomId() const { return roomId_; }

private:
    bool sendMessage(const json& message);
    void handleMessage(const std::string& message);
    
    // WebSocket
    std::unique_ptr<rtc::WebSocket> webSocket_;
    
    // Configuration
    std::string serverUrl_;
    std::string roomId_;
    std::string peerId_;
    SignalingCallback* callback_ = nullptr;
    
    // State
    std::atomic<bool> connected_{false};
    
    // Thread safety
    mutable std::mutex mutex_;
};

} // namespace ull_streamer

#endif // SIGNALING_CLIENT_H
