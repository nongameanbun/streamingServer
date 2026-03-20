#include "WebRTCManager.h"
#include <rtc/rtc.hpp>
#include <chrono>

namespace ull_streamer {

WebRTCManager::WebRTCManager() = default;

WebRTCManager::~WebRTCManager() {
    shutdown();
}

bool WebRTCManager::initialize(const StreamConfig& config, WebRTCCallback* callback) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (initialized_) {
        logError("WebRTCManager already initialized");
        return false;
    }
    
    config_ = config;
    callback_ = callback;
    
    // RTP timestamp increment calculation (90kHz / FPS)
    timestampIncrement_ = 90000 / config_.targetFPS;
    
    try {
        // Set libdatachannel log level (Warning = less noise)
        rtc::InitLogger(rtc::LogLevel::Warning);
        
        // NOTE: PeerConnection is created on-demand in createOffer()
        // This ensures first connection and reconnection use identical code paths
        
        initialized_ = true;
        state_ = WebRTCState::Disconnected;
        
        logInfo("WebRTCManager initialized successfully");
        return true;
        
    } catch (const std::exception& e) {
        logError("WebRTCManager initialization failed: " + std::string(e.what()));
        return false;
    }
}

void WebRTCManager::shutdown() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (videoTrack_) {
        videoTrack_->close();
        videoTrack_.reset();
    }
    
    if (peerConnection_) {
        peerConnection_->close();
        peerConnection_.reset();
    }
    
    initialized_ = false;
    state_ = WebRTCState::Disconnected;
    
    logInfo("WebRTCManager shutdown complete");
}

void WebRTCManager::setupPeerConnection() {
    // ICE server configuration
    rtc::Configuration rtcConfig;
    
    for (const auto& stunServer : config_.stunServers) {
        rtcConfig.iceServers.push_back(rtc::IceServer(stunServer));
    }

    for (const auto& turnServer : config_.turnServers) {
        rtc::IceServer ice(turnServer);
        if (!config_.turnUsername.empty()) {
            ice.username = config_.turnUsername;
            ice.password = config_.turnPassword;
        }
        rtcConfig.iceServers.push_back(ice);
    }
    
    // ICE transport policy (all = relay + host candidates)
    rtcConfig.iceTransportPolicy = rtc::TransportPolicy::All;
    
    // Enable ICE-TCP (Crucial for mobile networks/restrictive NATs)
    rtcConfig.enableIceTcp = true;
    rtcConfig.portRangeBegin = 0; // Random ports
    rtcConfig.portRangeEnd = 0;
    
    // Create PeerConnection
    peerConnection_ = std::make_unique<rtc::PeerConnection>(rtcConfig);
    
    // State change callback
    peerConnection_->onStateChange([this](rtc::PeerConnection::State state) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        switch (state) {
            case rtc::PeerConnection::State::New:
            case rtc::PeerConnection::State::Connecting:
                state_ = WebRTCState::Connecting;
                break;
            case rtc::PeerConnection::State::Connected:
                state_ = WebRTCState::Connected;
                hasEverConnected_ = true;  // Mark that we've had a real connection
                logInfo("WebRTC: Peer connected");
                break;
            case rtc::PeerConnection::State::Disconnected:
            case rtc::PeerConnection::State::Closed:
                // Skip if we're intentionally recreating (don't notify pipeline)
                if (isRecreating_) {
                    logInfo("WebRTC: Ignoring disconnected state during recreation");
                    return;  // Don't update state or notify callback
                }
                state_ = WebRTCState::Disconnected;
                logInfo("WebRTC: Peer disconnected");
                break;
            case rtc::PeerConnection::State::Failed:
                state_ = WebRTCState::Failed;
                logError("WebRTC: Connection failed");
                break;
        }
        
        if (callback_) {
            callback_->onStateChange(state_);
        }
    });
    
    // ICE gathering state callback
    peerConnection_->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            logInfo("WebRTC: ICE gathering complete");
        }
    });
    
    // Local candidate callback (Trickle ICE)
    peerConnection_->onLocalCandidate([this](rtc::Candidate candidate) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        if (callback_) {
            IceCandidate ice;
            ice.candidate = std::string(candidate);
            ice.sdpMid = candidate.mid();
            
            // Send candidate as soon as generated
            callback_->onLocalCandidate(ice);
            
            logDebug("WebRTC: Local ICE candidate generated");
        }
    });
    
    // Local description callback
    peerConnection_->onLocalDescription([this](rtc::Description description) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        if (callback_) {
            std::string type = (description.type() == rtc::Description::Type::Offer) 
                               ? "offer" : "answer";
            callback_->onLocalDescription(std::string(description), type);
            
            logInfo("WebRTC: Local " + type + " created");
        }
    });
}

