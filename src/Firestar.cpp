#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    ID3D11Device* g_device{};
    ID3D11DeviceContext* g_context{};
    IDXGISwapChain* g_swapChain{};
    ID3D11RenderTargetView* g_renderTarget{};
    UINT g_resizeWidth{};
    UINT g_resizeHeight{};
    ImFont* g_bodyFont{};
    ImFont* g_headingFont{};
    HICON g_appIcon{};
    std::vector<std::uint8_t> g_embeddedFontBytes;

    constexpr int ResourceFirestarApplicationIcon = 1;
    constexpr int ResourceFirestarFont = 101;
    constexpr int ResourceFirestarMenuIcon = 102;

    struct EmbeddedResource
    {
        const std::uint8_t* bytes{};
        size_t size{};

        [[nodiscard]] explicit operator bool() const { return bytes != nullptr && size != 0; }
    };

    struct Rect
    {
        int x{};
        int y{};
        int width{};
        int height{};
    };

    struct Sprite
    {
        fs::path sourcePath;
        std::string ruiPath;
        UINT width{};
        UINT height{};
        std::vector<std::uint8_t> rgba;
        Rect packed{};
    };

    struct Settings
    {
        std::string outputDirectory;
        std::string packageName{"firestar_atlas"};
        std::string texturePath{"texture/ui/firestar_atlas.rpak"};
        std::string uimgPath{"rui/firestar_atlas.rpak"};
        int maxAtlasSize{8192};
        int padding{2};
        bool autoSelectSize{true};
        bool keepDevOnly{true};
    };

    [[nodiscard]] std::string WideToUtf8(const std::wstring_view value)
    {
        if (value.empty())
            return {};

        const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0)
            return {};

        std::string result(static_cast<size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
        return result;
    }

    [[nodiscard]] std::wstring Utf8ToWide(const std::string_view value)
    {
        if (value.empty())
            return {};

        const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (length <= 0)
            return {};

        std::wstring result(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
        return result;
    }

    [[nodiscard]] std::string PathToUtf8(const fs::path& path)
    {
        const auto text = path.u8string();
        return {reinterpret_cast<const char*>(text.data()), text.size()};
    }

    [[nodiscard]] fs::path PathFromUtf8(const std::string_view path)
    {
        return fs::path(Utf8ToWide(path));
    }

    [[nodiscard]] std::string ToLowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    [[nodiscard]] std::string NormalizeAssetPath(std::string value)
    {
        std::replace(value.begin(), value.end(), '\\', '/');
        while (value.find("//") != std::string::npos)
            value.replace(value.find("//"), 2, "/");
        while (!value.empty() && value.front() == '/')
            value.erase(value.begin());
        return value;
    }

    [[nodiscard]] std::string FileStemSafe(const std::string_view value)
    {
        std::string result;
        result.reserve(value.size());
        for (const unsigned char c : value)
        {
            if (std::isalnum(c) || c == '_' || c == '-')
                result.push_back(static_cast<char>(std::tolower(c)));
            else
                result.push_back('_');
        }
        while (!result.empty() && result.back() == '_')
            result.pop_back();
        return result.empty() ? "firestar_atlas" : result;
    }

    [[nodiscard]] std::string RUINameFromFile(const fs::path& path)
    {
        std::string stem = PathToUtf8(path.stem());
        std::string cleaned;
        cleaned.reserve(stem.size());
        for (const unsigned char c : stem)
        {
            if (std::isalnum(c) || c == '_' || c == '-')
                cleaned.push_back(static_cast<char>(std::tolower(c)));
            else
                cleaned.push_back('_');
        }
        return "rui/firestar/" + (cleaned.empty() ? "sprite" : cleaned);
    }

    [[nodiscard]] bool InputTextString(const char* label, std::string& value, const size_t minimumBuffer = 512)
    {
        std::vector<char> buffer((std::max)(minimumBuffer, value.size() + 2), '\0');
        std::memcpy(buffer.data(), value.data(), value.size());
        if (!ImGui::InputText(label, buffer.data(), buffer.size()))
            return false;
        value.assign(buffer.data());
        return true;
    }

    [[nodiscard]] std::string JsonEscape(const std::string_view value)
    {
        std::ostringstream out;
        for (const unsigned char c : value)
        {
            switch (c)
            {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20)
                {
                    constexpr char hex[] = "0123456789ABCDEF";
                    out << "\\u00" << hex[c >> 4] << hex[c & 0x0F];
                }
                else
                {
                    out << static_cast<char>(c);
                }
                break;
            }
        }
        return out.str();
    }

    [[nodiscard]] fs::path GetExecutablePath()
    {
        std::array<wchar_t, 32768> buffer{};
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size())
            return {};

        return fs::path(buffer.data(), buffer.data() + length);
    }

    [[nodiscard]] fs::path GetExecutableDirectory()
    {
        const fs::path executablePath = GetExecutablePath();
        return executablePath.empty() ? fs::current_path() : executablePath.parent_path();
    }

    [[nodiscard]] fs::path GetDefaultOutputDirectory()
    {
        const fs::path executableDirectory = GetExecutableDirectory();
        if (executableDirectory == fs::current_path())
            return executableDirectory / L"FirestarOutput";

        // Firestar/bin/Release/Firestar.exe -> Firestar/FirestarOutput.
        return executableDirectory.parent_path().parent_path() / L"FirestarOutput";
    }

    [[nodiscard]] bool SetPerUserRegistryValue(const std::wstring& subKey, const std::wstring& value, std::string& error)
    {
        HKEY key{};
        const LSTATUS openResult = RegCreateKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE, nullptr, &key, nullptr);
        if (openResult != ERROR_SUCCESS)
        {
            error = "Could not create the per-user file-association registry key (Win32 error " + std::to_string(openResult) + ").";
            return false;
        }

        const LSTATUS setResult = RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.data()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        if (setResult != ERROR_SUCCESS)
        {
            error = "Could not write the per-user file-association registry key (Win32 error " + std::to_string(setResult) + ").";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool RegisterFsaFileAssociation(std::string& error)
    {
        const fs::path executablePath = GetExecutablePath();
        if (executablePath.empty())
        {
            error = "Could not determine the Firestar executable path.";
            return false;
        }

        const std::wstring executable = executablePath.wstring();
        const std::wstring command = L"\"" + executable + L"\" \"%1\"";
        if (!SetPerUserRegistryValue(L"Software\\Classes\\.fsa", L"Firestar.Project", error) ||
            !SetPerUserRegistryValue(L"Software\\Classes\\Firestar.Project", L"Firestar Atlas Project", error) ||
            !SetPerUserRegistryValue(L"Software\\Classes\\Firestar.Project\\DefaultIcon", L"\"" + executable + L"\",0", error) ||
            !SetPerUserRegistryValue(L"Software\\Classes\\Firestar.Project\\shell\\open\\command", command, error))
        {
            return false;
        }

        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return true;
    }

    [[nodiscard]] EmbeddedResource LoadEmbeddedResource(const int resourceId)
    {
        const HMODULE module = GetModuleHandleW(nullptr);
        const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
        if (!resource)
            return {};
        const HGLOBAL resourceData = LoadResource(module, resource);
        const DWORD size = SizeofResource(module, resource);
        const auto* bytes = static_cast<const std::uint8_t*>(LockResource(resourceData));
        return {bytes, size};
    }

    void ApplyFirestarStyle()
    {
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowPadding = ImVec2(16.0f, 14.0f);
        style.FramePadding = ImVec2(10.0f, 7.0f);
        style.ItemSpacing = ImVec2(10.0f, 8.0f);
        style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
        style.WindowRounding = 7.0f;
        style.ChildRounding = 7.0f;
        style.FrameRounding = 5.0f;
        style.PopupRounding = 6.0f;
        style.ScrollbarRounding = 7.0f;
        style.GrabRounding = 5.0f;
        style.WindowBorderSize = 1.0f;
        style.FrameBorderSize = 1.0f;

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_Text] = ImVec4(0.95f, 0.95f, 0.96f, 1.0f);
        colors[ImGuiCol_TextDisabled] = ImVec4(0.72f, 0.74f, 0.78f, 1.0f);
        colors[ImGuiCol_WindowBg] = ImVec4(0.085f, 0.090f, 0.102f, 1.0f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.108f, 0.114f, 0.130f, 1.0f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.125f, 0.132f, 0.150f, 1.0f);
        colors[ImGuiCol_Border] = ImVec4(0.290f, 0.305f, 0.335f, 1.0f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.145f, 0.152f, 0.172f, 1.0f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.205f, 0.215f, 0.240f, 1.0f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.260f, 0.272f, 0.302f, 1.0f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.100f, 0.106f, 0.120f, 1.0f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.145f, 0.152f, 0.172f, 1.0f);
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.115f, 0.122f, 0.140f, 1.0f);
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.060f, 0.065f, 0.075f, 1.0f);
        colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.310f, 0.325f, 0.365f, 1.0f);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.420f, 0.440f, 0.490f, 1.0f);
        colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.60f, 0.23f, 1.0f);
        colors[ImGuiCol_SliderGrab] = ImVec4(0.94f, 0.48f, 0.16f, 1.0f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.68f, 0.32f, 1.0f);
        colors[ImGuiCol_Button] = ImVec4(0.310f, 0.205f, 0.120f, 1.0f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.475f, 0.295f, 0.150f, 1.0f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.650f, 0.400f, 0.190f, 1.0f);
        colors[ImGuiCol_Header] = ImVec4(0.265f, 0.185f, 0.125f, 1.0f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.420f, 0.270f, 0.150f, 1.0f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.585f, 0.365f, 0.180f, 1.0f);
        colors[ImGuiCol_Separator] = ImVec4(0.300f, 0.315f, 0.350f, 1.0f);
        colors[ImGuiCol_ResizeGrip] = ImVec4(0.430f, 0.460f, 0.520f, 0.35f);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.920f, 0.510f, 0.180f, 0.76f);
        colors[ImGuiCol_ResizeGripActive] = ImVec4(1.00f, 0.660f, 0.300f, 1.0f);
        colors[ImGuiCol_NavHighlight] = ImVec4(1.00f, 0.600f, 0.230f, 0.95f);
    }

    void LoadFirestarFonts()
    {
        ImGuiIO& io = ImGui::GetIO();
        const EmbeddedResource fontResource = LoadEmbeddedResource(ResourceFirestarFont);
        if (fontResource)
        {
            g_embeddedFontBytes.assign(fontResource.bytes, fontResource.bytes + fontResource.size);
            ImFontConfig fontConfig{};
            fontConfig.FontDataOwnedByAtlas = false;
            g_bodyFont = io.Fonts->AddFontFromMemoryTTF(g_embeddedFontBytes.data(), static_cast<int>(g_embeddedFontBytes.size()), 18.0f, &fontConfig);
            g_headingFont = io.Fonts->AddFontFromMemoryTTF(g_embeddedFontBytes.data(), static_cast<int>(g_embeddedFontBytes.size()), 30.0f, &fontConfig);
        }
        if (g_bodyFont)
            io.FontDefault = g_bodyFont;
        if (!g_headingFont)
            g_headingFont = g_bodyFont;
    }

    [[nodiscard]] HICON LoadWindowIconFromPng()
    {
        const EmbeddedResource iconResource = LoadEmbeddedResource(ResourceFirestarMenuIcon);
        if (!iconResource)
            return nullptr;

        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        ComPtr<IWICStream> stream;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromMemory(const_cast<std::uint8_t*>(iconResource.bytes), static_cast<DWORD>(iconResource.size))) ||
            FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom)))
        {
            return nullptr;
        }

        UINT width{};
        UINT height{};
        if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0 || width > 256 || height > 256)
            return nullptr;

        std::vector<std::uint8_t> rgba(static_cast<size_t>(width) * height * 4);
        if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(rgba.size()), rgba.data())))
            return nullptr;

        BITMAPV5HEADER bitmap{};
        bitmap.bV5Size = sizeof(bitmap);
        bitmap.bV5Width = static_cast<LONG>(width);
        bitmap.bV5Height = -static_cast<LONG>(height);
        bitmap.bV5Planes = 1;
        bitmap.bV5BitCount = 32;
        bitmap.bV5Compression = BI_BITFIELDS;
        bitmap.bV5RedMask = 0x00FF0000;
        bitmap.bV5GreenMask = 0x0000FF00;
        bitmap.bV5BlueMask = 0x000000FF;
        bitmap.bV5AlphaMask = 0xFF000000;

        HDC screen = GetDC(nullptr);
        void* colorPixels{};
        HBITMAP colorBitmap = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bitmap), DIB_RGB_COLORS, &colorPixels, nullptr, 0);
        if (!colorBitmap || !colorPixels)
        {
            if (colorBitmap) DeleteObject(colorBitmap);
            ReleaseDC(nullptr, screen);
            return nullptr;
        }

        auto* destination = static_cast<std::uint8_t*>(colorPixels);
        for (size_t pixel = 0; pixel < static_cast<size_t>(width) * height; ++pixel)
        {
            destination[pixel * 4 + 0] = rgba[pixel * 4 + 2];
            destination[pixel * 4 + 1] = rgba[pixel * 4 + 1];
            destination[pixel * 4 + 2] = rgba[pixel * 4 + 0];
            destination[pixel * 4 + 3] = rgba[pixel * 4 + 3];
        }

        HBITMAP maskBitmap = CreateBitmap(static_cast<int>(width), static_cast<int>(height), 1, 1, nullptr);
        ICONINFO iconInfo{};
        iconInfo.fIcon = TRUE;
        iconInfo.hbmMask = maskBitmap;
        iconInfo.hbmColor = colorBitmap;
        const HICON icon = maskBitmap ? CreateIconIndirect(&iconInfo) : nullptr;
        if (maskBitmap) DeleteObject(maskBitmap);
        DeleteObject(colorBitmap);
        ReleaseDC(nullptr, screen);
        return icon;
    }

    [[nodiscard]] std::vector<fs::path> ChooseImageFiles(const HWND owner)
    {
        std::vector<wchar_t> fileBuffer(65536, L'\0');
        constexpr wchar_t filter[] =
            L"Image files (*.png;*.bmp;*.jpg;*.jpeg;*.tga;*.tif;*.gif)\0*.png;*.bmp;*.jpg;*.jpeg;*.tga;*.tif;*.gif\0"
            L"All files (*.*)\0*.*\0\0";

        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = owner;
        dialog.lpstrFilter = filter;
        dialog.lpstrFile = fileBuffer.data();
        dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_HIDEREADONLY;
        dialog.lpstrTitle = L"Add atlas source images";

        if (!GetOpenFileNameW(&dialog))
            return {};

        const std::wstring first = fileBuffer.data();
        const wchar_t* next = fileBuffer.data() + first.size() + 1;
        if (*next == L'\0')
            return {fs::path(first)};

        std::vector<fs::path> paths;
        const fs::path directory(first);
        while (*next != L'\0')
        {
            paths.emplace_back(directory / next);
            next += std::wcslen(next) + 1;
        }
        return paths;
    }

    [[nodiscard]] std::optional<fs::path> ChooseFolder(const HWND owner)
    {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return std::nullopt;

        FILEOPENDIALOGOPTIONS options{};
        if (FAILED(dialog->GetOptions(&options)))
            return std::nullopt;
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"Choose Firestar output folder");
        if (FAILED(dialog->Show(owner)))
            return std::nullopt;

        ComPtr<IShellItem> item;
        PWSTR path{};
        if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            return std::nullopt;

        const fs::path result(path);
        CoTaskMemFree(path);
        return result;
    }

    [[nodiscard]] std::optional<fs::path> ChooseProjectFile(const HWND owner, const bool save)
    {
        std::array<wchar_t, 32768> fileName{};
        constexpr wchar_t filter[] = L"Firestar atlas project (*.fsa)\0*.fsa\0\0";
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = owner;
        dialog.lpstrFilter = filter;
        dialog.lpstrFile = fileName.data();
        dialog.nMaxFile = static_cast<DWORD>(fileName.size());
        dialog.lpstrDefExt = L"fsa";
        dialog.lpstrTitle = save ? L"Save Firestar atlas project" : L"Open Firestar atlas project";
        dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        if (save)
            dialog.Flags |= OFN_OVERWRITEPROMPT;
        else
            dialog.Flags |= OFN_FILEMUSTEXIST;

        const BOOL completed = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
        if (!completed)
            return std::nullopt;
        return fs::path(fileName.data());
    }

    [[nodiscard]] bool RectanglesIntersect(const Rect& a, const Rect& b)
    {
        return a.x < b.x + b.width && a.x + a.width > b.x && a.y < b.y + b.height && a.y + a.height > b.y;
    }

    [[nodiscard]] bool IsContainedIn(const Rect& inner, const Rect& outer)
    {
        return inner.x >= outer.x && inner.y >= outer.y &&
            inner.x + inner.width <= outer.x + outer.width &&
            inner.y + inner.height <= outer.y + outer.height;
    }

