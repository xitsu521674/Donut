/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#include <donut/app/DeviceManager.h>
#include <donut/core/math/math.h>
#include <donut/core/log.h>
#include <nvrhi/utils.h>

#include <cstdio>
#include <iomanip>
#include <thread>
#include <sstream>

// Android specific includes
#include <android/native_window.h>
#include <android/native_activity.h>
#include <android/input.h>
#include <android/configuration.h>
#include <android/looper.h>
#include <jni.h>

#if DONUT_WITH_DX11
#include <d3d11.h>
#endif

#if DONUT_WITH_DX12
#include <d3d12.h>
#endif

#if DONUT_WITH_STREAMLINE
#include <StreamlineIntegration.h>
#endif

using namespace donut::app;

// The Android-specific class to manage joystick/gamepad inputs
class AndroidInputManager
{
public:
    static AndroidInputManager& Singleton()
    {
        static AndroidInputManager singleton;
        return singleton;
    }

    void UpdateAllInputs(const std::list<IRenderPass*>& passes);
    void ProcessInputEvent(AInputEvent* event, const std::list<IRenderPass*>& passes);
    
    void EraseDisconnectedDevices();
    void EnumerateInputDevices();

    void ConnectDevice(int id);
    void DisconnectDevice(int id);

private:
    AndroidInputManager() {}
    void UpdateGamepad(int id, const std::list<IRenderPass*>& passes);

    std::list<int> m_DeviceIDs, m_RemovedDevices;
};

static void AndroidNativeCallback_OnInputEvent(AndroidInputManager* inputManager, AInputEvent* event)
{
    // Process input events here
    DeviceManager* manager = (DeviceManager*)inputManager;
    
    int32_t eventType = AInputEvent_getType(event);
    if (eventType == AINPUT_EVENT_TYPE_MOTION) {
        float x = AMotionEvent_getX(event, 0);
        float y = AMotionEvent_getY(event, 0);
        
        int32_t action = AMotionEvent_getAction(event);
        unsigned int flags = action & AMOTION_EVENT_ACTION_MASK;
        
        if (flags == AMOTION_EVENT_ACTION_DOWN || flags == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            manager->MouseButtonUpdate(0, 1, 0); // Left button press
            manager->MousePosUpdate(x, y);
        } else if (flags == AMOTION_EVENT_ACTION_UP || flags == AMOTION_EVENT_ACTION_POINTER_UP) {
            manager->MouseButtonUpdate(0, 0, 0); // Left button release
            manager->MousePosUpdate(x, y);
        } else if (flags == AMOTION_EVENT_ACTION_MOVE) {
            manager->MousePosUpdate(x, y);
        }
    } else if (eventType == AINPUT_EVENT_TYPE_KEY) {
        int32_t keyCode = AKeyEvent_getKeyCode(event);
        int32_t action = AKeyEvent_getAction(event);
        int32_t metaState = AKeyEvent_getMetaState(event);
        
        manager->KeyboardUpdate(keyCode, 0, action == AKEY_EVENT_ACTION_DOWN ? 1 : 0, metaState);
    }
}

// Function to handle Android activity lifecycle
static void AndroidNativeCallback_OnAppCmd(struct android_app* app, int32_t cmd) {
    DeviceManager* deviceManager = (DeviceManager*)app->userData;
    
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            // Window created, create the swap chain
            if (app->window) {
                deviceManager->CreateWindowDeviceAndSwapChain(nullptr, "");
            }
            break;
        case APP_CMD_TERM_WINDOW:
            // Window destroyed
            deviceManager->Shutdown();
            break;
        case APP_CMD_GAINED_FOCUS:
            // App gained focus
            deviceManager->WindowFocusCallback(1);
            break;
        case APP_CMD_LOST_FOCUS:
            // App lost focus
            deviceManager->WindowFocusCallback(0);
            break;
        case APP_CMD_WINDOW_RESIZED:
            // Window resized
            deviceManager->UpdateWindowSize();
            break;
    }
}

