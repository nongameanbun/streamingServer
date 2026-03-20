#include "ScreenCapture.h"
#include "StreamCommon.h"
#include <dxgi1_6.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace ull_streamer {

ScreenCapture::ScreenCapture() = default;

ScreenCapture::~ScreenCapture() {
    shutdown();
}

bool ScreenCapture::initialize(const StreamConfig& config) {
    if (initialized_) {
        logError("ScreenCapture already initialized");
        return false;
    }
    
    config_ = config;
    width_ = config.width;
    height_ = config.height;
    
    try {
        if (!initializeD3D11()) {
            return false;
        }
        
        if (!initializeDesktopDuplication()) {
            return false;
        }
        
        if (!createStagingTexture()) {
            return false;
        }
        
        initialized_ = true;
        logInfo("ScreenCapture initialized successfully: " + 
                std::to_string(width_) + "x" + std::to_string(height_));
        return true;
        
    } catch (const std::exception& e) {
        logError("ScreenCapture initialization failed: " + std::string(e.what()));
        shutdown();
        return false;
    }
}

void ScreenCapture::shutdown() {
    releaseFrame();
    
    desktopDuplication_.Reset();
    stagingTexture_.Reset();
    lastTexture_.Reset();
    context_.Reset();
    device_.Reset();
    
    initialized_ = false;
    logInfo("ScreenCapture shutdown complete");
}

bool ScreenCapture::initializeD3D11() {
    // Create D3D11 device - prefer GPU acceleration
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    
    D3D_FEATURE_LEVEL featureLevel;
    
    // Create hardware device
    HRESULT hr = D3D11CreateDevice(
        nullptr,                    // Default adapter
        D3D_DRIVER_TYPE_HARDWARE,   // Hardware acceleration
        nullptr,                    // No software rasterizer
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, // Required for Desktop Duplication
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &device_,
        &featureLevel,
        &context_
    );
    
    HR_CHECK(hr, "Failed to create D3D11 device");
    
    logInfo("D3D11 Device created with feature level: " + 
            std::to_string(featureLevel));
    
    return true;
}

bool ScreenCapture::initializeDesktopDuplication() {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    HR_CHECK(hr, "Failed to create DXGI factory");

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 adapterDesc;
        adapter->GetDesc1(&adapterDesc);
        
        // Log adapter info (wide string to string conversion)
        std::wstring ws(adapterDesc.Description);
        std::string adapterName(ws.begin(), ws.end());
        logInfo("Checking adapter " + std::to_string(i) + ": " + adapterName);

        ComPtr<IDXGIOutput> output;
        for (UINT j = 0; adapter->EnumOutputs(j, &output) != DXGI_ERROR_NOT_FOUND; ++j) {
            DXGI_OUTPUT_DESC outputDesc;
            output->GetDesc(&outputDesc);
            
            logInfo("  Output " + std::to_string(j) + ": " + 
                    std::to_string(outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left) + "x" +
                    std::to_string(outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top));

            // Try to duplicate this output
            ComPtr<IDXGIOutput1> output1;
            hr = output.As(&output1);
            
            if (SUCCEEDED(hr)) {
                hr = output1->DuplicateOutput(device_.Get(), &desktopDuplication_);
                if (SUCCEEDED(hr)) {
                    logInfo("Successfully initialized Desktop Duplication on Adapter " + std::to_string(i) + ", Output " + std::to_string(j));
                    
                    // Update dimensions
                    uint32_t monW = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
                    uint32_t monH = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
                    
                    // If config is 0 or different, update? (Optional, keeping config fixed for now)
                    
                    return true;
                } else {
                    logError("Failed to duplicate output: " + std::to_string(hr));
                }
            }
        }
    }
    
    throw std::runtime_error("No valid display output found for Desktop Duplication");
}

bool ScreenCapture::createStagingTexture() {
    // Create staging texture for CPU reading
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = width_;
    stagingDesc.Height = height_;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    
    HRESULT hr = device_->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture_);
    HR_CHECK(hr, "Failed to create staging texture");
    
    return true;
}

void ScreenCapture::releaseFrame() {
    if (frameAcquired_ && desktopDuplication_) {
        desktopDuplication_->ReleaseFrame();
        frameAcquired_ = false;
    }
}

bool ScreenCapture::captureFrame(std::shared_ptr<CapturedFrame>& frame, uint32_t timeoutMs) {
    if (!initialized_) {
        logError("ScreenCapture not initialized");
        return false;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Release previous frame
    releaseFrame();
    
    // Acquire new frame
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    ComPtr<IDXGIResource> desktopResource;
    
    HRESULT hr = desktopDuplication_->AcquireNextFrame(
        timeoutMs,
        &frameInfo,
        &desktopResource
    );
    
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;
    }
    
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        logError("Desktop Duplication access lost, reinitializing...");
        shutdown();
        if (!initialize(config_)) {
            return false;
        }
        return captureFrame(frame, timeoutMs);
    }
    
    HR_CHECK(hr, "Failed to acquire next frame");
    
    frameAcquired_ = true;
    
    // Cast to Texture2D
    ComPtr<ID3D11Texture2D> desktopTexture;
    hr = desktopResource.As(&desktopTexture);
    HR_CHECK(hr, "Failed to get desktop texture");
    
    lastTexture_ = desktopTexture;
    
    frame = std::make_shared<CapturedFrame>();
    frame->timestamp = getCurrentTimestampUs();
    frame->width = width_;
    frame->height = height_;
    frame->gpuTexture = desktopTexture;
    frame->hasGpuTexture = true;
    
    D3D11_TEXTURE2D_DESC srcDesc;
    desktopTexture->GetDesc(&srcDesc);
    
    // Calculate capture region
    uint32_t capX = config_.captureX;
    uint32_t capY = config_.captureY;
    uint32_t capW = std::min(width_, srcDesc.Width - capX);
    uint32_t capH = std::min(height_, srcDesc.Height - capY);
    
    D3D11_BOX srcBox = {};
    srcBox.left = capX;
    srcBox.top = capY;
    srcBox.front = 0;
    srcBox.right = capX + capW;
    srcBox.bottom = capY + capH;
    srcBox.back = 1;
    
    context_->CopySubresourceRegion(
        stagingTexture_.Get(), 0, 0, 0, 0,
        desktopTexture.Get(), 0, &srcBox
    );
    
    // Read data from CPU
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    hr = context_->Map(stagingTexture_.Get(), 0, D3D11_MAP_READ, 0, &mappedResource);
    HR_CHECK(hr, "Failed to map staging texture");
    
    frame->pitch = mappedResource.RowPitch;
    size_t dataSize = frame->pitch * height_;
    frame->data.resize(dataSize);
    
    const uint8_t* srcData = static_cast<const uint8_t*>(mappedResource.pData);
    memcpy(frame->data.data(), srcData, dataSize);
    
    context_->Unmap(stagingTexture_.Get(), 0);
    
    capturedFrameCount_++;
    auto endTime = std::chrono::high_resolution_clock::now();
    double captureTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    avgCaptureTimeMs_ = avgCaptureTimeMs_ * 0.9 + captureTimeMs * 0.1;
    
    return true;
}

} // namespace ull_streamer
