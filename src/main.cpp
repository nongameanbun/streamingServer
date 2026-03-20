/**
 * Ultra Low Latency Screen Streamer
 * 
 * Windows DXGI Desktop Duplication + NVENC + WebRTC based
 * Ultra low latency screen streaming sender
 * 
 * Requirements:
 * - Windows 10 or later
 * - NVIDIA GPU (NVENC) or Intel (QuickSync)
 * - FFmpeg libraries
 * - libdatachannel
 */

#include "StreamingPipeline.h"
#include <iostream>
#include <csignal>

using namespace ull_streamer;

// Global pipeline pointer for signal handler
static StreamingPipeline* g_pipeline = nullptr;
static std::atomic<bool> g_running{true};

// Ctrl+C signal handler
void signalHandler(int signal) {
    g_running = false;
}

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [options]\n"
              << "\nOptions:\n"
              << "  --server <url>     Signaling server URL (default: ws://localhost:8765/ws)\n"
              << "  --room <id>        Room ID (default: default)\n"
              << "  --peer <id>        Peer ID (default: sender)\n"
              << "  --x <px>           Capture X offset (default: 0)\n"
              << "  --y <px>           Capture Y offset (default: 0)\n"
              << "  --width <px>       Capture width (default: 1366)\n"
              << "  --height <px>      Capture height (default: 768)\n"
              << "  --fps <n>          Target FPS (default: 60)\n"
              << "  --bitrate <kbps>   Bitrate in kbps (default: 8000)\n"
              << "  --gop <n>          GOP size (default: 30)\n"
              << "  --turn-url <url>   TURN server URL\n"
              << "  --turn-user <user> TURN username\n"
              << "  --turn-pass <pass> TURN password\n"
              << "  --help             Show this help\n"
              << std::endl;
}

StreamConfig parseArguments(int argc, char* argv[]) {
    StreamConfig config;
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            exit(0);
        }
        else if (arg == "--server" && i + 1 < argc) {
            config.signalingServerUrl = argv[++i];
        }
        else if (arg == "--room" && i + 1 < argc) {
            config.roomId = argv[++i];
        }
        else if (arg == "--peer" && i + 1 < argc) {
            config.peerId = argv[++i];
        }
        else if (arg == "--x" && i + 1 < argc) {
            config.captureX = std::stoi(argv[++i]);
        }
        else if (arg == "--y" && i + 1 < argc) {
            config.captureY = std::stoi(argv[++i]);
        }
        else if (arg == "--width" && i + 1 < argc) {
            config.width = std::stoi(argv[++i]);
        }
        else if (arg == "--height" && i + 1 < argc) {
            config.height = std::stoi(argv[++i]);
        }
        else if (arg == "--fps" && i + 1 < argc) {
            config.targetFPS = std::stoi(argv[++i]);
        }
        else if (arg == "--bitrate" && i + 1 < argc) {
            config.bitrate = std::stoi(argv[++i]) * 1000;  // kbps -> bps
        }
        else if (arg == "--gop" && i + 1 < argc) {
            config.gopSize = std::stoi(argv[++i]);
        }
        else if (arg == "--turn-url" && i + 1 < argc) {
            config.turnServers.push_back(argv[++i]);
        }
        else if (arg == "--turn-user" && i + 1 < argc) {
            config.turnUsername = argv[++i];
        }
        else if (arg == "--turn-pass" && i + 1 < argc) {
            config.turnPassword = argv[++i];
        }
    }
    
    return config;
}

int main(int argc, char* argv[]) {
    // Parse arguments
    StreamConfig config = parseArguments(argc, argv);
    
    // Register signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    // Initialize pipeline
    StreamingPipeline pipeline;
    g_pipeline = &pipeline;
    
    if (!pipeline.initialize(config)) {
        return 1;
    }
    
    // Main loop
    while (g_running) {
        // Check state
        auto state = pipeline.getState();
        
        if (state == PipelineState::Error) {
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Cleanup
    pipeline.shutdown();
    return 0;
}