static const struct
{
    nvrhi::Format format;
    uint32_t redBits;
    uint32_t greenBits;
    uint32_t blueBits;
    uint32_t alphaBits;
    uint32_t depthBits;
    uint32_t stencilBits;
} formatInfo[] = {
    { nvrhi::Format::UNKNOWN,            0,  0,  0,  0,  0,  0, },
    { nvrhi::Format::R8_UINT,            8,  0,  0,  0,  0,  0, },
    { nvrhi::Format::RG8_UINT,           8,  8,  0,  0,  0,  0, },
    { nvrhi::Format::RG8_UNORM,          8,  8,  0,  0,  0,  0, },
    { nvrhi::Format::R16_UINT,          16,  0,  0,  0,  0,  0, },
    { nvrhi::Format::R16_UNORM,         16,  0,  0,  0,  0,  0, },
    { nvrhi::Format::R16_FLOAT,         16,  0,  0,  0,  0,  0, },
    { nvrhi::Format::RGBA8_UNORM,        8,  8,  8,  8,  0,  0, },
    { nvrhi::Format::RGBA8_SNORM,        8,  8,  8,  8,  0,  0, },
    { nvrhi::Format::BGRA8_UNORM,        8,  8,  8,  8,  0,  0, },
    { nvrhi::Format::SRGBA8_UNORM,       8,  8,  8,  8,  0,  0, },
    { nvrhi::Format::SBGRA8_UNORM,       8,  8,  8,  8,  0,  0, },
    { nvrhi::Format::R10G10B10A2_UNORM, 10, 10, 10,  2,  0,  0, },
    { nvrhi::Format::R11G11B10_FLOAT,   11, 11, 10,  0,  0,  0, },
    { nvrhi::Format::RG16_UINT,         16, 16,  0,  0,  0,  0, },
    { nvrhi::Format::RG16_FLOAT,        16, 16,  0,  0,  0,  0, },
    { nvrhi::Format::R32_UINT,          32,  0,  0,  0,  0,  0, },
    { nvrhi::Format::R32_FLOAT,         32,  0,  0,  0,  0,  0, },
    { nvrhi::Format::RGBA16_FLOAT,      16, 16, 16, 16,  0,  0, },
    { nvrhi::Format::RGBA16_UNORM,      16, 16, 16, 16,  0,  0, },
    { nvrhi::Format::RGBA16_SNORM,      16, 16, 16, 16,  0,  0, },
    { nvrhi::Format::RG32_UINT,         32, 32,  0,  0,  0,  0, },
    { nvrhi::Format::RG32_FLOAT,        32, 32,  0,  0,  0,  0, },
    { nvrhi::Format::RGB32_UINT,        32, 32, 32,  0,  0,  0, },
    { nvrhi::Format::RGB32_FLOAT,       32, 32, 32,  0,  0,  0, },
    { nvrhi::Format::RGBA32_UINT,       32, 32, 32, 32,  0,  0, },
    { nvrhi::Format::RGBA32_FLOAT,      32, 32, 32, 32,  0,  0, },
};

bool DeviceManager::CreateInstance(const InstanceParameters& params)
{
    if (m_InstanceCreated)
        return true;

    static_cast<InstanceParameters&>(m_DeviceParams) = params;

#if DONUT_WITH_AFTERMATH
    if (params.enableAftermath)
    {
        m_AftermathCrashDumper.EnableCrashDumpTracking();
    }
#endif

    m_InstanceCreated = CreateInstanceInternal();
    return m_InstanceCreated;
}

bool DeviceManager::CreateHeadlessDevice(const DeviceCreationParameters& params)
{
    m_DeviceParams = params;
    m_DeviceParams.headlessDevice = true;

    if (!CreateInstance(m_DeviceParams))
        return false;

    return CreateDevice();
}

