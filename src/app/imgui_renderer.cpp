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

/*
License for Dear ImGui

Copyright (c) 2014-2019 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <donut/app/imgui_renderer.h>
#include <donut/core/vfs/VFS.h>

using namespace donut::vfs;
using namespace donut::engine;
using namespace donut::app;

ImGui_Renderer::ImGui_Renderer(DeviceManager *devManager)
    : IRenderPass(devManager)
    , m_supportExplicitDisplayScaling(devManager->GetDeviceParams().supportExplicitDisplayScaling)
{
    ImGui::CreateContext();

    m_defaultFont = std::make_shared<RegisteredFont>(13.f);
    m_fonts.push_back(m_defaultFont);
}

ImGui_Renderer::~ImGui_Renderer()
{
    ImGui::DestroyContext();
}

bool ImGui_Renderer::Init(std::shared_ptr<ShaderFactory> shaderFactory)
{
    // Set up keyboard mapping.
    // ImGui will use those indices to peek into the io.KeyDown[] array
    // that we will update during the application lifetime.
    ImGuiIO& io = ImGui::GetIO();
    io.KeyMap[ImGuiKey_Tab] = GLFW_KEY_TAB;
    io.KeyMap[ImGuiKey_LeftArrow] = GLFW_KEY_LEFT;
    io.KeyMap[ImGuiKey_RightArrow] = GLFW_KEY_RIGHT;
    io.KeyMap[ImGuiKey_UpArrow] = GLFW_KEY_UP;
    io.KeyMap[ImGuiKey_DownArrow] = GLFW_KEY_DOWN;
    io.KeyMap[ImGuiKey_PageUp] = GLFW_KEY_PAGE_UP;
    io.KeyMap[ImGuiKey_PageDown] = GLFW_KEY_PAGE_DOWN;
    io.KeyMap[ImGuiKey_Home] = GLFW_KEY_HOME;
    io.KeyMap[ImGuiKey_End] = GLFW_KEY_END;
    io.KeyMap[ImGuiKey_Delete] = GLFW_KEY_DELETE;
    io.KeyMap[ImGuiKey_Backspace] = GLFW_KEY_BACKSPACE;
    io.KeyMap[ImGuiKey_Enter] = GLFW_KEY_ENTER;
    io.KeyMap[ImGuiKey_Escape] = GLFW_KEY_ESCAPE;
    io.KeyMap[ImGuiKey_A] = 'A';
    io.KeyMap[ImGuiKey_C] = 'C';
    io.KeyMap[ImGuiKey_V] = 'V';
    io.KeyMap[ImGuiKey_X] = 'X';
    io.KeyMap[ImGuiKey_Y] = 'Y';
    io.KeyMap[ImGuiKey_Z] = 'Z';

    imgui_nvrhi = std::make_unique<ImGui_NVRHI>();
    return imgui_nvrhi->init(GetDevice(), shaderFactory);
}

std::shared_ptr<RegisteredFont> ImGui_Renderer::CreateFontFromFile(IFileSystem& fs,
    const std::filesystem::path& fontFile, float fontSize)
{
	auto fontData = fs.readFile(fontFile);
	if (!fontData)
		return std::make_shared<RegisteredFont>();

	auto font = std::make_shared<RegisteredFont>(fontData, false, fontSize);
    m_fonts.push_back(font);

    return std::move(font);
}

std::shared_ptr<RegisteredFont> ImGui_Renderer::CreateFontFromMemoryInternal(void const* pData, size_t size,
    bool compressed, float fontSize)
{
    if (!pData || !size)
		return std::make_shared<RegisteredFont>();

    // Copy the font data into a blob to make the RegisteredFont object own it
    void* dataCopy = malloc(size);
    memcpy(dataCopy, pData, size);
    std::shared_ptr<vfs::Blob> blob = std::make_shared<vfs::Blob>(dataCopy, size);
    
    auto font = std::make_shared<RegisteredFont>(blob, compressed, fontSize);
    m_fonts.push_back(font);

    return std::move(font);
}

std::shared_ptr<RegisteredFont> ImGui_Renderer::CreateFontFromMemory(void const* pData, size_t size, float fontSize)
{
    return CreateFontFromMemoryInternal(pData, size, false, fontSize);
}

std::shared_ptr<RegisteredFont> ImGui_Renderer::CreateFontFromMemoryCompressed(void const* pData, size_t size,
    float fontSize)
{
    return CreateFontFromMemoryInternal(pData, size, true, fontSize);
}

bool ImGui_Renderer::KeyboardUpdate(int key, int scancode, int action, int mods)
{
    auto& io = ImGui::GetIO();

    bool keyIsDown;
    if (action == GLFW_PRESS || action == GLFW_REPEAT)
    {
        keyIsDown = true;
    } else {
        keyIsDown = false;
    }

    // update our internal state tracking for this key button
    keyDown[key] = keyIsDown;

    if (keyIsDown)
    {
        // if the key was pressed, update ImGui immediately
        io.KeysDown[key] = true;
    } else {
        // for key up events, ImGui state is only updated after the next frame
        // this ensures that short keypresses are not missed
    }

    return io.WantCaptureKeyboard;
}

bool ImGui_Renderer::KeyboardCharInput(unsigned int unicode, int mods)
{
    auto& io = ImGui::GetIO();

    io.AddInputCharacter(unicode);

    return io.WantCaptureKeyboard;
}

bool ImGui_Renderer::MousePosUpdate(double xpos, double ypos)
{
    auto& io = ImGui::GetIO();
    io.MousePos.x = float(xpos);
    io.MousePos.y = float(ypos);

    return io.WantCaptureMouse;
}

bool ImGui_Renderer::MouseScrollUpdate(double xoffset, double yoffset)
{
    auto& io = ImGui::GetIO();
    io.MouseWheel += float(yoffset);

    return io.WantCaptureMouse;
}

bool ImGui_Renderer::MouseButtonUpdate(int button, int action, int mods)
{
    auto& io = ImGui::GetIO();

    bool buttonIsDown;
    int buttonIndex;

    if (action == GLFW_PRESS || action == GLFW_REPEAT)
    {
        buttonIsDown = true;
    } else {
        buttonIsDown = false;
    }

    switch(button)
    {
        case GLFW_MOUSE_BUTTON_LEFT:
            buttonIndex = 0;
            break;

        case GLFW_MOUSE_BUTTON_RIGHT:
            buttonIndex = 1;
            break;

        case GLFW_MOUSE_BUTTON_MIDDLE:
            buttonIndex = 2;
            break;
    }

    // update our internal state tracking for this mouse button
    mouseDown[buttonIndex] = buttonIsDown;

    if (buttonIsDown)
    {
        // update ImGui state immediately
        io.MouseDown[buttonIndex] = true;
    } else {
        // for mouse up events, ImGui state is only updated after the next frame
        // this ensures that short clicks are not missed
    }

    return io.WantCaptureMouse;
}

void ImGui_Renderer::Animate(float elapsedTimeSeconds)
{
    // multiple Animate may be called before the first Render due to the m_SkipRenderOnFirstFrame extension
    // ensure each imgui_nvrhi->beginFrame matches with exactly one imgui_nvrhi->Render
    if (!imgui_nvrhi || m_beginFrameCalled)
        return;

    // Make sure that all registered fonts have corresponding ImFont objects at the current DPI scale
    float scaleX, scaleY;
    GetDeviceManager()->GetDPIScaleInfo(scaleX, scaleY);
    for (auto& font : m_fonts)
    {
        if (!font->GetScaledFont())
            font->CreateScaledFont(m_supportExplicitDisplayScaling ? scaleX : 1.f);
    }

    // Creates the font texture if it's not yet valid
    imgui_nvrhi->updateFontTexture();
    
    int w, h;
    GetDeviceManager()->GetWindowDimensions(w, h);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(w), float(h));
    if (!m_supportExplicitDisplayScaling)
    {
        io.DisplayFramebufferScale.x = scaleX;
        io.DisplayFramebufferScale.y = scaleY;
    }

    io.KeyCtrl = io.KeysDown[GLFW_KEY_LEFT_CONTROL] || io.KeysDown[GLFW_KEY_RIGHT_CONTROL];
    io.KeyShift = io.KeysDown[GLFW_KEY_LEFT_SHIFT] || io.KeysDown[GLFW_KEY_RIGHT_SHIFT];
    io.KeyAlt = io.KeysDown[GLFW_KEY_LEFT_ALT] || io.KeysDown[GLFW_KEY_RIGHT_ALT];
    io.KeySuper = io.KeysDown[GLFW_KEY_LEFT_SUPER] || io.KeysDown[GLFW_KEY_RIGHT_SUPER];

    io.DeltaTime = elapsedTimeSeconds;
    io.MouseDrawCursor = false;

    ImGui::NewFrame();
    
    m_beginFrameCalled = true;
}

void ImGui_Renderer::Render(nvrhi::IFramebuffer* framebuffer)
{
    if (!imgui_nvrhi) return;

    buildUI();

    ImGui::Render();
    imgui_nvrhi->render(framebuffer);
    m_beginFrameCalled = false;

    // reconcile mouse button states
    auto& io = ImGui::GetIO();
    for(size_t i = 0; i < mouseDown.size(); i++)
    {
        if (io.MouseDown[i] == true && mouseDown[i] == false)
        {
            io.MouseDown[i] = false;
        }
    }

    // reconcile key states
    for(size_t i = 0; i < keyDown.size(); i++)
    {
        if (io.KeysDown[i] == true && keyDown[i] == false)
        {
            io.KeysDown[i] = false;
        }
    }
}

void ImGui_Renderer::BackBufferResizing()
{
    if(imgui_nvrhi) imgui_nvrhi->backbufferResizing();
}

void ImGui_Renderer::DisplayScaleChanged(float scaleX, float scaleY)
{
    // Apps that don't implement explicit scaling won't expect the fonts to be resized etc.
    if (!m_supportExplicitDisplayScaling)
        return;

    auto& io = ImGui::GetIO();

    // Clear the ImGui font atlas and invalidate the font texture
    // to re-register and re-rasterize all fonts on the next frame (see Animate)
    io.Fonts->Clear();
    io.Fonts->TexID = 0;

    for (auto& font : m_fonts)
        font->ReleaseScaledFont();
        
    ImGui::GetStyle() = ImGuiStyle();
    ImGui::GetStyle().ScaleAllSizes(scaleX);
}

void ImGui_Renderer::BeginFullScreenWindow()
{
    ImGuiIO const& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(
        io.DisplaySize.x / io.DisplayFramebufferScale.x,
        io.DisplaySize.y / io.DisplayFramebufferScale.y),
        ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::Begin(" ", 0, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
}

void ImGui_Renderer::DrawScreenCenteredText(const char* text)
{
    ImGuiIO const& io = ImGui::GetIO();
    ImVec2 textSize = ImGui::CalcTextSize(text);
    ImGui::SetCursorPosX((io.DisplaySize.x / io.DisplayFramebufferScale.x - textSize.x) * 0.5f);
    ImGui::SetCursorPosY((io.DisplaySize.y / io.DisplayFramebufferScale.y - textSize.y) * 0.5f);
    ImGui::TextUnformatted(text);
}

void ImGui_Renderer::EndFullScreenWindow()
{
    ImGui::End();
    ImGui::PopStyleVar();
}

void RegisteredFont::CreateScaledFont(float displayScale)
{
    ImFontConfig fontConfig;
    fontConfig.SizePixels = m_sizeAtDefaultScale * displayScale;

    m_imFont = nullptr;

    if (m_data)
    {
        fontConfig.FontDataOwnedByAtlas = false;
        if (m_isCompressed)
        {
            m_imFont = ImGui::GetIO().Fonts->AddFontFromMemoryCompressedTTF(
                (void*)(m_data->data()), (int)(m_data->size()), 0.f, &fontConfig);
        }
        else
        {
            m_imFont = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
                (void*)(m_data->data()), (int)(m_data->size()), 0.f, &fontConfig);
        }
    }
    else if (m_isDefault)
    {
        m_imFont = ImGui::GetIO().Fonts->AddFontDefault(&fontConfig);
    }

    if (m_imFont)
    {
        ImGui::GetIO().Fonts->TexID = 0;
    }
}

void RegisteredFont::ReleaseScaledFont()
{
    m_imFont = nullptr;
}
