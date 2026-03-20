#include "SignalingClient.h"

namespace ull_streamer {

SignalingClient::SignalingClient() = default;

SignalingClient::~SignalingClient() {
    disconnect();
}

bool SignalingClient::connect(const std::string& serverUrl,
                               const std::string& roomId,
                               const std::string& peerId,
                               SignalingCallback* callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (connected_) {
        logError("SignalingClient already connected");
        return false;
    }
    
    serverUrl_ = serverUrl;
    roomId_ = roomId;
    peerId_ = peerId;
    callback_ = callback;
    
    try {
        // Create and connect WebSocket
        webSocket_ = std::make_unique<rtc::WebSocket>();
        
        // Connection success callback
        webSocket_->onOpen([this]() {
            connected_ = true;
            logInfo("SignalingClient: Connected to " + serverUrl_);
            
            // Auto join room
            joinRoom();
            
            if (callback_) {
                callback_->onConnected();
            }
        });
        
        // Connection close callback
        webSocket_->onClosed([this]() {
            connected_ = false;
            logInfo("SignalingClient: Disconnected");
            
            if (callback_) {
                callback_->onDisconnected();
            }
        });
        
        // Error callback
        webSocket_->onError([this](std::string error) {
            logError("SignalingClient: Error - " + error);
            
            if (callback_) {
                callback_->onError(error);
            }
        });
        
        // Message reception callback
        webSocket_->onMessage([this](std::variant<rtc::binary, rtc::string> message) {
            if (std::holds_alternative<rtc::string>(message)) {
                handleMessage(std::get<rtc::string>(message));
            }
        });
        
        // Start connection
        webSocket_->open(serverUrl);
        
        logInfo("SignalingClient: Connecting to " + serverUrl);
        return true;
        
    } catch (const std::exception& e) {
        logError("SignalingClient: Connection failed - " + std::string(e.what()));
        return false;
    }
}

void SignalingClient::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (webSocket_) {
        // Send leave message
        json leaveMsg = {
            {"type", "leave"},
            {"roomId", roomId_},
            {"peerId", peerId_}
        };
        
        try {
            webSocket_->send(leaveMsg.dump());
        } catch (...) {}
        
        webSocket_->close();
        webSocket_.reset();
    }
    
    connected_ = false;
    logInfo("SignalingClient: Disconnected");
}

bool SignalingClient::joinRoom() {
    json message = {
        {"type", "join"},
        {"roomId", roomId_},
        {"peerId", peerId_},
        {"role", "sender"}  // Sender role
    };
    
    return sendMessage(message);
}

bool SignalingClient::sendOffer(const std::string& targetPeerId, const std::string& sdp) {
    json message = {
        {"type", "offer"},
        {"roomId", roomId_},
        {"fromPeerId", peerId_},
        {"toPeerId", targetPeerId},
        {"sdp", sdp}
    };
    
    logInfo("SignalingClient: Sending offer to " + targetPeerId);
    return sendMessage(message);
}

bool SignalingClient::sendAnswer(const std::string& targetPeerId, const std::string& sdp) {
    json message = {
        {"type", "answer"},
        {"roomId", roomId_},
        {"fromPeerId", peerId_},
        {"toPeerId", targetPeerId},
        {"sdp", sdp}
    };
    
    logInfo("SignalingClient: Sending answer to " + targetPeerId);
    return sendMessage(message);
}

bool SignalingClient::sendCandidate(const std::string& targetPeerId, const IceCandidate& candidate) {
    json message = {
        {"type", "candidate"},
        {"roomId", roomId_},
        {"fromPeerId", peerId_},
        {"toPeerId", targetPeerId},
        {"candidate", {
            {"candidate", candidate.candidate},
            {"sdpMid", candidate.sdpMid},
            {"sdpMLineIndex", candidate.sdpMLineIndex}
        }}
    };
    
    logDebug("SignalingClient: Sending ICE candidate to " + targetPeerId);
    return sendMessage(message);
}

bool SignalingClient::sendMessage(const json& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!connected_ || !webSocket_) {
        logError("SignalingClient: Not connected");
        return false;
    }
    
    try {
        std::string messageStr = message.dump();
        webSocket_->send(messageStr);
        return true;
    } catch (const std::exception& e) {
        logError("SignalingClient: Failed to send message - " + std::string(e.what()));
        return false;
    }
}

void SignalingClient::handleMessage(const std::string& message) {
    try {
        json msg = json::parse(message);
        
        std::string type = msg.value("type", "");
        
        if (type == "join" || type == "peer_joined") {
            // New peer joined
            std::string peerId = msg.value("peerId", "");
            if (!peerId.empty() && peerId != peerId_) {
                logInfo("SignalingClient: Peer joined - " + peerId);
                if (callback_) {
                    callback_->onPeerJoined(peerId);
                }
            }
        }
        else if (type == "leave" || type == "peer_left") {
            // Peer left
            std::string peerId = msg.value("peerId", "");
            if (!peerId.empty()) {
                logInfo("SignalingClient: Peer left - " + peerId);
                if (callback_) {
                    callback_->onPeerLeft(peerId);
                }
            }
        }
        else if (type == "offer") {
            // Offer received
            std::string fromPeerId = msg.value("fromPeerId", "");
            std::string sdp = msg.value("sdp", "");
            
            if (!sdp.empty() && callback_) {
                logInfo("SignalingClient: Received offer from " + fromPeerId);
                callback_->onOffer(fromPeerId, sdp);
            }
        }
        else if (type == "answer") {
            // Answer received
            std::string fromPeerId = msg.value("fromPeerId", "");
            std::string sdp = msg.value("sdp", "");
            
            if (!sdp.empty() && callback_) {
                logInfo("SignalingClient: Received answer from " + fromPeerId);
                callback_->onAnswer(fromPeerId, sdp);
            }
        }
        else if (type == "candidate") {
            // ICE Candidate received
            std::string fromPeerId = msg.value("fromPeerId", "");
            
            if (msg.contains("candidate")) {
                auto& candJson = msg["candidate"];
                
                IceCandidate candidate;
                candidate.candidate = candJson.value("candidate", "");
                candidate.sdpMid = candJson.value("sdpMid", "");
                candidate.sdpMLineIndex = candJson.value("sdpMLineIndex", 0);
                
                if (!candidate.candidate.empty() && callback_) {
                    logDebug("SignalingClient: Received ICE candidate from " + fromPeerId);
                    callback_->onCandidate(fromPeerId, candidate);
                }
            }
        }
        else if (type == "ready") {
            // Peer is ready
            std::string peerId = msg.value("peerId", "");
            logInfo("SignalingClient: Peer ready - " + peerId);
            if (callback_) {
                callback_->onPeerJoined(peerId);
            }
        }
        else if (type == "error") {
            // Error
            std::string error = msg.value("message", "Unknown error");
            logError("SignalingClient: Server error - " + error);
            if (callback_) {
                callback_->onError(error);
            }
        }
        else if (type == "welcome" || type == "joined") {
            // Welcome message
            logInfo("SignalingClient: Successfully joined room " + roomId_);
        }
        else {
            logDebug("SignalingClient: Unknown message type - " + type);
        }
        
    } catch (const json::exception& e) {
        logError("SignalingClient: JSON parse error - " + std::string(e.what()));
    }
}

} // namespace ull_streamer