bool DeviceManager::CreateWindowDeviceAndSwapChain(const DeviceCreationParameters& params, const char *windowTitle)
{
    m_DeviceParams = params;
    m_DeviceParams.headlessDevice = false;
    m_RequestedVSync = params.vsyncEnabled;

    if (!CreateInstance(m_DeviceParams))
        return false;

    // Get the Android native window from the app structure
    ANativeWindow* nativeWindow = android_app->window;
    if (!nativeWindow) {
        log::error("No native window available");
        return false;
    }

    // Store window dimensions
    m_DeviceParams.backBufferWidth = ANativeWindow_getWidth(nativeWindow);
    m_DeviceParams.backBufferHeight = ANativeWindow_getHeight(nativeWindow);

    // Initialize the native window for rendering
    m_NativeWindow = nativeWindow;

    if (windowTitle)
        m_WindowTitle = windowTitle;

    // Initialize Android input handling
    AInputQueue* inputQueue = android_app->inputQueue;
    if (inputQueue) {
        AInputQueue_attachLooper(inputQueue, android_app->looper, 
                                LOOPER_ID_INPUT, NULL, NULL);
    }

    // Set display metrics
    AConfiguration* config = AConfiguration_new();
    AConfiguration_fromAssetManager(config, android_app->activity->assetManager);
    float density = AConfiguration_getDensity(config) / (float)ACONFIGURATION_DENSITY_MEDIUM;
    m_DPIScaleFactorX = density;
    m_DPIScaleFactorY = density;
    AConfiguration_delete(config);

    AndroidInputManager::Singleton().EnumerateInputDevices();

    if (!CreateDevice())
        return false;

    if (!CreateSwapChain())
        return false;

    m_windowVisible = true;

    // reset the back buffer size state to enforce a resize event
    m_DeviceParams.backBufferWidth = 0;
    m_DeviceParams.backBufferHeight = 0;

    UpdateWindowSize();

    return true;
}

void DeviceManager::AddRenderPassToFront(IRenderPass *pRenderPass)
{
    m_vRenderPasses.remove(pRenderPass);
    m_vRenderPasses.push_front(pRenderPass);

    pRenderPass->BackBufferResizing();
    pRenderPass->BackBufferResized(
        m_DeviceParams.backBufferWidth,
        m_DeviceParams.backBufferHeight,
        m_DeviceParams.swapChainSampleCount);
}

void DeviceManager::AddRenderPassToBack(IRenderPass *pRenderPass)
{
    m_vRenderPasses.remove(pRenderPass);
    m_vRenderPasses.push_back(pRenderPass);

    pRenderPass->BackBufferResizing();
    pRenderPass->BackBufferResized(
        m_DeviceParams.backBufferWidth,
        m_DeviceParams.backBufferHeight,
        m_DeviceParams.swapChainSampleCount);
}

void DeviceManager::RemoveRenderPass(IRenderPass *pRenderPass)
{
    m_vRenderPasses.remove(pRenderPass);
}

void DeviceManager::BackBufferResizing()
{
    m_SwapChainFramebuffers.clear();

    for (auto it : m_vRenderPasses)
    {
        it->BackBufferResizing();
    }
}

void DeviceManager::BackBufferResized()
{
    for(auto it : m_vRenderPasses)
    {
        it->BackBufferResized(m_DeviceParams.backBufferWidth,
                              m_DeviceParams.backBufferHeight,
                              m_DeviceParams.swapChainSampleCount);
    }

    uint32_t backBufferCount = GetBackBufferCount();
    m_SwapChainFramebuffers.resize(backBufferCount);
    for (uint32_t index = 0; index < backBufferCount; index++)
    {
        m_SwapChainFramebuffers[index] = GetDevice()->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(GetBackBuffer(index)));
    }
}

void DeviceManager::DisplayScaleChanged()
{
    for(auto it : m_vRenderPasses)
    {
        it->DisplayScaleChanged(m_DPIScaleFactorX, m_DPIScaleFactorY);
    }
}

void DeviceManager::Animate(double elapsedTime)
{
    for(auto it : m_vRenderPasses)
    {
        it->Animate(float(elapsedTime));
        it->SetLatewarpOptions();
    }
}