void WebRTCManager::setupVideoTrack() {
    // H.264 Video track configuration
    rtc::Description::Video videoDescription("video", rtc::Description::Direction::SendOnly);
    
    // H.264 Codec configuration (Constrained Baseline for maximum compatibility)
    // profile-level-id=42e01f matches Constrained Baseline Profile, Level 3.1.
    std::string fmtp = "profile-level-id=42e01f;packetization-mode=1";
    videoDescription.addH264Codec(96, fmtp);
    
    // SSRC configuration
    videoDescription.addSSRC(videoSSRC_, "video-stream");
    
    // Add track
    videoTrack_ = peerConnection_->addTrack(videoDescription);
    
    if (!videoTrack_) {
        throw std::runtime_error("Failed to create video track");
    }


    
    // RTP packetization config
    rtpConfig_ = std::make_shared<rtc::RtpPacketizationConfig>(
        videoSSRC_,
        "video",
        96,
        rtc::H264RtpPacketizer::defaultClockRate
    );
    
    // H264 RTP packetizer creation
    h264Packetizer_ = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::H264RtpPacketizer::Separator::StartSequence,
        rtpConfig_
    );
    
    // Create RTCP SR Reporter
    rtcpSrReporter_ = std::make_shared<rtc::RtcpSrReporter>(rtpConfig_);
    
    // Set up handler chain: rtcpSrReporter -> h264Packetizer
    rtcpSrReporter_->addToChain(h264Packetizer_);
    
    // Set media handler
    videoTrack_->setMediaHandler(rtcpSrReporter_);
    
    // Track state callbacks
    videoTrack_->onOpen([this]() {
        logInfo("WebRTC: Video track opened");
        sentKeyframe_ = false;
    });
    
    videoTrack_->onClosed([this]() {
        logInfo("WebRTC: Video track closed");
    });
    
    videoTrack_->onError([this](std::string error) {
        logError("WebRTC: Video track error: " + error);
        if (callback_) {
            callback_->onError(error);
        }
    });
}

bool WebRTCManager::createOffer() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (!initialized_) {
        logError("WebRTCManager not initialized");
        return false;
    }
    
    try {
        // Always recreate for proper RTP chain setup
        // Use hasEverConnected_ to log differently
        if (hasEverConnected_) {
            logInfo("WebRTC: (Re)creating PeerConnection for reconnection");
        } else {
            logInfo("WebRTC: Creating PeerConnection for first peer");
        }
        
        // Suppress state callbacks during intentional recreation
        isRecreating_ = true;
        
        // Clean up old track
        if (videoTrack_) {
            videoTrack_->close();
            videoTrack_.reset();
        }
        h264Packetizer_.reset();
        rtcpSrReporter_.reset();
        rtpConfig_.reset();
        
        // Clean up old PC
        if (peerConnection_) {
            peerConnection_->close();
            peerConnection_.reset();
        }
        
        // Recreate fresh
        setupPeerConnection();
        setupVideoTrack();
        
        // Re-enable state callbacks
        isRecreating_ = false;
        
        // Reset keyframe flag
        sentKeyframe_ = false;
        
        state_ = WebRTCState::Connecting;
        
        // Set local description to trigger offer generation
        peerConnection_->setLocalDescription(rtc::Description::Type::Offer);
        
        logInfo("WebRTC: Creating offer...");
        return true;
        
    } catch (const std::exception& e) {
        isRecreating_ = false;
        logError("Failed to create offer: " + std::string(e.what()));
        return false;
    }
}