#pragma pack(push, 1)
    struct DdsPixelFormat
    {
        std::uint32_t size{};
        std::uint32_t flags{};
        std::uint32_t fourCC{};
        std::uint32_t rgbBitCount{};
        std::uint32_t rBitMask{};
        std::uint32_t gBitMask{};
        std::uint32_t bBitMask{};
        std::uint32_t aBitMask{};
    };

    struct DdsHeader
    {
        std::uint32_t size{};
        std::uint32_t flags{};
        std::uint32_t height{};
        std::uint32_t width{};
        std::uint32_t pitchOrLinearSize{};
        std::uint32_t depth{};
        std::uint32_t mipMapCount{};
        std::uint32_t reserved1[11]{};
        DdsPixelFormat pixelFormat{};
        std::uint32_t caps{};
        std::uint32_t caps2{};
        std::uint32_t caps3{};
        std::uint32_t caps4{};
        std::uint32_t reserved2{};
    };
#pragma pack(pop)

    static_assert(sizeof(DdsPixelFormat) == 32);
    static_assert(sizeof(DdsHeader) == 124);

    void CreateRenderTarget()
    {
        ID3D11Texture2D* backBuffer{};
        if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        {
            g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
            backBuffer->Release();
        }
    }

    void CleanupRenderTarget()
    {
        if (g_renderTarget)
        {
            g_renderTarget->Release();
            g_renderTarget = nullptr;
        }
    }

    [[nodiscard]] bool CreateDevice(const HWND window)
    {
        DXGI_SWAP_CHAIN_DESC description{};
        description.BufferCount = 2;
        description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.OutputWindow = window;
        description.SampleDesc.Count = 1;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL selected{};
        HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &description, &g_swapChain, &g_device, &selected, &g_context);
        if (result == DXGI_ERROR_UNSUPPORTED)
        {
            result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels,
                static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &description, &g_swapChain, &g_device, &selected, &g_context);
        }
        if (FAILED(result))
            return false;

        CreateRenderTarget();
        return g_renderTarget != nullptr;
    }

    void CleanupDevice()
    {
        CleanupRenderTarget();
        if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
        if (g_context) { g_context->Release(); g_context = nullptr; }
        if (g_device) { g_device->Release(); g_device = nullptr; }
    }

    class AtlasApplication
    {
    public:
        AtlasApplication()
        {
            settings_.outputDirectory = PathToUtf8(GetDefaultOutputDirectory());
        }

        ~AtlasApplication()
        {
            ClearPreview();
        }

        void AddFiles(const std::vector<fs::path>& paths)
        {
            const size_t originalCount = sprites_.size();
            for (const fs::path& path : paths)
            {
                if (sprites_.size() >= 65535)
                {
                    Log("Firestar supports at most 65,535 authored sprites in one atlas.", true);
                    break;
                }

                Sprite sprite;
                std::string error;
                if (!LoadImage(path, sprite, error))
                {
                    Log("Skipped " + PathToUtf8(path.filename()) + ": " + error, true);
                    continue;
                }

                sprite.ruiPath = MakeUniqueRuiName(RUINameFromFile(path));
                sprites_.push_back(std::move(sprite));
                Log("Added " + PathToUtf8(path.filename()));
            }
            InvalidatePacking();
            if (sprites_.size() > originalCount)
            {
                projectDirty_ = true;
                SetStatus("Loaded " + std::to_string(sprites_.size() - originalCount) + " image(s). Building a preview…");
            }
            else if (!paths.empty())
                SetStatus("No supported source images were loaded. See Activity for file-specific details.", true);
        }

        void AddDropHandle(const HDROP drop)
        {
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<fs::path> paths;
            paths.reserve(count);
            std::vector<wchar_t> buffer(32768);
            for (UINT index = 0; index < count; ++index)
            {
                const UINT length = DragQueryFileW(drop, index, buffer.data(), static_cast<UINT>(buffer.size()));
                if (length > 0 && length < buffer.size())
                    paths.emplace_back(buffer.data(), buffer.data() + length);
            }
            DragFinish(drop);
            AddFiles(paths);
        }

        void OpenProjectFromCommandLine(const fs::path& path)
        {
            (void)LoadProjectWithFeedback(path);
        }

        void ClearPreview()
        {
            if (previewTexture_)
            {
                previewTexture_->Release();
                previewTexture_ = nullptr;
            }
        }

        void Draw(const HWND window)
        {
            if (autoPreview_ && previewQueued_)
            {
                previewQueued_ = false;
                PackWithFeedback();
            }

            DrawMenu(window);
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            if (!mainLayoutInitialized_)
            {
                const float menuStripHeight = ImGui::GetFrameHeight() + 8.0f;
                ImGui::SetNextWindowViewport(viewport->ID);
                ImGui::SetNextWindowPos(viewport->WorkPos + ImVec2(10.0f, menuStripHeight), ImGuiCond_Always);
                ImGui::SetNextWindowSize(viewport->WorkSize - ImVec2(20.0f, menuStripHeight + 10.0f), ImGuiCond_Always);
                mainLayoutInitialized_ = true;
            }

            constexpr ImGuiWindowFlags mainFlags = ImGuiWindowFlags_NoCollapse;
            // Keep this independent root window in its own platform viewport
            // when it is dragged out. Re-merging a DX11 viewport into the host
            // caused its background to render almost black on some systems.
            ImGuiWindowClass atlasWindowClass{};
            atlasWindowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge | ImGuiViewportFlags_NoTaskBarIcon;
            ImGui::SetNextWindowClass(&atlasWindowClass);
            ImGui::SetNextWindowBgAlpha(1.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.085f, 0.090f, 0.102f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.108f, 0.114f, 0.130f, 1.0f));
            ImGui::Begin("Firestar", nullptr, mainFlags);
            if (g_headingFont) ImGui::PushFont(g_headingFont);
            ImGui::TextColored(ImVec4(1.0f, 0.57f, 0.20f, 1.0f), "FIRESTAR");
            if (g_headingFont) ImGui::PopFont();
            ImGui::TextDisabled("Apex Legends UI Atlas Builder");
            ImGui::Separator();

            const ImVec4 statusColor = statusIsError_ ? ImVec4(1.0f, 0.48f, 0.42f, 1.0f) : ImVec4(1.0f, 0.66f, 0.34f, 1.0f);
            ImGui::TextColored(statusColor, "%s", status_.c_str());

            const float workspaceHeight = (std::max)(230.0f, ImGui::GetContentRegionAvail().y - 18.0f);

            if (ImGui::BeginTable("FirestarWorkspace", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0.0f, workspaceHeight)))
            {
                ImGui::TableSetupColumn("Sprites", ImGuiTableColumnFlags_WidthStretch, 0.40f);
                ImGui::TableSetupColumn("Atlas preview", ImGuiTableColumnFlags_WidthStretch, 0.60f);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                DrawSprites(window, workspaceHeight);
                ImGui::TableNextColumn();
                DrawPreview(workspaceHeight);
                ImGui::EndTable();
            }

            ImGui::End();
            ImGui::PopStyleColor(2);
            DrawSettingsWindow(window);
        }

        [[nodiscard]] bool CreateSelfTest(const fs::path& outputRoot, std::string& error)
        {
            sprites_.clear();
            settings_.outputDirectory = PathToUtf8(outputRoot);
            settings_.packageName = "firestar_selftest";
            settings_.texturePath = "texture/ui/firestar_selftest.rpak";
            settings_.uimgPath = "rui/firestar_selftest.rpak";
            settings_.maxAtlasSize = 512;
            settings_.autoSelectSize = true;
            settings_.padding = 2;

            AddGeneratedSprite("rui/firestar_test/alpha_box", 96, 64, {255, 90, 50, 255});
            AddGeneratedSprite("rui/firestar_test/checker", 55, 127, {55, 178, 255, 255});
            AddGeneratedSprite("rui/firestar_test/ring", 145, 72, {255, 203, 80, 255});

            if (!Pack(error))
                return false;
            if (!Export(error))
                return false;

            const fs::path projectPath = outputRoot / L"firestar_selftest.fsa";
            if (!SaveProject(projectPath, error))
                return false;

            AtlasApplication reopenedProject;
            if (!reopenedProject.LoadProject(projectPath, error))
                return false;
            return reopenedProject.Export(error);
        }

    private:
        Settings settings_{};
        std::vector<Sprite> sprites_;
        std::vector<std::uint8_t> atlasPixels_;
        ID3D11ShaderResourceView* previewTexture_{};
        int atlasSize_{};
        int selectedSprite_{-1};
        bool packed_{};
        bool autoPreview_{true};
        bool previewQueued_{};
        bool fitPreview_{true};
        float previewZoom_{1.0f};
        float lastFitPreviewZoom_{0.10f};
        bool mainLayoutInitialized_{};
        bool showSettingsWindow_{};
        fs::path currentProjectPath_;
        bool projectDirty_{};
        std::string status_{"Add image files to build an atlas."};
        bool statusIsError_{};
        std::vector<std::pair<std::string, bool>> log_;

        void Log(std::string message, const bool error = false)
        {
            log_.emplace_back(std::move(message), error);
            if (log_.size() > 200)
                log_.erase(log_.begin(), log_.begin() + 50);
        }

        void SetStatus(std::string message, const bool isError = false)
        {
            status_ = std::move(message);
            statusIsError_ = isError;
        }

        bool PackWithFeedback()
        {
            std::string error;
            if (!Pack(error))
            {
                Log(error, true);
                SetStatus(error, true);
                return false;
            }
            SetStatus("Preview ready: " + std::to_string(sprites_.size()) + " image(s) packed into a " +
                std::to_string(atlasSize_) + " × " + std::to_string(atlasSize_) + " atlas.");
            return true;
        }

        bool ExportWithFeedback()
        {
            std::string error;
            if (!Export(error))
            {
                Log(error, true);
                SetStatus(error, true);
                return false;
            }
            SetStatus("Exported DDS atlas and RePak JSON to " + settings_.outputDirectory + ".");
            return true;
        }

        bool SaveProjectWithFeedback(const fs::path& path)
        {
            std::string error;
            if (!SaveProject(path, error))
            {
                Log(error, true);
                SetStatus(error, true);
                return false;
            }
            SetStatus("Saved " + PathToUtf8(path.filename()) + ".");
            return true;
        }

        bool LoadProjectWithFeedback(const fs::path& path)
        {
            std::string error;
            if (!LoadProject(path, error))
            {
                Log(error, true);
                SetStatus(error, true);
                return false;
            }
            return true;
        }

        [[nodiscard]] std::string MakeUniqueRuiName(const std::string& requested) const
        {
            std::set<std::string> names;
            for (const Sprite& sprite : sprites_)
                names.insert(ToLowerAscii(sprite.ruiPath));

            std::string result = requested;
            for (int suffix = 2; names.contains(ToLowerAscii(result)); ++suffix)
                result = requested + "_" + std::to_string(suffix);
            return result;
        }

        [[nodiscard]] static bool WriteBytes(std::ofstream& file, const void* data, const size_t byteCount)
        {
            file.write(static_cast<const char*>(data), static_cast<std::streamsize>(byteCount));
            return file.good();
        }

        [[nodiscard]] static bool ReadBytes(std::ifstream& file, void* data, const size_t byteCount)
        {
            file.read(static_cast<char*>(data), static_cast<std::streamsize>(byteCount));
            return file.good();
        }

        [[nodiscard]] static bool WriteU32(std::ofstream& file, const std::uint32_t value)
        {
            return WriteBytes(file, &value, sizeof(value));
        }

        [[nodiscard]] static bool ReadU32(std::ifstream& file, std::uint32_t& value)
        {
            return ReadBytes(file, &value, sizeof(value));
        }

        [[nodiscard]] static bool WriteU64(std::ofstream& file, const std::uint64_t value)
        {
            return WriteBytes(file, &value, sizeof(value));
        }

        [[nodiscard]] static bool ReadU64(std::ifstream& file, std::uint64_t& value)
        {
            return ReadBytes(file, &value, sizeof(value));
        }

        [[nodiscard]] static bool WriteString(std::ofstream& file, const std::string& value)
        {
            return value.size() <= (std::numeric_limits<std::uint32_t>::max)() &&
                WriteU32(file, static_cast<std::uint32_t>(value.size())) &&
                (value.empty() || WriteBytes(file, value.data(), value.size()));
        }

        [[nodiscard]] static bool ReadString(std::ifstream& file, std::string& value, const std::uint32_t maximumLength = 32768)
        {
            std::uint32_t length{};
            if (!ReadU32(file, length) || length > maximumLength)
                return false;
            value.resize(length);
            return length == 0 || ReadBytes(file, value.data(), length);
        }

        [[nodiscard]] bool SaveProject(const fs::path& path, std::string& error)
        {
            if (path.empty())
            {
                error = "Choose a .fsa destination before saving.";
                return false;
            }

            std::error_code directoryError;
            if (!path.parent_path().empty())
                fs::create_directories(path.parent_path(), directoryError);
            if (directoryError)
            {
                error = "Cannot create the project folder: " + directoryError.message();
                return false;
            }

            fs::path temporaryPath = path;
            temporaryPath += L".tmp";
            std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                error = "Cannot write project file: " + PathToUtf8(temporaryPath);
                return false;
            }

            constexpr std::array<char, 4> magic{'F', 'S', 'A', '1'};
            constexpr std::uint32_t version = 1;
            const std::uint32_t spriteCount = static_cast<std::uint32_t>(sprites_.size());
            const std::uint32_t atlasLimit = static_cast<std::uint32_t>(settings_.maxAtlasSize);
            const std::uint32_t padding = static_cast<std::uint32_t>(settings_.padding);
            const std::uint32_t autoSelect = settings_.autoSelectSize ? 1u : 0u;
            const std::uint32_t keepDevOnly = settings_.keepDevOnly ? 1u : 0u;

            bool written = WriteBytes(file, magic.data(), magic.size()) && WriteU32(file, version) &&
                WriteString(file, settings_.outputDirectory) && WriteString(file, settings_.packageName) &&
                WriteString(file, settings_.texturePath) && WriteString(file, settings_.uimgPath) &&
                WriteU32(file, atlasLimit) && WriteU32(file, padding) && WriteU32(file, autoSelect) &&
                WriteU32(file, keepDevOnly) && WriteU32(file, spriteCount);

            for (const Sprite& sprite : sprites_)
            {
                const std::string sourcePath = PathToUtf8(sprite.sourcePath);
                written = written && WriteString(file, sourcePath) && WriteString(file, sprite.ruiPath) &&
                    WriteU32(file, sprite.width) && WriteU32(file, sprite.height) &&
                    WriteU64(file, static_cast<std::uint64_t>(sprite.rgba.size())) &&
                    (sprite.rgba.empty() || WriteBytes(file, sprite.rgba.data(), sprite.rgba.size()));
                if (!written)
                    break;
            }
            file.close();
            if (!written || !file.good())
            {
                std::error_code cleanupError;
                fs::remove(temporaryPath, cleanupError);
                error = "Project write was incomplete.";
                return false;
            }

            if (!MoveFileExW(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                std::error_code cleanupError;
                fs::remove(temporaryPath, cleanupError);
                error = "Could not finalize the .fsa project file.";
                return false;
            }

            currentProjectPath_ = path;
            projectDirty_ = false;
            Log("Saved project " + PathToUtf8(path));
            return true;
        }

        [[nodiscard]] bool LoadProject(const fs::path& path, std::string& error)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                error = "Cannot open project file: " + PathToUtf8(path);
                return false;
            }

            std::array<char, 4> magic{};
            std::uint32_t version{};
            if (!ReadBytes(file, magic.data(), magic.size()) || !ReadU32(file, version) || magic != std::array<char, 4>{'F', 'S', 'A', '1'} || version != 1)
            {
                error = "This is not a supported Firestar .fsa project.";
                return false;
            }

            Settings loadedSettings;
            std::uint32_t atlasLimit{};
            std::uint32_t padding{};
            std::uint32_t autoSelect{};
            std::uint32_t keepDevOnly{};
            std::uint32_t spriteCount{};
            if (!ReadString(file, loadedSettings.outputDirectory) || !ReadString(file, loadedSettings.packageName) ||
                !ReadString(file, loadedSettings.texturePath) || !ReadString(file, loadedSettings.uimgPath) ||
                !ReadU32(file, atlasLimit) || !ReadU32(file, padding) || !ReadU32(file, autoSelect) ||
                !ReadU32(file, keepDevOnly) || !ReadU32(file, spriteCount) || spriteCount > 65535 ||
                atlasLimit < 64 || atlasLimit > 8192 || padding > 64 || autoSelect > 1 || keepDevOnly > 1)
            {
                error = "The .fsa settings block is invalid or corrupted.";
                return false;
            }
            loadedSettings.maxAtlasSize = static_cast<int>(atlasLimit);
            loadedSettings.padding = static_cast<int>(padding);
            loadedSettings.autoSelectSize = autoSelect != 0;
            loadedSettings.keepDevOnly = keepDevOnly != 0;

            std::vector<Sprite> loadedSprites;
            loadedSprites.reserve(spriteCount);
            constexpr std::uint64_t maximumProjectPixels = 1024ull * 1024ull * 1024ull;
            std::uint64_t totalPixelBytes{};
            for (std::uint32_t index = 0; index < spriteCount; ++index)
            {
                std::string sourcePath;
                Sprite sprite;
                std::uint64_t pixelBytes{};
                if (!ReadString(file, sourcePath) || !ReadString(file, sprite.ruiPath) || !ReadU32(file, sprite.width) ||
                    !ReadU32(file, sprite.height) || !ReadU64(file, pixelBytes) || sprite.width == 0 || sprite.height == 0 ||
                    sprite.width > 8192 || sprite.height > 8192 || sprite.ruiPath.empty())
                {
                    error = "The .fsa sprite list is invalid or corrupted.";
                    return false;
                }

                const std::uint64_t expectedPixelBytes = static_cast<std::uint64_t>(sprite.width) * sprite.height * 4;
                if (pixelBytes != expectedPixelBytes || pixelBytes > maximumProjectPixels ||
                    totalPixelBytes > maximumProjectPixels - pixelBytes)
                {
                    error = "The .fsa pixel data is invalid or exceeds Firestar's 1 GiB safety limit.";
                    return false;
                }
                totalPixelBytes += pixelBytes;
                sprite.sourcePath = PathFromUtf8(sourcePath);
                sprite.rgba.resize(static_cast<size_t>(pixelBytes));
                if (!ReadBytes(file, sprite.rgba.data(), sprite.rgba.size()))
                {
                    error = "The .fsa pixel data ended unexpectedly.";
                    return false;
                }
                loadedSprites.push_back(std::move(sprite));
            }

            settings_ = std::move(loadedSettings);
            sprites_ = std::move(loadedSprites);
            selectedSprite_ = -1;
            currentProjectPath_ = path;
            projectDirty_ = false;
            InvalidatePacking();
            Log("Opened project " + PathToUtf8(path));
            SetStatus("Opened " + PathToUtf8(path.filename()) + ". Rebuilding preview…");
            return true;
        }

        [[nodiscard]] static bool LoadImage(const fs::path& path, Sprite& output, std::string& error)
        {
            ComPtr<IWICImagingFactory> factory;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;

            const HRESULT createFactory = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            if (FAILED(createFactory) || FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, &decoder)) || FAILED(decoder->GetFrame(0, &frame)) ||
                FAILED(factory->CreateFormatConverter(&converter)) ||
                FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                    WICBitmapPaletteTypeCustom)))
            {
                error = "Windows Imaging Component could not decode the file";
                return false;
            }

            UINT width{};
            UINT height{};
            if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0 || width > 8192 || height > 8192)
            {
                error = "image dimensions must be between 1 and 8192 pixels";
                return false;
            }

            const size_t rowPitch = static_cast<size_t>(width) * 4;
            const size_t byteCount = rowPitch * height;
            output.rgba.resize(byteCount);
            if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(rowPitch), static_cast<UINT>(byteCount), output.rgba.data())))
            {
                error = "unable to read decoded pixels";
                return false;
            }

            output.sourcePath = path;
            output.width = width;
            output.height = height;
            return true;
        }

        void AddGeneratedSprite(const std::string& ruiPath, const UINT width, const UINT height, const std::array<std::uint8_t, 4> baseColor)
        {
            Sprite sprite;
            sprite.sourcePath = fs::path(Utf8ToWide(ruiPath + ".generated"));
            sprite.ruiPath = ruiPath;
            sprite.width = width;
            sprite.height = height;
            sprite.rgba.resize(static_cast<size_t>(width) * height * 4);
            for (UINT y = 0; y < height; ++y)
            {
                for (UINT x = 0; x < width; ++x)
                {
                    const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
                    const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
                    const bool checker = ((x / 8) + (y / 8)) % 2 == 0;
                    const float distance = (nx - 0.5f) * (nx - 0.5f) + (ny - 0.5f) * (ny - 0.5f);
                    const std::uint8_t alpha = distance < 0.23f ? 255 : (checker ? 90 : 0);
                    const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
                    sprite.rgba[offset + 0] = checker ? baseColor[0] : static_cast<std::uint8_t>(baseColor[0] / 2);
                    sprite.rgba[offset + 1] = checker ? baseColor[1] : static_cast<std::uint8_t>(baseColor[1] / 2);
                    sprite.rgba[offset + 2] = checker ? baseColor[2] : static_cast<std::uint8_t>(baseColor[2] / 2);
                    sprite.rgba[offset + 3] = alpha;
                }
            }
            sprites_.push_back(std::move(sprite));
        }

        void InvalidatePacking()
        {
            packed_ = false;
            atlasSize_ = 0;
            atlasPixels_.clear();
            ClearPreview();
            previewQueued_ = autoPreview_ && !sprites_.empty();
        }

        [[nodiscard]] bool ValidatePacking(std::string& error) const
        {
            if (sprites_.empty())
            {
                error = "Add at least one source image before packing.";
                return false;
            }
            if (sprites_.size() > 65535)
            {
                error = "The generated UIMG must contain at most 65,535 sprites.";
                return false;
            }
            if (settings_.padding < 0 || settings_.padding > 64)
            {
                error = "Padding must be between 0 and 64 pixels.";
                return false;
            }
            if (settings_.maxAtlasSize < 64 || settings_.maxAtlasSize > 8192)
            {
                error = "The maximum atlas size must be between 64 and 8192.";
                return false;
            }

            for (const Sprite& sprite : sprites_)
            {
                if (sprite.width == 0 || sprite.height == 0 || sprite.width > 8192 || sprite.height > 8192)
                {
                    error = "Source dimensions must be between 1 and 8192 pixels.";
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool Validate(std::string& error) const
        {
            if (!ValidatePacking(error))
                return false;

            const std::string texture = NormalizeAssetPath(settings_.texturePath);
            const std::string uimg = NormalizeAssetPath(settings_.uimgPath);
            if (!texture.starts_with("texture/") || !texture.ends_with(".rpak"))
            {
                error = "Texture asset path must start with texture/ and end with .rpak.";
                return false;
            }
            if (!uimg.starts_with("rui/") || !uimg.ends_with(".rpak"))
            {
                error = "UIMG asset path must start with rui/ and end with .rpak.";
                return false;
            }
            if (settings_.outputDirectory.empty())
            {
                error = "Choose an output folder for the atlas and map.";
                return false;
            }

            std::set<std::string> imageNames;
            for (const Sprite& sprite : sprites_)
            {
                const std::string imageName = NormalizeAssetPath(sprite.ruiPath);
                if (imageName.empty() || imageName.ends_with(".rpak"))
                {
                    error = "Each image name must be a non-empty RUI image path, not an .rpak asset path.";
                    return false;
                }
                if (!imageNames.insert(ToLowerAscii(imageName)).second)
                {
                    error = "RUI image names must be unique (case-insensitive): " + imageName;
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] static bool TryPackAtSize(const std::vector<Sprite>& sprites, const int padding, const int size, std::vector<Rect>& positions)
        {
            std::vector<size_t> order(sprites.size());
            for (size_t index = 0; index < order.size(); ++index)
                order[index] = index;
            std::sort(order.begin(), order.end(), [&sprites](const size_t a, const size_t b) {
                const int aLongest = static_cast<int>((std::max)(sprites[a].width, sprites[a].height));
                const int bLongest = static_cast<int>((std::max)(sprites[b].width, sprites[b].height));
                if (aLongest != bLongest)
                    return aLongest > bLongest;
                return static_cast<std::uint64_t>(sprites[a].width) * sprites[a].height >
                    static_cast<std::uint64_t>(sprites[b].width) * sprites[b].height;
            });

            std::vector<Rect> freeRects{{0, 0, size, size}};
            positions.assign(sprites.size(), {});
            for (const size_t index : order)
            {
                const int requestedWidth = static_cast<int>(sprites[index].width) + padding * 2;
                const int requestedHeight = static_cast<int>(sprites[index].height) + padding * 2;
                int bestFree = -1;
                int bestShortSide = (std::numeric_limits<int>::max)();
                int bestLongSide = (std::numeric_limits<int>::max)();

                for (size_t freeIndex = 0; freeIndex < freeRects.size(); ++freeIndex)
                {
                    const Rect& free = freeRects[freeIndex];
                    if (requestedWidth > free.width || requestedHeight > free.height)
                        continue;

                    const int leftoverHorizontal = free.width - requestedWidth;
                    const int leftoverVertical = free.height - requestedHeight;
                    const int shortSide = (std::min)(leftoverHorizontal, leftoverVertical);
                    const int longSide = (std::max)(leftoverHorizontal, leftoverVertical);
                    if (shortSide < bestShortSide || (shortSide == bestShortSide && longSide < bestLongSide))
                    {
                        bestFree = static_cast<int>(freeIndex);
                        bestShortSide = shortSide;
                        bestLongSide = longSide;
                    }
                }

                if (bestFree < 0)
                    return false;

                const Rect placed{freeRects[bestFree].x, freeRects[bestFree].y, requestedWidth, requestedHeight};
                positions[index] = {placed.x + padding, placed.y + padding, static_cast<int>(sprites[index].width), static_cast<int>(sprites[index].height)};

                for (size_t freeIndex = 0; freeIndex < freeRects.size();)
                {
                    const Rect free = freeRects[freeIndex];
                    if (!RectanglesIntersect(placed, free))
                    {
                        ++freeIndex;
                        continue;
                    }

                    if (placed.x > free.x)
                        freeRects.push_back({free.x, free.y, placed.x - free.x, free.height});
                    if (placed.x + placed.width < free.x + free.width)
                        freeRects.push_back({placed.x + placed.width, free.y, free.x + free.width - (placed.x + placed.width), free.height});
                    if (placed.y > free.y)
                        freeRects.push_back({free.x, free.y, free.width, placed.y - free.y});
                    if (placed.y + placed.height < free.y + free.height)
                        freeRects.push_back({free.x, placed.y + placed.height, free.width, free.y + free.height - (placed.y + placed.height)});

                    freeRects.erase(freeRects.begin() + static_cast<std::ptrdiff_t>(freeIndex));
                }

                for (size_t a = 0; a < freeRects.size();)
                {
                    bool removedA = false;
                    for (size_t b = a + 1; b < freeRects.size();)
                    {
                        if (IsContainedIn(freeRects[a], freeRects[b]))
                        {
                            freeRects.erase(freeRects.begin() + static_cast<std::ptrdiff_t>(a));
                            removedA = true;
                            break;
                        }
                        if (IsContainedIn(freeRects[b], freeRects[a]))
                        {
                            freeRects.erase(freeRects.begin() + static_cast<std::ptrdiff_t>(b));
                            continue;
                        }
                        ++b;
                    }
                    if (!removedA)
                        ++a;
                }
            }
            return true;
        }

        void CopySpriteWithPadding(const Sprite& sprite, std::vector<std::uint8_t>& pixels, const int atlasSize) const
        {
            const int padding = settings_.padding;
            const int baseX = sprite.packed.x;
            const int baseY = sprite.packed.y;
            const auto pixel = [&pixels, atlasSize](const int x, const int y) -> std::uint8_t* {
                return &pixels[(static_cast<size_t>(y) * atlasSize + x) * 4];
            };

            for (UINT y = 0; y < sprite.height; ++y)
            {
                for (UINT x = 0; x < sprite.width; ++x)
                {
                    const std::uint8_t* source = &sprite.rgba[(static_cast<size_t>(y) * sprite.width + x) * 4];
                    std::memcpy(pixel(baseX + static_cast<int>(x), baseY + static_cast<int>(y)), source, 4);
                }
            }

            // Extruded gutters prevent transparent-edge halos with linear UI sampling.
            for (int y = 0; y < static_cast<int>(sprite.height); ++y)
            {
                const std::uint8_t* left = pixel(baseX, baseY + y);
                const std::uint8_t* right = pixel(baseX + static_cast<int>(sprite.width) - 1, baseY + y);
                for (int p = 1; p <= padding; ++p)
                {
                    std::memcpy(pixel(baseX - p, baseY + y), left, 4);
                    std::memcpy(pixel(baseX + static_cast<int>(sprite.width) - 1 + p, baseY + y), right, 4);
                }
            }
            for (int p = 1; p <= padding; ++p)
            {
                for (int x = -padding; x < static_cast<int>(sprite.width) + padding; ++x)
                {
                    std::memcpy(pixel(baseX + x, baseY - p), pixel(baseX + x, baseY), 4);
                    std::memcpy(pixel(baseX + x, baseY + static_cast<int>(sprite.height) - 1 + p),
                        pixel(baseX + x, baseY + static_cast<int>(sprite.height) - 1), 4);
                }
            }
        }

        [[nodiscard]] bool Pack(std::string& error)
        {
            if (!ValidatePacking(error))
                return false;

            const int padding = settings_.padding;
            int largestRequired = 1;
            for (const Sprite& sprite : sprites_)
            {
                largestRequired = (std::max)(largestRequired, static_cast<int>((std::max)(sprite.width, sprite.height)) + padding * 2);
            }
            if (largestRequired > settings_.maxAtlasSize)
            {
                error = "The largest source image needs at least a " + std::to_string(largestRequired) +
                    "px atlas including padding; the current limit is " + std::to_string(settings_.maxAtlasSize) + "px.";
                return false;
            }

            int candidate = 64;
            while (candidate < largestRequired && candidate < 8192)
                candidate *= 2;
            if (!settings_.autoSelectSize)
                candidate = settings_.maxAtlasSize;

            std::vector<Rect> positions;
            int selectedSize{};
            for (;;)
            {
                if (candidate > settings_.maxAtlasSize)
                    break;
                if (TryPackAtSize(sprites_, padding, candidate, positions))
                {
                    selectedSize = candidate;
                    break;
                }
                if (!settings_.autoSelectSize || candidate >= settings_.maxAtlasSize)
                    break;
                candidate *= 2;
            }

            if (selectedSize == 0)
            {
                error = "Sprites do not fit within the selected maximum atlas size. Raise it or remove/resize source images.";
                return false;
            }

            const size_t byteCount = static_cast<size_t>(selectedSize) * selectedSize * 4;
            std::vector<std::uint8_t> pixels;
            try
            {
                pixels.assign(byteCount, 0);
            }
            catch (const std::bad_alloc&)
            {
                error = "Firestar could not allocate " + std::to_string(byteCount / (1024 * 1024)) + " MiB for this atlas.";
                return false;
            }
            for (size_t index = 0; index < sprites_.size(); ++index)
                sprites_[index].packed = positions[index];
            for (const Sprite& sprite : sprites_)
                CopySpriteWithPadding(sprite, pixels, selectedSize);

            atlasSize_ = selectedSize;
            atlasPixels_ = std::move(pixels);
            packed_ = true;
            if (!UpdatePreview(error))
                return false;
            Log("Packed " + std::to_string(sprites_.size()) + " sprites into " + std::to_string(atlasSize_) + "x" + std::to_string(atlasSize_) + " atlas.");
            return true;
        }

        [[nodiscard]] bool UpdatePreview(std::string& error)
        {
            ClearPreview();
            if (!g_device || atlasPixels_.empty() || atlasSize_ == 0)
                return true;

            D3D11_TEXTURE2D_DESC description{};
            description.Width = static_cast<UINT>(atlasSize_);
            description.Height = static_cast<UINT>(atlasSize_);
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_IMMUTABLE;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{};
            data.pSysMem = atlasPixels_.data();
            data.SysMemPitch = static_cast<UINT>(atlasSize_ * 4);

            ComPtr<ID3D11Texture2D> texture;
            const HRESULT textureResult = g_device->CreateTexture2D(&description, &data, &texture);
            if (FAILED(textureResult))
            {
                error = "DirectX could not create the atlas preview texture (0x" + std::to_string(static_cast<std::uint32_t>(textureResult)) + ").";
                return false;
            }
            const HRESULT viewResult = g_device->CreateShaderResourceView(texture.Get(), nullptr, &previewTexture_);
            if (FAILED(viewResult) || !previewTexture_)
            {
                error = "DirectX could not create the atlas preview view (0x" + std::to_string(static_cast<std::uint32_t>(viewResult)) + ").";
                return false;
            }
            return true;
        }

        [[nodiscard]] bool WriteDds(const fs::path& path, std::string& error) const
        {
            std::error_code createError;
            fs::create_directories(path.parent_path(), createError);
            if (createError)
            {
                error = "Cannot create texture directory: " + createError.message();
                return false;
            }

            DdsHeader header{};
            header.size = 124;
            header.flags = 0x0000100F; // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT
            header.height = static_cast<std::uint32_t>(atlasSize_);
            header.width = static_cast<std::uint32_t>(atlasSize_);
            header.pitchOrLinearSize = static_cast<std::uint32_t>(atlasSize_ * 4);
            header.mipMapCount = 1;
            header.pixelFormat.size = 32;
            header.pixelFormat.flags = 0x00000041; // DDPF_RGB | DDPF_ALPHAPIXELS
            header.pixelFormat.rgbBitCount = 32;
            // RePak maps this legacy A8B8G8R8 mask to its supported
            // DXGI_FORMAT_R8G8B8A8_UNORM RPAK texture format.
            header.pixelFormat.rBitMask = 0x000000FF;
            header.pixelFormat.gBitMask = 0x0000FF00;
            header.pixelFormat.bBitMask = 0x00FF0000;
            header.pixelFormat.aBitMask = 0xFF000000;
            header.caps = 0x00001000; // DDSCAPS_TEXTURE

            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                error = "Cannot write DDS file: " + PathToUtf8(path);
                return false;
            }
            constexpr std::uint32_t magic = 0x20534444; // 'DDS '
            file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
            file.write(reinterpret_cast<const char*>(&header), sizeof(header));

            file.write(reinterpret_cast<const char*>(atlasPixels_.data()), static_cast<std::streamsize>(atlasPixels_.size()));
            if (!file.good())
            {
                error = "DDS write was incomplete: " + PathToUtf8(path);
                return false;
            }
            return true;
        }

        [[nodiscard]] bool WriteManifest(const fs::path& path, const std::string& texturePath, const std::string& uimgPath, std::string& error) const
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                error = "Cannot write RePak map: " + PathToUtf8(path);
                return false;
            }

            file << "{\n"
                 << "  \"version\": 8,\n"
                 << "  \"name\": \"" << JsonEscape(FileStemSafe(settings_.packageName)) << "\",\n"
                 << "  \"assetsDir\": \"assets/\",\n"
                 << "  \"outputDir\": \"build/\",\n"
                 << "  \"keepDevOnly\": " << (settings_.keepDevOnly ? "true" : "false") << ",\n"
                 << "  \"keepServerOnly\": true,\n"
                 << "  \"keepClientOnly\": true,\n"
                 << "  \"files\": [\n"
                 << "    {\n"
                 << "      \"_type\": \"txtr\",\n"
                 << "      \"_path\": \"" << JsonEscape(texturePath) << "\",\n"
                 << "      \"$disableStreaming\": true\n"
                 << "    },\n"
                 << "    {\n"
                 << "      \"_type\": \"uimg\",\n"
                 << "      \"_path\": \"" << JsonEscape(uimgPath) << "\",\n"
                 << "      \"atlas\": \"" << JsonEscape(texturePath) << "\",\n"
                 << "      \"images\": [\n";

            for (size_t index = 0; index < sprites_.size(); ++index)
            {
                const Sprite& sprite = sprites_[index];
                file << "        {\n"
                     << "          \"path\": \"" << JsonEscape(NormalizeAssetPath(sprite.ruiPath)) << "\",\n"
                     << "          \"posX\": " << sprite.packed.x << ",\n"
                     << "          \"posY\": " << sprite.packed.y << ",\n"
                     << "          \"width\": " << sprite.width << ",\n"
                     << "          \"height\": " << sprite.height << "\n"
                     << "        }" << (index + 1 == sprites_.size() ? "\n" : ",\n");
            }
            file << "      ]\n"
                 << "    }\n"
                 << "  ]\n"
                 << "}\n";
            if (!file.good())
            {
                error = "RePak map write was incomplete: " + PathToUtf8(path);
                return false;
            }
            return true;
        }

        [[nodiscard]] bool Export(std::string& error)
        {
            if (!Validate(error))
                return false;
            if (!packed_ && !Pack(error))
                return false;

            const fs::path outputRoot = PathFromUtf8(settings_.outputDirectory);
            const std::string texturePath = NormalizeAssetPath(settings_.texturePath);
            const std::string uimgPath = NormalizeAssetPath(settings_.uimgPath);
            const std::string textureWithoutRpak = texturePath.substr(0, texturePath.size() - std::string_view(".rpak").size());
            const fs::path ddsPath = outputRoot / L"assets" / PathFromUtf8(textureWithoutRpak + ".dds");
            const fs::path manifestPath = outputRoot / Utf8ToWide(FileStemSafe(settings_.packageName) + ".json");

            std::error_code createError;
            fs::create_directories(outputRoot, createError);
            if (createError)
            {
                error = "Cannot create output folder: " + createError.message();
                return false;
            }
            // RePak canonicalizes outputDir before it starts writing, so create the
            // manifest-declared folder as part of a complete Firestar export.
            fs::create_directories(outputRoot / L"build", createError);
            if (createError)
            {
                error = "Cannot create RePak build folder: " + createError.message();
                return false;
            }
            if (!WriteDds(ddsPath, error) || !WriteManifest(manifestPath, texturePath, uimgPath, error))
                return false;

            Log("Exported DDS: " + PathToUtf8(ddsPath));
            Log("Exported RePak map: " + PathToUtf8(manifestPath));
            return true;
        }

        void DrawMenu(const HWND window)
        {
            if (!ImGui::BeginMainMenuBar())
                return;
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open project...", "Ctrl+O"))
                {
                    if (const auto path = ChooseProjectFile(window, false))
                        LoadProjectWithFeedback(*path);
                }
                if (ImGui::MenuItem("Save project", "Ctrl+S"))
                {
                    if (currentProjectPath_.empty())
                    {
                        if (const auto path = ChooseProjectFile(window, true))
                            SaveProjectWithFeedback(*path);
                    }
                    else
                    {
                        SaveProjectWithFeedback(currentProjectPath_);
                    }
                }
                if (ImGui::MenuItem("Save project as...", "Ctrl+Shift+S"))
                {
                    if (const auto path = ChooseProjectFile(window, true))
                        SaveProjectWithFeedback(*path);
                }
                if (ImGui::MenuItem("Add source images...", "Ctrl+I"))
                    AddFiles(ChooseImageFiles(window));
                if (ImGui::MenuItem("Choose output folder..."))
                {
                    if (const auto folder = ChooseFolder(window))
                    {
                        settings_.outputDirectory = PathToUtf8(*folder);
                        projectDirty_ = true;
                        SetStatus("Output folder set to " + settings_.outputDirectory + ".");
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit"))
                    PostMessageW(window, WM_CLOSE, 0, 0);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Atlas"))
            {
                if (ImGui::MenuItem("Pack preview", "Ctrl+P"))
                    PackWithFeedback();
                if (ImGui::MenuItem("Export atlas and RePak map", "Ctrl+Shift+E"))
                    ExportWithFeedback();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Settings"))
            {
                if (ImGui::MenuItem("Show settings", nullptr, showSettingsWindow_))
                    showSettingsWindow_ = !showSettingsWindow_;
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        void DrawSettingsWindow(const HWND window)
        {
            if (!showSettingsWindow_)
                return;

            ImGuiWindowClass settingsWindowClass{};
            settingsWindowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge | ImGuiViewportFlags_NoTaskBarIcon | ImGuiViewportFlags_NoDecoration;
            ImGui::SetNextWindowClass(&settingsWindowClass);
            ImGui::SetNextWindowSize(ImVec2(760.0f, 420.0f), ImGuiCond_FirstUseEver);
            // Keep the ImGui title strip so the borderless detached viewport
            // stays draggable, collapsible, and closable without a second
            // native Windows title bar.
            if (ImGui::Begin("Settings", &showSettingsWindow_))
            {
                ImGui::TextDisabled("Atlas layout, export location, and RPAK asset paths.");
                ImGui::SeparatorText("Build output");
                DrawOutput(window);
                ImGui::SeparatorText("Activity");
                DrawLog((std::max)(110.0f, ImGui::GetContentRegionAvail().y));
            }
            ImGui::End();
        }

        void DrawSprites(const HWND window, const float workspaceHeight)
        {
            ImGui::Text("Source images (%zu)", sprites_.size());
            ImGui::SameLine();
            if (ImGui::Button("Add images..."))
                AddFiles(ChooseImageFiles(window));
            ImGui::SameLine();
            if (ImGui::Button("Clear all") && !sprites_.empty())
                ImGui::OpenPopup("Confirm clear source images");
            ImGui::SameLine();
            if (ImGui::Button("Export DDS + JSON"))
                ExportWithFeedback();
            ImGui::TextDisabled("Drop files here or use Add images. Names become UIMG image paths.");

            int removeIndex = -1;
            const float listHeight = (std::max)(145.0f, workspaceHeight - 70.0f);
            if (ImGui::BeginChild("SpriteList", ImVec2(0, listHeight), ImGuiChildFlags_Borders))
            {
                for (size_t index = 0; index < sprites_.size(); ++index)
                {
                    Sprite& sprite = sprites_[index];
                    ImGui::PushID(static_cast<int>(index));
                    const bool selected = selectedSprite_ == static_cast<int>(index);
                    if (ImGui::Selectable(sprite.ruiPath.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
                        selectedSprite_ = static_cast<int>(index);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove"))
                        removeIndex = static_cast<int>(index);
                    ImGui::TextDisabled("%ux%u  -  %s", sprite.width, sprite.height, PathToUtf8(sprite.sourcePath.filename()).c_str());
                    ImGui::SetNextItemWidth(-1.0f);
                    if (InputTextString("##rui_path", sprite.ruiPath))
                    {
                        // Logical image names affect the exported UIMG record, not its packed pixels.
                        projectDirty_ = true;
                    }
                    ImGui::Separator();
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            if (removeIndex >= 0)
            {
                Log("Removed " + sprites_[removeIndex].ruiPath);
                sprites_.erase(sprites_.begin() + removeIndex);
                if (selectedSprite_ >= static_cast<int>(sprites_.size()))
                    selectedSprite_ = static_cast<int>(sprites_.size()) - 1;
                InvalidatePacking();
                projectDirty_ = true;
                SetStatus("Removed source image. Rebuild the preview when ready.");
            }

            if (ImGui::BeginPopupModal("Confirm clear source images", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted("Clear every source image from this project?");
                ImGui::TextDisabled("This only clears the current Firestar project; it does not delete source files.");
                ImGui::Separator();
                if (ImGui::Button("Clear images", ImVec2(125.0f, 0.0f)))
                {
                    sprites_.clear();
                    selectedSprite_ = -1;
                    InvalidatePacking();
                    projectDirty_ = true;
                    Log("Cleared source sprite list.");
                    SetStatus("Source image list cleared.");
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }

        void DrawPreview(const float workspaceHeight)
        {
            ImGui::Text("Atlas preview");
            ImGui::SameLine();
            if (ImGui::Checkbox("Auto update", &autoPreview_) && autoPreview_ && !packed_ && !sprites_.empty())
                previewQueued_ = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("Fit preview", &fitPreview_) && !fitPreview_)
                previewZoom_ = lastFitPreviewZoom_;
            if (!fitPreview_)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(118.0f);
                float zoomPercent = previewZoom_ * 100.0f;
                if (ImGui::SliderFloat("Zoom", &zoomPercent, 1.0f, 200.0f, "%.0f%%", ImGuiSliderFlags_Logarithmic))
                    previewZoom_ = zoomPercent / 100.0f;
            }

            const float canvasHeight = (std::max)(145.0f, workspaceHeight - 69.0f);

            if (!packed_ || !previewTexture_)
            {
                ImGui::BeginChild("AtlasPreviewCanvas", ImVec2(0, canvasHeight), ImGuiChildFlags_Borders);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (canvasHeight - 50.0f) * 0.42f);
                const ImVec4 messageColor = statusIsError_ ? ImVec4(1.0f, 0.48f, 0.42f, 1.0f) : ImVec4(1.0f, 0.66f, 0.34f, 1.0f);
                ImGui::TextColored(messageColor, "%s", status_.c_str());
                ImGui::Spacing();
                ImGui::TextDisabled("The preview is generated from the same pixels written to the DDS export.");
                ImGui::EndChild();
                return;
            }

            ImGui::TextDisabled("%dx%d  |  RGBA8 DDS  |  base mip  |  non-streamed txtr", atlasSize_, atlasSize_);
            int hoveredSprite = -1;
            if (ImGui::BeginChild("AtlasPreviewCanvas", ImVec2(0, canvasHeight), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            {
                const ImVec2 canvasPosition = ImGui::GetCursorScreenPos();
                const ImVec2 available = ImGui::GetContentRegionAvail();
                const float fitScale = (std::max)(0.05f, (std::min)((available.x - 12.0f) / atlasSize_, (available.y - 12.0f) / atlasSize_));
                lastFitPreviewZoom_ = fitScale;
                const float scale = fitPreview_ ? fitScale : previewZoom_;
                const ImVec2 imageSize(atlasSize_ * scale, atlasSize_ * scale);
                const ImVec2 imagePosition(
                    canvasPosition.x + (std::max)(0.0f, (available.x - imageSize.x) * 0.5f),
                    canvasPosition.y + (std::max)(0.0f, (available.y - imageSize.y) * 0.5f));
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                const ImVec2 imageEnd(imagePosition.x + imageSize.x, imagePosition.y + imageSize.y);

                drawList->PushClipRect(canvasPosition, ImVec2(canvasPosition.x + available.x, canvasPosition.y + available.y), true);
                drawList->AddRectFilled(imagePosition, imageEnd, IM_COL32(48, 50, 54, 255));
                const float checkerSize = (std::max)(8.0f, 18.0f * scale);
                for (float y = imagePosition.y; y < imageEnd.y; y += checkerSize)
                {
                    for (float x = imagePosition.x; x < imageEnd.x; x += checkerSize)
                    {
                        const int row = static_cast<int>((y - imagePosition.y) / checkerSize);
                        const int column = static_cast<int>((x - imagePosition.x) / checkerSize);
                        const ImU32 color = ((row + column) & 1) == 0 ? IM_COL32(82, 84, 90, 255) : IM_COL32(62, 64, 70, 255);
                        drawList->AddRectFilled(ImVec2(x, y), ImVec2((std::min)(x + checkerSize, imageEnd.x), (std::min)(y + checkerSize, imageEnd.y)), color);
                    }
                }

                // Keep one regular ImGui item inside the child. Drawing the texture directly
                // avoids SetCursorScreenPos extending the parent bounds (an ImGui assertion).
                ImGui::Dummy(available);
                drawList->AddImage(reinterpret_cast<ImTextureID>(previewTexture_), imagePosition, imageEnd);

                const ImVec2 mouse = ImGui::GetMousePos();
                for (size_t index = 0; index < sprites_.size(); ++index)
                {
                    const Sprite& sprite = sprites_[index];
                    const ImVec2 min(imagePosition.x + sprite.packed.x * scale, imagePosition.y + sprite.packed.y * scale);
                    const ImVec2 max(min.x + sprite.packed.width * scale, min.y + sprite.packed.height * scale);
                    if (mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y)
                        hoveredSprite = static_cast<int>(index);
                    const bool selected = selectedSprite_ == static_cast<int>(index);
                    const bool hovered = hoveredSprite == static_cast<int>(index);
                    const ImU32 color = selected ? IM_COL32(255, 183, 87, 255) : (hovered ? IM_COL32(255, 143, 51, 255) : IM_COL32(214, 122, 52, 190));
                    drawList->AddRect(min, max, color, 1.5f, 0, selected ? 2.5f : (hovered ? 2.0f : 1.0f));
                }
                drawList->PopClipRect();
            }
            ImGui::EndChild();

            if (hoveredSprite >= 0)
            {
                const Sprite& sprite = sprites_[hoveredSprite];
                ImGui::TextColored(ImVec4(1.0f, 0.66f, 0.34f, 1.0f), "Hover: %s  |  %ux%u at (%d, %d)", sprite.ruiPath.c_str(), sprite.width, sprite.height, sprite.packed.x, sprite.packed.y);
            }
            else if (selectedSprite_ >= 0 && selectedSprite_ < static_cast<int>(sprites_.size()))
            {
                const Sprite& sprite = sprites_[selectedSprite_];
                ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.42f, 1.0f), "Selected: %s  |  %ux%u at (%d, %d)", sprite.ruiPath.c_str(), sprite.width, sprite.height, sprite.packed.x, sprite.packed.y);
            }
            else
            {
                ImGui::TextDisabled("Hover a rectangle to inspect its packed coordinates; select a sprite in the list to keep it highlighted.");
            }
        }

        void DrawOutput(const HWND window)
        {
            bool layoutChanged = false;
            ImGui::TextUnformatted("Output folder");
            ImGui::SameLine();
            const float browseButtonWidth = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            const float outputInputWidth = ImGui::GetContentRegionAvail().x - browseButtonWidth - ImGui::GetStyle().ItemSpacing.x;
            if (outputInputWidth > 150.0f)
            {
                ImGui::SetNextItemWidth(outputInputWidth);
                if (InputTextString("##output_folder", settings_.outputDirectory, 2048))
                    projectDirty_ = true;
                ImGui::SameLine();
                if (ImGui::Button("Browse..."))
                {
                    if (const auto folder = ChooseFolder(window))
                    {
                        settings_.outputDirectory = PathToUtf8(*folder);
                        projectDirty_ = true;
                        SetStatus("Output folder set to " + settings_.outputDirectory + ".");
                    }
                }
            }
            else
            {
                ImGui::NewLine();
                ImGui::SetNextItemWidth(-1.0f);
                if (InputTextString("##output_folder", settings_.outputDirectory, 2048))
                    projectDirty_ = true;
                if (ImGui::Button("Browse..."))
                {
                    if (const auto folder = ChooseFolder(window))
                    {
                        settings_.outputDirectory = PathToUtf8(*folder);
                        projectDirty_ = true;
                        SetStatus("Output folder set to " + settings_.outputDirectory + ".");
                    }
                }
            }

            constexpr std::array<int, 6> atlasSizes{256, 512, 1024, 2048, 4096, 8192};
            int sizeIndex = 0;
            for (int index = 0; index < static_cast<int>(atlasSizes.size()); ++index)
            {
                if (atlasSizes[index] == settings_.maxAtlasSize)
                    sizeIndex = index;
            }
            ImGui::SetNextItemWidth(110.0f);
            constexpr const char* atlasSizeNames[] = {"256", "512", "1024", "2048", "4096", "8192"};
            if (ImGui::Combo("Atlas limit", &sizeIndex, atlasSizeNames, static_cast<int>(std::size(atlasSizeNames))))
            {
                settings_.maxAtlasSize = atlasSizes[sizeIndex];
                layoutChanged = true;
            }
            ImGui::SameLine();
            layoutChanged |= ImGui::Checkbox("Use smallest fitting size", &settings_.autoSelectSize);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.0f);
            layoutChanged |= ImGui::InputInt("Padding", &settings_.padding);
            ImGui::SameLine();
            if (ImGui::Button("Open folder"))
                ShellExecuteW(window, L"open", PathFromUtf8(settings_.outputDirectory).c_str(), nullptr, nullptr, SW_SHOWNORMAL);

            if (ImGui::CollapsingHeader("Advanced asset paths"))
            {
                ImGui::SetNextItemWidth(300.0f);
                if (InputTextString("Package name", settings_.packageName))
                    projectDirty_ = true;
                ImGui::SameLine();
                if (ImGui::Button("Reset paths"))
                {
                    const std::string name = FileStemSafe(settings_.packageName);
                    settings_.texturePath = "texture/ui/" + name + ".rpak";
                    settings_.uimgPath = "rui/" + name + ".rpak";
                    projectDirty_ = true;
                }
                ImGui::SetNextItemWidth(430.0f);
                if (InputTextString("Texture asset", settings_.texturePath))
                    projectDirty_ = true;
                ImGui::SetNextItemWidth(430.0f);
                if (InputTextString("UIMG asset", settings_.uimgPath))
                    projectDirty_ = true;
                if (ImGui::Checkbox("Keep development image names", &settings_.keepDevOnly))
                    projectDirty_ = true;
                ImGui::SameLine();
                ImGui::TextDisabled("(keepDevOnly)");
            }

            if (layoutChanged)
            {
                InvalidatePacking();
                projectDirty_ = true;
                SetStatus("Atlas settings changed. The preview will rebuild automatically.");
            }

            ImGui::TextDisabled("Export writes assets/<texture>.dds and <package>.json. Use the top Export button when the preview is ready.");
        }

        void DrawLog(const float height)
        {
            if (!ImGui::BeginChild("ActivityLog", ImVec2(0, height), ImGuiChildFlags_Borders))
            {
                ImGui::EndChild();
                return;
            }
            for (const auto& [message, isError] : log_)
                ImGui::TextColored(isError ? ImVec4(1.0f, 0.36f, 0.30f, 1.0f) : ImVec4(0.83f, 0.84f, 0.89f, 1.0f), "%s", message.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }
    };

    AtlasApplication* g_application{};

    BOOL CALLBACK ApplyFirestarIconToPlatformWindow(const HWND candidate, LPARAM)
    {
        DWORD processId{};
        GetWindowThreadProcessId(candidate, &processId);
        if (processId == GetCurrentProcessId() && g_appIcon)
        {
            SendMessageW(candidate, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon));
            SendMessageW(candidate, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIcon));
        }
        return TRUE;
    }

    void ApplyFirestarIconToPlatformWindows()
    {
        if (g_appIcon)
            EnumWindows(ApplyFirestarIconToPlatformWindow, 0);
    }

    LRESULT WINAPI WindowProcedure(const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam)
    {
        if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam))
            return true;

        switch (message)
        {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED)
            {
                g_resizeWidth = LOWORD(lParam);
                g_resizeHeight = HIWORD(lParam);
            }
            return 0;
        case WM_DROPFILES:
            if (g_application)
                g_application->AddDropHandle(reinterpret_cast<HDROP>(wParam));
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    [[nodiscard]] int RunSelfTest(const fs::path& outputDirectory)
    {
        AtlasApplication app;
        std::string error;
        if (!app.CreateSelfTest(outputDirectory, error))
        {
            OutputDebugStringA(("Firestar self-test failed: " + error + "\n").c_str());
            return 1;
        }
        OutputDebugStringA("Firestar self-test passed.\n");
        return 0;
    }
}

int APIENTRY wWinMain(const HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    int argumentCount{};
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    std::optional<fs::path> projectToOpen;
    if (arguments)
    {
        bool selfTest = false;
        bool registerFsa = false;
        fs::path selfTestOutput = GetDefaultOutputDirectory() / L"selftest-output";
        for (int index = 1; index < argumentCount; ++index)
        {
            if (_wcsicmp(arguments[index], L"--self-test") == 0)
            {
                selfTest = true;
                if (index + 1 < argumentCount && arguments[index + 1][0] != L'-')
                    selfTestOutput = arguments[++index];
            }
            else if (_wcsicmp(arguments[index], L"--register-fsa") == 0)
            {
                registerFsa = true;
            }
            else
            {
                const fs::path candidate(arguments[index]);
                if (_wcsicmp(candidate.extension().c_str(), L".fsa") == 0)
                    projectToOpen = candidate;
            }
        }
        LocalFree(arguments);
        if (registerFsa)
        {
            std::string error;
            return RegisterFsaFileAssociation(error) ? 0 : 1;
        }
        if (selfTest)
        {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const int result = RunSelfTest(selfTestOutput);
            if (SUCCEEDED(com)) CoUninitialize();
            return result;
        }
    }

    // Keep the per-user project association current after Firestar is moved
    // or rebuilt. A failure here must not prevent the atlas builder opening.
    std::string associationError;
    if (!RegisterFsaFileAssociation(associationError))
        OutputDebugStringA(("Firestar .fsa association update failed: " + associationError + "\n").c_str());

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    g_appIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(ResourceFirestarApplicationIcon), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    if (!g_appIcon)
        g_appIcon = LoadWindowIconFromPng();
    windowClass.hIcon = g_appIcon;
    windowClass.hIconSm = g_appIcon;
    windowClass.lpszClassName = L"FirestarAtlasBuilder";
    RegisterClassExW(&windowClass);

    const HWND window = CreateWindowW(windowClass.lpszClassName, L"Firestar", WS_OVERLAPPEDWINDOW,
        100, 100, 1440, 940, nullptr, nullptr, instance, nullptr);
    if (!window || !CreateDevice(window))
    {
        CleanupDevice();
        if (g_appIcon) { DestroyIcon(g_appIcon); g_appIcon = nullptr; }
        UnregisterClassW(windowClass.lpszClassName, instance);
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    if (g_appIcon)
    {
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon));
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIcon));
    }
    DragAcceptFiles(window, TRUE);
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_ViewportsEnable;
    ApplyFirestarStyle();
    ImGui::GetStyle().WindowRounding = 0.0f;
    LoadFirestarFonts();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_context);

    AtlasApplication app;
    if (projectToOpen)
        app.OpenProjectFromCommandLine(*projectToOpen);
    g_application = &app;
    bool done = false;
    while (!done)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        if (g_resizeWidth != 0 && g_resizeHeight != 0)
        {
            CleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = 0;
            g_resizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app.Draw(window);
        ImGui::Render();

        constexpr float clearColor[] = {0.075f, 0.080f, 0.090f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        g_context->ClearRenderTargetView(g_renderTarget, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        ImGui::UpdatePlatformWindows();
        ApplyFirestarIconToPlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        g_swapChain->Present(1, 0);
    }

    g_application = nullptr;
    app.ClearPreview();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, instance);
    if (g_appIcon) { DestroyIcon(g_appIcon); g_appIcon = nullptr; }
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}
