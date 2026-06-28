#include "ScreenCapture.h"
#include "StreamCommon.h"
#include <dxgi1_6.h>
#include <algorithm>
#include <cctype>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// PW_RENDERFULLCONTENT (Win8.1+) forces the window to render its full content,
// including GPU/Direct3D-drawn surfaces, into the target DC. Without it many
// hardware-accelerated windows capture as black. Define a fallback in case the
// SDK headers in use predate it.
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

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

    // Window capture mode: no active display output required.
    if (!config.windowTitle.empty()) {
        useWindowCapture_ = true;
        windowTitle_ = config.windowTitle;
        windowClientOnly_ = config.windowClientOnly;
        if (!initializeWindowCapture()) {
            shutdown();
            return false;
        }
        initialized_ = true;
        logInfo("ScreenCapture initialized (WINDOW mode): '" + windowTitle_ +
                "' -> " + std::to_string(width_) + "x" + std::to_string(height_));
        return true;
    }

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
    releaseGdiResources();

    desktopDuplication_.Reset();
    stagingTexture_.Reset();
    lastTexture_.Reset();
    context_.Reset();
    device_.Reset();

    useWindowCapture_ = false;
    targetWindow_ = nullptr;
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

    if (useWindowCapture_) {
        return captureFrameWindow(frame);
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

// ============================================================================
// Window capture (GDI PrintWindow) - works without an active display output
// ============================================================================

namespace {
struct FindWindowCtx {
    std::string needle;   // already lower-cased
    HWND result = nullptr;
};

BOOL CALLBACK enumWindowProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<FindWindowCtx*>(lParam);

    if (!IsWindowVisible(hwnd)) return TRUE;            // skip hidden windows
    int len = GetWindowTextLengthA(hwnd);
    if (len <= 0) return TRUE;                          // skip untitled windows

    std::string title(static_cast<size_t>(len) + 1, '\0');
    GetWindowTextA(hwnd, title.data(), len + 1);
    title.resize(static_cast<size_t>(len));

    std::transform(title.begin(), title.end(), title.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (title.find(ctx->needle) != std::string::npos) {
        ctx->result = hwnd;
        return FALSE;                                  // stop enumeration
    }
    return TRUE;
}
} // namespace

HWND ScreenCapture::findWindowByTitle(const std::string& title) {
    FindWindowCtx ctx;
    ctx.needle = title;
    std::transform(ctx.needle.begin(), ctx.needle.end(), ctx.needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    EnumWindows(enumWindowProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

bool ScreenCapture::initializeWindowCapture() {
    // Locating the window is best-effort: the target (e.g. a game client) may not
    // be up yet. captureFrameWindow() retries each tick, so don't fail hard here.
    targetWindow_ = findWindowByTitle(windowTitle_);
    if (targetWindow_) {
        logInfo("Found target window for '" + windowTitle_ + "'");
    } else {
        logInfo("Target window '" + windowTitle_ +
                "' not found yet - will retry during capture");
    }
    return true;
}

bool ScreenCapture::ensureGdiBuffers(int srcW, int srcH) {
    HDC screenDC = GetDC(nullptr);
    if (!screenDC) {
        logError("ensureGdiBuffers: GetDC(NULL) failed");
        return false;
    }

    // Destination DIB (config-sized, top-down BGRA) - created once.
    if (!dstDC_) {
        dstDC_ = CreateCompatibleDC(screenDC);
        if (!dstDC_) {
            ReleaseDC(nullptr, screenDC);
            logError("ensureGdiBuffers: CreateCompatibleDC (dst) failed");
            return false;
        }

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = static_cast<LONG>(width_);
        bi.bmiHeader.biHeight = -static_cast<LONG>(height_);   // negative => top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;                          // BGRA
        bi.bmiHeader.biCompression = BI_RGB;

        dstBmp_ = CreateDIBSection(dstDC_, &bi, DIB_RGB_COLORS, &dstBits_, nullptr, 0);
        if (!dstBmp_ || !dstBits_) {
            ReleaseDC(nullptr, screenDC);
            logError("ensureGdiBuffers: CreateDIBSection (dst) failed");
            return false;
        }
        SelectObject(dstDC_, dstBmp_);
        SetStretchBltMode(dstDC_, HALFTONE);
        SetBrushOrgEx(dstDC_, 0, 0, nullptr);
    }

    // Source bitmap (window-sized) - recreate when the window size changes.
    if (!srcDC_ || srcW != srcW_ || srcH != srcH_) {
        if (srcBmp_) {
            DeleteObject(srcBmp_);
            srcBmp_ = nullptr;
        }
        if (!srcDC_) {
            srcDC_ = CreateCompatibleDC(screenDC);
        }
        if (!srcDC_) {
            ReleaseDC(nullptr, screenDC);
            logError("ensureGdiBuffers: CreateCompatibleDC (src) failed");
            return false;
        }
        srcBmp_ = CreateCompatibleBitmap(screenDC, srcW, srcH);
        if (!srcBmp_) {
            ReleaseDC(nullptr, screenDC);
            logError("ensureGdiBuffers: CreateCompatibleBitmap (src) failed");
            return false;
        }
        SelectObject(srcDC_, srcBmp_);
        srcW_ = srcW;
        srcH_ = srcH;
    }

    ReleaseDC(nullptr, screenDC);
    return true;
}

bool ScreenCapture::captureFrameWindow(std::shared_ptr<CapturedFrame>& frame) {
    auto startTime = std::chrono::high_resolution_clock::now();

    // (Re)acquire the window handle if needed.
    if (!targetWindow_ || !IsWindow(targetWindow_)) {
        targetWindow_ = findWindowByTitle(windowTitle_);
        if (!targetWindow_) {
            return false;   // not available this tick; frame is simply skipped
        }
    }

    if (IsIconic(targetWindow_)) {
        return false;       // minimized windows don't paint; skip
    }

    // Determine source size.
    RECT rc;
    if (windowClientOnly_) {
        if (!GetClientRect(targetWindow_, &rc)) return false;
    } else {
        if (!GetWindowRect(targetWindow_, &rc)) return false;
        rc.right -= rc.left;
        rc.bottom -= rc.top;
        rc.left = 0;
        rc.top = 0;
    }
    int srcW = rc.right - rc.left;
    int srcH = rc.bottom - rc.top;
    if (srcW <= 0 || srcH <= 0) return false;

    if (!ensureGdiBuffers(srcW, srcH)) return false;

    // Render the window (including GPU-drawn content) into the source DC.
    UINT pwFlags = PW_RENDERFULLCONTENT;
    if (windowClientOnly_) pwFlags |= PW_CLIENTONLY;
    if (!PrintWindow(targetWindow_, srcDC_, pwFlags)) {
        return false;
    }

    // Scale into the encoder's fixed (config-sized) BGRA buffer.
    StretchBlt(dstDC_, 0, 0, static_cast<int>(width_), static_cast<int>(height_),
               srcDC_, 0, 0, srcW, srcH, SRCCOPY);
    GdiFlush();

    // Hand the BGRA bytes to the pipeline (same format as the DXGI path).
    frame = std::make_shared<CapturedFrame>();
    frame->timestamp = getCurrentTimestampUs();
    frame->width = width_;
    frame->height = height_;
    frame->pitch = width_ * 4;
    frame->hasGpuTexture = false;

    size_t dataSize = static_cast<size_t>(frame->pitch) * height_;
    frame->data.resize(dataSize);
    memcpy(frame->data.data(), dstBits_, dataSize);

    capturedFrameCount_++;
    auto endTime = std::chrono::high_resolution_clock::now();
    double captureTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    avgCaptureTimeMs_ = avgCaptureTimeMs_ * 0.9 + captureTimeMs * 0.1;

    return true;
}

void ScreenCapture::releaseGdiResources() {
    if (srcBmp_) { DeleteObject(srcBmp_); srcBmp_ = nullptr; }
    if (dstBmp_) { DeleteObject(dstBmp_); dstBmp_ = nullptr; }
    if (srcDC_)  { DeleteDC(srcDC_); srcDC_ = nullptr; }
    if (dstDC_)  { DeleteDC(dstDC_); dstDC_ = nullptr; }
    dstBits_ = nullptr;
    srcW_ = 0;
    srcH_ = 0;
}

} // namespace ull_streamer