bool WebRTCManager::setRemoteAnswer(const std::string& sdp) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (!initialized_ || !peerConnection_) {
        logError("WebRTCManager not initialized");
        return false;
    }
    
    try {
        rtc::Description answer(sdp, rtc::Description::Type::Answer);
        peerConnection_->setRemoteDescription(answer);
        
        logInfo("WebRTC: Remote answer set");
        return true;
        
    } catch (const std::exception& e) {
        logError("Failed to set remote answer: " + std::string(e.what()));
        return false;
    }
}

bool WebRTCManager::addRemoteCandidate(const IceCandidate& candidate) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (!initialized_ || !peerConnection_) {
        return false;
    }
    
    try {
        rtc::Candidate rtcCandidate(candidate.candidate, candidate.sdpMid);
        peerConnection_->addRemoteCandidate(rtcCandidate);
        
        logDebug("WebRTC: Remote ICE candidate added");
        return true;
        
    } catch (const std::exception& e) {
        logError("Failed to add remote candidate: " + std::string(e.what()));
        return false;
    }
}

void WebRTCManager::setVideoExtraData(const std::vector<uint8_t>& extraData) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (extraData.empty()) {
        logInfo("WebRTC: extradata is empty");
        return;
    }
    
    logInfo("WebRTC: Parsing extradata, size=" + std::to_string(extraData.size()) + " bytes");
    
    // Debug: Log first few bytes
    std::string hexStr;
    for (size_t i = 0; i < std::min(extraData.size(), (size_t)32); i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X ", extraData[i]);
        hexStr += buf;
    }
    logInfo("WebRTC: extradata first bytes: " + hexStr);
    
    sps_.clear();
    pps_.clear();
    
    // Check for AVCC format (starts with 0x01 for configurationVersion)
    // AVCC format: [1 byte version][3 bytes profile/level][1 byte NAL length size - 1]
    //              [1 byte num SPS][2 bytes SPS length][SPS data]
    //              [1 byte num PPS][2 bytes PPS length][PPS data]
    if (extraData.size() > 8 && extraData[0] == 0x01) {
        logInfo("WebRTC: Detected AVCC format");
        
        // AVCC format parsing
        size_t offset = 5;  // Skip version, profile, compatibility, level, nalLengthSize
        
        if (offset >= extraData.size()) return;
        
        // Number of SPS (& 0x1F to get count)
        uint8_t numSPS = extraData[offset] & 0x1F;
        offset++;
        
        for (uint8_t i = 0; i < numSPS && offset + 2 <= extraData.size(); i++) {
            uint16_t spsLen = (extraData[offset] << 8) | extraData[offset + 1];
            offset += 2;
            
            if (offset + spsLen <= extraData.size()) {
                sps_.assign(extraData.begin() + offset, extraData.begin() + offset + spsLen);
                logInfo("WebRTC: Found SPS in AVCC, size=" + std::to_string(spsLen));
                offset += spsLen;
            }
        }
        
        if (offset >= extraData.size()) {
            extraDataSet_ = !sps_.empty();
            return;
        }
        
        // Number of PPS
        uint8_t numPPS = extraData[offset];
        offset++;
        
        for (uint8_t i = 0; i < numPPS && offset + 2 <= extraData.size(); i++) {
            uint16_t ppsLen = (extraData[offset] << 8) | extraData[offset + 1];
            offset += 2;
            
            if (offset + ppsLen <= extraData.size()) {
                pps_.assign(extraData.begin() + offset, extraData.begin() + offset + ppsLen);
                logInfo("WebRTC: Found PPS in AVCC, size=" + std::to_string(ppsLen));
                offset += ppsLen;
            }
        }
    } else {
        // Try Annex-B format (start codes: 00 00 01 or 00 00 00 01)
        logInfo("WebRTC: Trying Annex-B format");
        
        size_t i = 0;
        while (i < extraData.size()) {
            // Find start code
            size_t startCodeLen = 0;
            if (i + 3 < extraData.size() && 
                extraData[i] == 0 && extraData[i+1] == 0 && extraData[i+2] == 1) {
                startCodeLen = 3;
            } else if (i + 4 < extraData.size() && 
                       extraData[i] == 0 && extraData[i+1] == 0 && 
                       extraData[i+2] == 0 && extraData[i+3] == 1) {
                startCodeLen = 4;
            } else {
                i++;
                continue;
            }
            
            size_t nalStart = i + startCodeLen;
            
            // Find next start code or end
            size_t nalEnd = extraData.size();
            for (size_t j = nalStart; j + 2 < extraData.size(); j++) {
                if (extraData[j] == 0 && extraData[j+1] == 0 && 
                    (extraData[j+2] == 1 || (j + 3 < extraData.size() && extraData[j+2] == 0 && extraData[j+3] == 1))) {
                    nalEnd = j;
                    break;
                }
            }
            
            if (nalStart < nalEnd) {
                uint8_t nalType = extraData[nalStart] & 0x1F;
                
                if (nalType == 7) {  // SPS
                    sps_.assign(extraData.begin() + nalStart, extraData.begin() + nalEnd);
                    logInfo("WebRTC: Found SPS in Annex-B, size=" + std::to_string(sps_.size()));
                } else if (nalType == 8) {  // PPS
                    pps_.assign(extraData.begin() + nalStart, extraData.begin() + nalEnd);
                    logInfo("WebRTC: Found PPS in Annex-B, size=" + std::to_string(pps_.size()));
                }
            }
            
            i = nalEnd;
        }
    }
    
    extraDataSet_ = !sps_.empty() && !pps_.empty();
    
    if (extraDataSet_) {
        logInfo("WebRTC: SPS/PPS successfully parsed (SPS: " + std::to_string(sps_.size()) + 
                " bytes, PPS: " + std::to_string(pps_.size()) + " bytes)");
    } else {
        logError("WebRTC: Failed to parse SPS/PPS from extradata");
    }
}