void DeviceManager::Render()
{    
    nvrhi::IFramebuffer* framebuffer = m_SwapChainFramebuffers[GetCurrentBackBufferIndex()];

    for (auto it : m_vRenderPasses)
    {
        it->Render(framebuffer);
    }
}

void DeviceManager::UpdateAverageFrameTime(double elapsedTime)
{
    m_FrameTimeSum += elapsedTime;
    m_NumberOfAccumulatedFrames += 1;
    
    if (m_FrameTimeSum > m_AverageTimeUpdateInterval && m_NumberOfAccumulatedFrames > 0)
    {
        m_AverageFrameTime = m_FrameTimeSum / double(m_NumberOfAccumulatedFrames);
        m_NumberOfAccumulatedFrames = 0;
        m_FrameTimeSum = 0.0;
    }
}

bool DeviceManager::ShouldRenderUnfocused() const
{
    for (auto it = m_vRenderPasses.crbegin(); it != m_vRenderPasses.crend(); it++)
    {
        bool ret = (*it)->ShouldRenderUnfocused();
        if (ret)
            return true;
    }

    return false;
}

void DeviceManager::RunMessageLoop()
{
    // Store current time as previous frame timestamp
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    m_PreviousFrameTimestamp = now.tv_sec + now.tv_nsec / 1000000000.0;
    
    // Android main loop
    while (android_app->destroyRequested == 0) {
        // Process Android events
        int events;
        android_poll_source* source;
        
        // Process events - timeout of 0 means non-blocking
        int timeout = m_windowVisible ? 0 : -1;  // Wait forever if not visible
        if (ALooper_pollAll(timeout, NULL, &events, (void**)&source) >= 0) {
            if (source != NULL) {
                source->process(android_app, source);
            }
        }
        
        if (m_windowVisible) {
            bool presentSuccess = AnimateRenderPresent();
            if (!presentSuccess) {
#if DONUT_WITH_AFTERMATH
                dumpingCrash = true;
#endif
                break;
            }
        }
    }

    bool waitSuccess = GetDevice()->waitForIdle();
#if DONUT_WITH_AFTERMATH
    dumpingCrash |= !waitSuccess;
    // wait for Aftermath dump to complete before exiting application
    if (dumpingCrash && m_DeviceParams.enableAftermath)
        AftermathCrashDump::WaitForCrashDump();
#endif
}