std::vector<std::vector<uint8_t>> WebRTCManager::packetizeH264(const std::shared_ptr<EncodedPacket>& packet) {
    std::vector<std::vector<uint8_t>> rtpPackets;
    
    const uint32_t maxPayloadSize = 1200;  // MTU safe size
    const uint8_t* data = packet->data.data();
    size_t dataSize = packet->data.size();
    
    size_t i = 0;
    while (i < dataSize) {
        // Find start code
        size_t startCodeLen = 0;
        if (i + 3 <= dataSize && data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
            startCodeLen = 3;
        } else if (i + 4 <= dataSize && data[i] == 0 && data[i+1] == 0 && 
                   data[i+2] == 0 && data[i+3] == 1) {
            startCodeLen = 4;
        } else {
            i++;
            continue;
        }
        
        size_t nalStart = i + startCodeLen;
        
        // Find next start code or end
        size_t nalEnd = dataSize;
        for (size_t j = nalStart; j + 2 < dataSize; j++) {
            if (data[j] == 0 && data[j+1] == 0 && 
                (data[j+2] == 1 || (j + 3 < dataSize && data[j+2] == 0 && data[j+3] == 1))) {
                nalEnd = j;
                break;
            }
        }
        
        size_t nalSize = nalEnd - nalStart;
        
        if (nalSize == 0) {
            i = nalEnd;
            continue;
        }
        
        uint8_t nalType = data[nalStart] & 0x1F;
        
        if (nalSize <= maxPayloadSize) {
            // Single NAL unit mode
            std::vector<uint8_t> rtpPayload(data + nalStart, data + nalEnd);
            rtpPackets.push_back(std::move(rtpPayload));
        } else {
            // Fragmentation Unit (FU-A)
            const uint8_t* nalData = data + nalStart;
            uint8_t nalHeader = nalData[0];
            uint8_t fuIndicator = (nalHeader & 0xE0) | 28;  // Type 28 = FU-A
            
            size_t offset = 1;  // Skip NAL header
            bool isStart = true;
            
            while (offset < nalSize) {
                size_t fragmentSize = std::min(static_cast<size_t>(maxPayloadSize - 2), nalSize - offset);
                bool isEnd = (offset + fragmentSize >= nalSize);
                
                uint8_t fuHeader = (nalHeader & 0x1F);  // NAL type
                if (isStart) fuHeader |= 0x80;  // Start bit
                if (isEnd) fuHeader |= 0x40;    // End bit
                
                std::vector<uint8_t> rtpPayload;
                rtpPayload.reserve(fragmentSize + 2);
                rtpPayload.push_back(fuIndicator);
                rtpPayload.push_back(fuHeader);
                rtpPayload.insert(rtpPayload.end(), 
                                 nalData + offset, 
                                 nalData + offset + fragmentSize);
                
                rtpPackets.push_back(std::move(rtpPayload));
                
                offset += fragmentSize;
                isStart = false;
            }
        }
        
        i = nalEnd;
    }
    
    return rtpPackets;
}

bool WebRTCManager::sendVideoPacket(const std::shared_ptr<EncodedPacket>& packet) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (!initialized_ || !videoTrack_ || state_ != WebRTCState::Connected) {
        return false;
    }
    
    if (!rtpConfig_ || !h264Packetizer_ || !rtcpSrReporter_) {
        return false;
    }
    
    // Request RTCP SR periodically
    rtcpSrReporter_->setNeedsToReport();
    
    // Set timestamp
    auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    rtpConfig_->timestamp = rtpConfig_->startTimestamp + 
        static_cast<uint32_t>(elapsedUs * rtpConfig_->clockRate / 1000000);
    
    try {
        // For keyframes, ensure SPS/PPS are present
        if (packet->isKeyFrame && extraDataSet_ && !sps_.empty() && !pps_.empty()) {
            // Check if the packet already starts with SPS (NAL Type 7)
            bool hasSPS = false;
            const uint8_t* p = packet->data.data();
            size_t s = packet->data.size();
            
            // Scan for first NAL (skip AUD if present)
            size_t offset = 0;
            while (offset + 4 < s) {
                if (p[offset] == 0 && p[offset+1] == 0 && 
                    (p[offset+2] == 1 || (p[offset+2] == 0 && p[offset+3] == 1))) {
                    
                    size_t nalStart = (p[offset+2] == 1) ? offset + 3 : offset + 4;
                    uint8_t nalType = p[nalStart] & 0x1F;
                    if (nalType == 7) {
                        hasSPS = true;
                        break;
                    } else if (nalType == 9) { // Skip AUD
                        offset = nalStart;
                        continue;
                    } else {
                        break;
                    }
                }
                offset++;
            }

            if (!hasSPS) {
                // Prepend SPS/PPS
                std::vector<uint8_t> completeFrame;
                completeFrame.reserve(sps_.size() + pps_.size() + packet->data.size() + 16);
                
                completeFrame.push_back(0x00);
                completeFrame.push_back(0x00);
                completeFrame.push_back(0x00);
                completeFrame.push_back(0x01);
                completeFrame.insert(completeFrame.end(), sps_.begin(), sps_.end());
                
                completeFrame.push_back(0x00);
                completeFrame.push_back(0x00);
                completeFrame.push_back(0x00);
                completeFrame.push_back(0x01);
                completeFrame.insert(completeFrame.end(), pps_.begin(), pps_.end());
                
                completeFrame.insert(completeFrame.end(), packet->data.begin(), packet->data.end());
                
                rtc::binary frameData(
                    reinterpret_cast<const std::byte*>(completeFrame.data()),
                    reinterpret_cast<const std::byte*>(completeFrame.data() + completeFrame.size())
                );
                
                videoTrack_->send(frameData);
                bytesSent_ += completeFrame.size();
                // logInfo("Sent keyframe (prepended SPS/PPS), size: " + std::to_string(completeFrame.size()));
            } else {
                // Already has SPS, send as is
                rtc::binary frameData(
                    reinterpret_cast<const std::byte*>(packet->data.data()),
                    reinterpret_cast<const std::byte*>(packet->data.data() + packet->data.size())
                );
                
                videoTrack_->send(frameData);
                bytesSent_ += packet->data.size();
                // logInfo("Sent keyframe (existing SPS/PPS), size: " + std::to_string(packet->data.size()));
            }
        } else {
            rtc::binary frameData(
                reinterpret_cast<const std::byte*>(packet->data.data()),
                reinterpret_cast<const std::byte*>(packet->data.data() + packet->data.size())
            );
            
            videoTrack_->send(frameData);
            bytesSent_ += packet->data.size();
        }
        
        packetsSent_++;
        
        if (packet->isKeyFrame) {
            sentKeyframe_ = true;
        }
        
        return true;
    } catch (const std::exception& e) {
        logError("Failed to send frame: " + std::string(e.what()));
        return false;
    }
}

double WebRTCManager::getRTT() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return 0.0;
}

} // namespace ull_streamer