bool DeviceManager::AnimateRenderPresent()
{
    // Get current time
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double curTime = now.tv_sec + now.tv_nsec / 1000000000.0;
    
    double elapsedTime = curTime - m_PreviousFrameTimestamp;

    // Process Android-specific input
    AndroidInputManager::Singleton().EraseDisconnectedDevices();
    AndroidInputManager::Singleton().UpdateAllInputs(m_vRenderPasses);

    if (m_windowVisible && (m_windowIsInFocus || ShouldRenderUnfocused()))
    {
        if (m_PrevDPIScaleFactorX != m_DPIScaleFactorX || m_PrevDPIScaleFactorY != m_DPIScaleFactorY)
        {
            DisplayScaleChanged();
            m_PrevDPIScaleFactorX = m_DPIScaleFactorX;
            m_PrevDPIScaleFactorY = m_DPIScaleFactorY;
        }

        if (m_callbacks.beforeAnimate) m_callbacks.beforeAnimate(*this, m_FrameIndex);
        Animate(elapsedTime);
#if DONUT_WITH_STREAMLINE
        StreamlineIntegration::Get().SimEnd(*this);
#endif
        if (m_callbacks.afterAnimate) m_callbacks.afterAnimate(*this, m_FrameIndex);

        // normal rendering           : A0    R0 P0 A1 R1 P1
        // m_SkipRenderOnFirstFrame on: A0 A1 R0 P0 A2 R1 P1
        // m_SkipRenderOnFirstFrame simulates multi-threaded rendering frame indices, m_FrameIndex becomes the simulation index
        // while the local variable below becomes the render/present index, which will be different only if m_SkipRenderOnFirstFrame is set
        if (m_FrameIndex > 0 || !m_SkipRenderOnFirstFrame)
        {
            if (BeginFrame())
            {
                // first time entering this loop, m_FrameIndex is 1 for m_SkipRenderOnFirstFrame, 0 otherwise;
                uint32_t frameIndex = m_FrameIndex;

#if DONUT_WITH_STREAMLINE
                StreamlineIntegration::Get().RenderStart(*this);
#endif
                if (m_SkipRenderOnFirstFrame)
                {
                    frameIndex--;
                }

                if (m_callbacks.beforeRender) m_callbacks.beforeRender(*this, frameIndex);
                Render();
                if (m_callbacks.afterRender) m_callbacks.afterRender(*this, frameIndex);
#if DONUT_WITH_STREAMLINE
                StreamlineIntegration::Get().RenderEnd(*this);
                StreamlineIntegration::Get().PresentStart(*this);
#endif
                if (m_callbacks.beforePresent) m_callbacks.beforePresent(*this, frameIndex);
                bool presentSuccess = Present();
                if (m_callbacks.afterPresent) m_callbacks.afterPresent(*this, frameIndex);
#if DONUT_WITH_STREAMLINE
                StreamlineIntegration::Get().PresentEnd(*this);
#endif
                if (!presentSuccess)
                {
                    return false;
                }
            }
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(0));

    GetDevice()->runGarbageCollection();

    UpdateAverageFrameTime(elapsedTime);
    m_PreviousFrameTimestamp = curTime;

    ++m_FrameIndex;
    return true;
}

void DeviceManager::GetWindowDimensions(int& width, int& height)
{
    width = m_DeviceParams.backBufferWidth;
    height = m_DeviceParams.backBufferHeight;
}

const DeviceCreationParameters& DeviceManager::GetDeviceParams()
{
    return m_DeviceParams;
}

donut::app::DeviceManager::DeviceManager()
#if DONUT_WITH_AFTERMATH
    : m_AftermathCrashDumper(*this)
#endif
{
}

void DeviceManager::UpdateWindowSize()
{
    if (!m_NativeWindow) {
        m_windowVisible = false;
        return;
    }

    int width = ANativeWindow_getWidth(m_NativeWindow);
    int height = ANativeWindow_getHeight(m_NativeWindow);

    if (width == 0 || height == 0)
    {
        // window is minimized
        m_windowVisible = false;
        return;
    }

    m_windowVisible = true;
    m_windowIsInFocus = true; // Android doesn't have the same focus concept, assume always in focus when visible

    if (int(m_DeviceParams.backBufferWidth) != width || 
        int(m_DeviceParams.backBufferHeight) != height ||
        (m_DeviceParams.vsyncEnabled != m_RequestedVSync && GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN))
    {
        // window is not minimized, and the size has changed
        BackBufferResizing();

        m_DeviceParams.backBufferWidth = width;
        m_DeviceParams.backBufferHeight = height;
        m_DeviceParams.vsyncEnabled = m_RequestedVSync;

        ResizeSwapChain();
        BackBufferResized();
    }

    m_DeviceParams.vsyncEnabled = m_RequestedVSync;
}

void DeviceManager::WindowPosCallback(int x, int y)
{
    // Android doesn't use window position in the same way, but we'll keep the function for compatibility
    
    // Update DPI info when position changes
    AConfiguration* config = AConfiguration_new();
    AConfiguration_fromAssetManager(config, android_app->activity->assetManager);
    float density = AConfiguration_getDensity(config) / (float)ACONFIGURATION_DENSITY_MEDIUM;
    m_DPIScaleFactorX = density;
    m_DPIScaleFactorY = density;
    AConfiguration_delete(config);
    
    if (m_EnableRenderDuringWindowMovement && m_SwapChainFramebuffers.size() > 0)
    {
        if (m_callbacks.beforeFrame) m_callbacks.beforeFrame(*this, m_FrameIndex);
        AnimateRenderPresent();
    }
}

void DeviceManager::KeyboardUpdate(int key, int scancode, int action, int mods)
{
    for (auto it = m_vRenderPasses.crbegin(); it != m_vRenderPasses.crend(); it++)
    {
        bool ret = (*it)->KeyboardUpdate(key, scancode, action, mods);
        if (ret)
            break;
    }
}

void DeviceManager::KeyboardCharInput(unsigned int unicode, int mods)
{
    for (auto it = m_vRenderPasses.crbegin(); it != m_vRenderPasses.crend(); it++)
    {
        bool ret = (*it)->KeyboardCharInput(unicode, mods);
        if (ret)
            break;
    }
}

void DeviceManager::MousePosUpdate(double xpos, double ypos)
{
    if (!m_DeviceParams.supportExplicitDisplayScaling)
    {
        xpos /= m_DPIScaleFactorX;
        ypos /= m_DPIScaleFactorY;
    }
    
    for (auto it = m_vRenderPasses.crbegin(); it != m_vRenderPasses.crend(); it++)
    {
        bool ret = (*it)->MousePosUpdate(xpos, ypos);
        if (ret)
            break;
    }
}

void DeviceManager::MouseButtonUpdate(int button, int action, int mods)
{
    for (auto it = m_vRenderPasses.crbegin(); it != m_vRenderPasses.crend(); it++)
    {
        bool ret = (*it)->MouseButtonUpdate(button, action, mods);
        if (ret)
            break;
    }
}

void DeviceManager::MouseScrollUpdate(double xoffset, double yoffset)
{
    for (auto it = m_vRenderPasses.crbegin(); it != m_vRenderPasses.crend(); it++)
    {
        bool ret = (*it)->MouseScrollUpdate(xoffset, yoffset);
        if (ret)
            break;
    }
}

void AndroidInputManager::EnumerateInputDevices()
{
    // Check for available input devices using Android specific methods
    // This would typically query the Android InputManager to find connected devices
}

void AndroidInputManager::EraseDisconnectedDevices()
{
    while (!m_RemovedDevices.empty())
    {
        auto id = m_RemovedDevices.back();
        m_RemovedDevices.pop_back();

        auto it = std::find(m_DeviceIDs.begin(), m_DeviceIDs.end(), id);
        if (it != m_DeviceIDs.end())
            m_DeviceIDs.erase(it);
    }
}

void AndroidInputManager::ConnectDevice(int id)
{
    m_DeviceIDs.push_back(id);
}

void AndroidInputManager::DisconnectDevice(int id)
{
    m_RemovedDevices.push_back(id);
}

void AndroidInputManager::UpdateAllInputs(const std::list<IRenderPass*>& passes)
{
    for (auto j = m_DeviceIDs.begin(); j != m_DeviceIDs.end(); ++j)
        UpdateGamepad(*j, passes);
}

static void ApplyDeadZone(dm::float2& v, const float deadZone = 0.1f)
{
    v *= std::max(dm::length(v) - deadZone, 0.f) / (1.f - deadZone);
}

void AndroidInputManager::UpdateGamepad(int j, const std::list<IRenderPass*>& passes)
{
    // This would typically use Android's InputDevice API to get gamepad states
    
    // Example of how this might work with Android:
    /*
    // Get device state
    // Apply dead zones to analog sticks
    dm::float2 leftStick(leftX, leftY);
    ApplyDeadZone(leftStick);
    
    // Process the input for each pass
    for (auto it = passes.crbegin(); it != passes.crend(); it++)
    {
        bool ret = (*it)->JoystickAxisUpdate(AXIS_LEFT_X, leftStick.x);
        if (ret) break;
    }
    */
}

void AndroidInputManager::ProcessInputEvent(AInputEvent* event, const std::list<IRenderPass*>& passes)
{
    // Process Android input events and map them to the appropriate callbacks
}

void DeviceManager::Shutdown()
{
#if DONUT_WITH_STREAMLINE
    // Shut down Streamline before destroying swapchain and device.
    StreamlineIntegration::Get().Shutdown();
#endif

    m_SwapChainFramebuffers.clear();

    DestroyDeviceAndSwapChain();

    if (m_NativeWindow)
    {
        ANativeWindow_release(m_NativeWindow);
        m_NativeWindow = nullptr;
    }

    m_InstanceCreated = false;
}

nvrhi::IFramebuffer* donut::app::DeviceManager::GetCurrentFramebuffer()
{
    return GetFramebuffer(GetCurrentBackBufferIndex());
}

nvrhi::IFramebuffer* donut::app::DeviceManager::GetFramebuffer(uint32_t index)
{
    if (index < m_SwapChainFramebuffers.size())
        return m_SwapChainFramebuffers[index];

    return nullptr;
}

void DeviceManager::SetWindowTitle(const char* title)
{
    assert(title);
    if (m_WindowTitle == title)
        return;

    // Android doesn't have window titles in the same way desktop apps do
    // Could potentially send a toast notification or update status bar

    m_WindowTitle = title;
}

void DeviceManager::SetInformativeWindowTitle(const char* applicationName, bool includeFramerate, const char* extraInfo)
{
    std::stringstream ss;
    ss << applicationName;
    ss << " (" << nvrhi::utils::GraphicsAPIToString(GetDevice()->getGraphicsAPI());

    if (m_DeviceParams.enableDebugRuntime)
    {
        if (GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
            ss << ", VulkanValidationLayer";
        else
            ss << ", DebugRuntime";
    }

    if (m_DeviceParams.enableNvrhiValidationLayer)
    {
        ss << ", NvrhiValidationLayer";
    }

    ss << ")";

    double frameTime = GetAverageFrameTimeSeconds();
    if (includeFramerate && frameTime > 0)
    {
        double const fps = 1.0 / frameTime;
        int const precision = (fps <= 20.0) ? 1 : 0;
        ```cpp
        ss << " - " << std::fixed << std::setprecision(precision) << fps << " FPS ";
    }

    if (extraInfo)
        ss << extraInfo;

    SetWindowTitle(ss.str().c_str());
}

const char* donut::app::DeviceManager::GetWindowTitle()
{
    return m_WindowTitle.c_str();
}

donut::app::DeviceManager* donut::app::DeviceManager::Create(nvrhi::GraphicsAPI api)
{
    switch (api)
    {
#if DONUT_WITH_DX11
    case nvrhi::GraphicsAPI::D3D11:
        return CreateD3D11();
#endif
#if DONUT_WITH_DX12
    case nvrhi::GraphicsAPI::D3D12:
        return CreateD3D12();
#endif
#if DONUT_WITH_VULKAN
    case nvrhi::GraphicsAPI::VULKAN:
        return CreateVK();
#endif
    default:
        log::error("DeviceManager::Create: Unsupported Graphics API (%d)", api);
        return nullptr;
    }
}

DefaultMessageCallback& DefaultMessageCallback::GetInstance()
{
    static DefaultMessageCallback Instance;
    return Instance;
}

void DefaultMessageCallback::message(nvrhi::MessageSeverity severity, const char* messageText)
{
    donut::log::Severity donutSeverity = donut::log::Severity::Info;
    switch (severity)
    {
    case nvrhi::MessageSeverity::Info:
        donutSeverity = donut::log::Severity::Info;
        break;
    case nvrhi::MessageSeverity::Warning:
        donutSeverity = donut::log::Severity::Warning;
        break;
    case nvrhi::MessageSeverity::Error:
        donutSeverity = donut::log::Severity::Error;
        break;
    case nvrhi::MessageSeverity::Fatal:
        donutSeverity = donut::log::Severity::Fatal;
        break;
    }
    
    donut::log::message(donutSeverity, "%s", messageText);
}

#if DONUT_WITH_STREAMLINE
StreamlineInterface& DeviceManager::GetStreamline()
{
    // StreamlineIntegration doesn't support instances
    return StreamlineIntegration::Get();
}
#endif