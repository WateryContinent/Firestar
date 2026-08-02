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
#include "FirestarVersion.h"
#include "editor/ProjectDocument.h"
#include "editor/AssetCompatibility.h"
#include "editor/AudioPreview.h"
#include "editor/PackedProject.h"
#include "editor/RmdlPreview.h"
#include "repak/application/embedded.h"
#include "repak/logic/rtech.h"
#include "repak/utils/dxutils.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

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
// Dear ImGui exposes this for selecting the swap-chain template used by
// detached platform windows, but the docking backend does not publish it in
// the header yet.
void ImGui_ImplDX11_SetSwapChainDescs(const DXGI_SWAP_CHAIN_DESC* descTemplates, int descTemplateCount);

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
    std::string g_imguiIniPath;

    constexpr int ResourceFirestarApplicationIcon = 1;
    constexpr int ResourceFirestarFont = 101;
    constexpr int ResourceFirestarMenuIcon = 102;
    constexpr int ResourceRePakAssetTypesReference = 103;

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

    struct ModelPreviewMaterialTexture
    {
        std::uint64_t guid{};
        ComPtr<ID3D11ShaderResourceView> texture;
        fs::path sourcePath;
        bool missing{true};
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

    struct RePakAssetTypeOption
    {
        const char* code;
        const char* name;
    };

    // Kept in the same order as the handlers in the vendored RePak pakfile.cpp.
    constexpr std::array<RePakAssetTypeOption, 21> RePakAssetTypes{{
        {"txtr", "Texture"}, {"matl", "Material"}, {"mdl_", "Model"},
        {"arig", "Animation rig"}, {"aseq", "Animation sequence"}, {"anir", "Animation recording"},
        {"txan", "Texture animation"}, {"txls", "Texture list"}, {"uimg", "UI image atlas"},
        {"ui", "RUI package"}, {"shdr", "Shader"}, {"shds", "Shader set"},
        {"dtbl", "Data table"}, {"stlt", "Settings layout"}, {"stgs", "Settings"},
        {"mt4a", "Material for aspect"}, {"font", "Font"}, {"rlcd", "LCD screen effect"},
        {"awsr", "Audio source"}, {"aevt", "Audio event"}, {"Ptch", "Patch relationship"}
    }};

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

    [[nodiscard]] std::string DefaultMandatoryStarPakPath(const std::string_view packageName)
    {
        return "paks/Win64/" + FileStemSafe(packageName) + ".starpak";
    }

    [[nodiscard]] std::string DefaultOptionalStarPakPath(const std::string_view packageName)
    {
        return "paks/Win64/" + FileStemSafe(packageName) + ".opt.starpak";
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

    [[nodiscard]] fs::path GetUserSettingsPath()
    {
        std::array<wchar_t, 32768> buffer{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
        const fs::path root = length > 0 && length < buffer.size()
            ? fs::path(buffer.data(), buffer.data() + length) : GetExecutableDirectory();
        return root / L"Firestar" / L"settings.json";
    }

    [[nodiscard]] fs::path GetImGuiIniPath()
    {
        PWSTR documentsPath{};
        fs::path root;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &documentsPath)) &&
            documentsPath)
        {
            root = documentsPath;
            CoTaskMemFree(documentsPath);
        }
        else
        {
            if (documentsPath) CoTaskMemFree(documentsPath);
            root = GetUserSettingsPath().parent_path().parent_path();
        }
        root /= L"Firestar";
        std::error_code error;
        fs::create_directories(root, error);
        return root / L"imgui.ini";
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
            !SetPerUserRegistryValue(L"Software\\Classes\\Firestar.Project\\shell\\open\\command", command, error) ||
            !SetPerUserRegistryValue(L"Software\\Classes\\.fsp", L"Firestar.Workspace", error) ||
            !SetPerUserRegistryValue(L"Software\\Classes\\Firestar.Workspace", L"Firestar RPAK Project", error) ||
            !SetPerUserRegistryValue(L"Software\\Classes\\Firestar.Workspace\\DefaultIcon", L"\"" + executable + L"\",0", error) ||
            !SetPerUserRegistryValue(L"Software\\Classes\\Firestar.Workspace\\shell\\open\\command", command, error) ||
            !SetPerUserRegistryValue(L"Software\\Classes\\.fspa", L"Firestar.PackedWorkspace", error) ||
            !SetPerUserRegistryValue(L"Software\\Classes\\Firestar.PackedWorkspace", L"Packed Firestar Project", error) ||
            !SetPerUserRegistryValue(L"Software\\Classes\\Firestar.PackedWorkspace\\DefaultIcon", L"\"" + executable + L"\",0", error) ||
            !SetPerUserRegistryValue(L"Software\\Classes\\Firestar.PackedWorkspace\\shell\\open\\command", command, error))
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
        colors[ImGuiCol_WindowBg] = ImVec4(0.070f, 0.074f, 0.084f, 1.0f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.090f, 0.095f, 0.108f, 1.0f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.185f, 0.195f, 0.220f, 1.0f);
        colors[ImGuiCol_Border] = ImVec4(0.360f, 0.375f, 0.415f, 1.0f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.145f, 0.152f, 0.172f, 1.0f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.205f, 0.215f, 0.240f, 1.0f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.260f, 0.272f, 0.302f, 1.0f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.100f, 0.106f, 0.120f, 1.0f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.145f, 0.152f, 0.172f, 1.0f);
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.165f, 0.175f, 0.200f, 1.0f);
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.060f, 0.065f, 0.075f, 1.0f);
        colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.310f, 0.325f, 0.365f, 1.0f);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.420f, 0.440f, 0.490f, 1.0f);
        colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.60f, 0.23f, 1.0f);
        colors[ImGuiCol_SliderGrab] = ImVec4(0.94f, 0.48f, 0.16f, 1.0f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.68f, 0.32f, 1.0f);
        colors[ImGuiCol_Button] = ImVec4(0.310f, 0.205f, 0.120f, 1.0f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.475f, 0.295f, 0.150f, 1.0f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.650f, 0.400f, 0.190f, 1.0f);
        colors[ImGuiCol_Header] = ImVec4(0.340f, 0.225f, 0.140f, 1.0f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.505f, 0.325f, 0.175f, 1.0f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.660f, 0.430f, 0.220f, 1.0f);
        colors[ImGuiCol_Tab] = ImVec4(0.145f, 0.152f, 0.172f, 1.0f);
        colors[ImGuiCol_TabHovered] = ImVec4(0.475f, 0.295f, 0.150f, 1.0f);
        colors[ImGuiCol_TabSelected] = ImVec4(0.340f, 0.225f, 0.140f, 1.0f);
        colors[ImGuiCol_TabSelectedOverline] = ImVec4(1.00f, 0.60f, 0.23f, 1.0f);
        colors[ImGuiCol_TabDimmed] = ImVec4(0.110f, 0.116f, 0.132f, 1.0f);
        colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.250f, 0.180f, 0.120f, 1.0f);
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

    [[nodiscard]] std::optional<fs::path> ChooseFolder(const HWND owner,
        const wchar_t* const title = L"Choose Firestar output folder")
    {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return std::nullopt;

        FILEOPENDIALOGOPTIONS options{};
        if (FAILED(dialog->GetOptions(&options)))
            return std::nullopt;
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(title);
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
        dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        if (save)
            dialog.Flags |= OFN_OVERWRITEPROMPT;
        else
            dialog.Flags |= OFN_FILEMUSTEXIST;

        const BOOL completed = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
        if (!completed)
            return std::nullopt;
        return fs::path(fileName.data());
    }

    [[nodiscard]] std::optional<fs::path> ChooseRePakManifest(const HWND owner, const bool save)
    {
        std::array<wchar_t, 32768> fileName{};
        constexpr wchar_t filter[] = L"RePak build maps (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = owner;
        dialog.lpstrFilter = filter;
        dialog.lpstrFile = fileName.data();
        dialog.nMaxFile = static_cast<DWORD>(fileName.size());
        dialog.lpstrDefExt = L"json";
        dialog.lpstrTitle = save ? L"Save RePak build map" : L"Open RePak build map";
        dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        dialog.Flags |= save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST;
        const BOOL completed = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
        return completed ? std::optional<fs::path>(fileName.data()) : std::nullopt;
    }

    [[nodiscard]] std::optional<fs::path> ChooseFirestarProjectFile(const HWND owner, const bool save)
    {
        std::array<wchar_t, 32768> fileName{};
        constexpr wchar_t filter[] = L"Firestar projects (*.fsp)\0*.fsp\0\0";
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = owner;
        dialog.lpstrFilter = filter;
        dialog.lpstrFile = fileName.data();
        dialog.nMaxFile = static_cast<DWORD>(fileName.size());
        dialog.lpstrDefExt = L"fsp";
        dialog.lpstrTitle = save ? L"Save Firestar project" : L"Open Firestar project";
        dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        dialog.Flags |= save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST;
        const BOOL completed = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
        return completed ? std::optional<fs::path>(fileName.data()) : std::nullopt;
    }

    [[nodiscard]] std::optional<fs::path> ChoosePackedProjectFile(const HWND owner, const bool save)
    {
        std::array<wchar_t, 32768> fileName{};
        constexpr wchar_t filter[] = L"Packed Firestar projects (*.fspa)\0*.fspa\0\0";
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = owner;
        dialog.lpstrFilter = filter;
        dialog.lpstrFile = fileName.data();
        dialog.nMaxFile = static_cast<DWORD>(fileName.size());
        dialog.lpstrDefExt = L"fspa";
        dialog.lpstrTitle = save ? L"Pack and export Firestar project" : L"Open packed Firestar project";
        dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        dialog.Flags |= save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST;
        const BOOL completed = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
        return completed ? std::optional<fs::path>(fileName.data()) : std::nullopt;
    }

    [[nodiscard]] std::vector<fs::path> ChooseAnySourceFiles(const HWND owner, const wchar_t* const title,
        const bool allowMultiple)
    {
        std::vector<wchar_t> fileNames(65536, L'\0');
        constexpr wchar_t filter[] =
            L"RPAK source assets (*.dds;*.rmdl;*.rrig;*.rseq;*.json;*.uber;*.msw;*.ruip;*.csv;*.wav;*.ogg;*.anir;*.txan;*.bin)\0*.dds;*.rmdl;*.rrig;*.rseq;*.json;*.uber;*.msw;*.ruip;*.csv;*.wav;*.ogg;*.oga;*.anir;*.txan;*.bin\0"
            L"All files (*.*)\0*.*\0\0";
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = owner;
        dialog.lpstrFilter = filter;
        dialog.lpstrFile = fileNames.data();
        dialog.nMaxFile = static_cast<DWORD>(fileNames.size());
        dialog.lpstrTitle = title;
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
            OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        if (allowMultiple) dialog.Flags |= OFN_ALLOWMULTISELECT;
        if (!GetOpenFileNameW(&dialog)) return {};

        std::vector<fs::path> selected;
        const wchar_t* entry = fileNames.data();
        const fs::path first(entry);
        entry += wcslen(entry) + 1;
        if (*entry == L'\0')
        {
            selected.push_back(first);
            return selected;
        }

        while (*entry != L'\0')
        {
            selected.push_back(first / entry);
            entry += wcslen(entry) + 1;
        }
        return selected;
    }

    [[nodiscard]] std::optional<fs::path> ChooseAnySourceFile(const HWND owner, const wchar_t* const title)
    {
        std::vector<fs::path> selected = ChooseAnySourceFiles(owner, title, false);
        return selected.empty() ? std::nullopt : std::optional<fs::path>(std::move(selected.front()));
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

#pragma pack(push, 1)
    struct FirestarProjectHeader
    {
        char magic[8]{'F', 'I', 'R', 'E', 'S', 'P', '1', '\0'};
        std::uint32_t version{1};
        std::uint32_t flags{};
        std::uint64_t buildBaseBytes{};
        std::uint64_t manifestHintBytes{};
        std::uint64_t jsonBytes{};
    };
#pragma pack(pop)

    constexpr std::uint32_t FirestarProjectBaseRelative = 1u << 0;
    constexpr std::uint32_t FirestarProjectManifestRelative = 1u << 1;
    constexpr std::uint64_t FirestarProjectMaximumStringBytes = 256ull * 1024ull * 1024ull;

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
        // The bundled RSX ImGui backend converts vertex colours from sRGB to
        // linear space and creates detached viewports as scRGB float targets.
        // Keep the host target in the same colour space so an ImGui window has
        // identical brightness before and after it leaves the main viewport.
        description.BufferDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.OutputWindow = window;
        description.SampleDesc.Count = 1;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

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
        enum class WorkspaceMode
        {
            Home,
            RePakProject
        };

        AtlasApplication()
        {
            settings_.outputDirectory = PathToUtf8(GetDefaultOutputDirectory());
            LoadEditorSettings();
        }

        ~AtlasApplication()
        {
            std::string settingsError;
            if (!SaveEditorSettings(settingsError))
                OutputDebugStringA(("Firestar settings save failed: " + settingsError + "\n").c_str());
            if (repakBuildThread_.joinable())
                repakBuildThread_.join();
            ClearAssetPreview();
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
            if (paths.size() == 1 && _wcsicmp(paths.front().extension().c_str(), L".json") == 0)
            {
                LoadRePakProject(paths.front());
                return;
            }
            if (paths.size() == 1 && _wcsicmp(paths.front().extension().c_str(), L".fsp") == 0)
            {
                LoadFirestarProject(paths.front());
                return;
            }
            if (paths.size() == 1 && _wcsicmp(paths.front().extension().c_str(), L".fspa") == 0)
            {
                OpenPackedProject(paths.front());
                return;
            }
            if (paths.size() == 1 && _wcsicmp(paths.front().extension().c_str(), L".fsa") == 0)
            {
                OpenAtlasWindow();
                LoadProjectWithFeedback(paths.front());
                return;
            }
            OpenAtlasWindow();
            AddFiles(paths);
        }

        void OpenProjectFromCommandLine(const fs::path& path)
        {
            OpenAtlasWindow();
            (void)LoadProjectWithFeedback(path);
        }

        void OpenRePakFromCommandLine(const fs::path& path)
        {
            LoadRePakProject(path);
        }

        void OpenFirestarFromCommandLine(const fs::path& path)
        {
            LoadFirestarProject(path);
        }

        void OpenPackedFromCommandLine(const fs::path& path)
        {
            OpenPackedProject(path);
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

            HandleAutosave();

            HandleShortcuts(window);
            DrawMenu(window);
            DrawEditorRoot(window);
            DrawEditorSettings(window);
            DrawRePakSettings();
            DrawModCreator(window);
            DrawAboutWindow();
            DrawAssetTypesReferenceWindow();
            if (!showAtlasWindow_)
                return;
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            if (!mainLayoutInitialized_)
            {
                ImGui::SetNextWindowViewport(viewport->ID);
                ImGui::SetNextWindowPos(viewport->WorkPos + viewport->WorkSize * 0.5f,
                    ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(
                    (std::min)(1120.0f, viewport->WorkSize.x - 80.0f),
                    (std::min)(540.0f, viewport->WorkSize.y - 100.0f)), ImGuiCond_Appearing);
                mainLayoutInitialized_ = true;
            }

            constexpr ImGuiWindowFlags mainFlags = ImGuiWindowFlags_NoCollapse;
            // Merge with the host while the Atlas is inside it so the global
            // File/Atlas/Settings menus remain above the editor. Dear ImGui
            // will still create a normal platform window when it is dragged
            // outside the host viewport.
            ImGuiWindowClass atlasWindowClass{};
            atlasWindowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoTaskBarIcon |
                ImGuiViewportFlags_NoAutoMerge;
            ImGui::SetNextWindowClass(&atlasWindowClass);
            ImGui::SetNextWindowBgAlpha(1.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.070f, 0.074f, 0.084f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.090f, 0.095f, 0.108f, 1.0f));
            ImGui::Begin("UI Atlas", &showAtlasWindow_, mainFlags);
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

        [[nodiscard]] bool CreateProjectSelfTest(const fs::path& outputRoot, std::string& error)
        {
            std::error_code ioError;
            fs::create_directories(outputRoot, ioError);
            if (ioError)
            {
                error = "Unable to create the project self-test directory: " + ioError.message();
                return false;
            }
            firestar::editor::ProjectDocument animationOrderDocument;
            if (!animationOrderDocument.Create(outputRoot / L"animation-order", "animation_order", error))
                return false;
            (void)animationOrderDocument.AddAsset("arig", "animrig/test.rrig");
            (void)animationOrderDocument.AddAsset("aseq", "animseq/test.rseq");
            if (!animationOrderDocument.HasLeadingAnimationAsset())
            {
                error = "The animation-only project did not retain its build warning state.";
                return false;
            }
            const size_t safeAssetIndex = animationOrderDocument.AddAsset("txtr", "texture/test.dds");
            const rapidjson::Value* const safeAsset = animationOrderDocument.Asset(0);
            if (safeAssetIndex != 0 || animationOrderDocument.HasLeadingAnimationAsset() || !safeAsset ||
                AssetMemberString(*safeAsset, "_type") != "txtr")
            {
                error = "Firestar did not move a non-animation asset ahead of leading rigs/sequences.";
                return false;
            }
            (void)animationOrderDocument.AddAsset("matl", "material/test.json");
            if (!animationOrderDocument.RemoveAsset(0))
            {
                error = "The animation-order self-test could not remove its leading asset.";
                return false;
            }
            const rapidjson::Value* const replacementSafeAsset = animationOrderDocument.Asset(0);
            if (animationOrderDocument.HasLeadingAnimationAsset() || !replacementSafeAsset ||
                AssetMemberString(*replacementSafeAsset, "_type") != "matl")
            {
                error = "Removing the leading asset left a rig/sequence ahead of another safe asset.";
                return false;
            }
            constexpr std::string_view legacyAnimationFirstJson = R"json({
                "version": 8,
                "name": "legacy_animation_order",
                "assetsDir": "assets/",
                "outputDir": "build/",
                "files": [
                    { "_type": "aseq", "_path": "animseq/legacy.rseq" },
                    { "_type": "arig", "_path": "animrig/legacy.rrig" },
                    { "_type": "txtr", "_path": "texture/legacy.dds" }
                ]
            })json";
            firestar::editor::ProjectDocument importedOrderDocument;
            if (!importedOrderDocument.LoadSerialized(legacyAnimationFirstJson,
                outputRoot / L"legacy-animation-order.json", outputRoot, error))
                return false;
            const rapidjson::Value* const importedFirstAsset = importedOrderDocument.Asset(0);
            if (!importedOrderDocument.IsDirty() || importedOrderDocument.HasLeadingAnimationAsset() ||
                !importedFirstAsset || AssetMemberString(*importedFirstAsset, "_type") != "txtr")
            {
                error = "Firestar did not migrate an imported animation-first RePak manifest.";
                return false;
            }
            if (!repakProject_.Create(outputRoot, "firestar_project_test", error))
                return false;
            currentRePakProjectPath_.clear();
            const rapidjson::Value& createdProject = repakProject_.Document();
            const auto mandatoryStream = createdProject.FindMember("streamFileMandatory");
            const auto optionalStream = createdProject.FindMember("streamFileOptional");
            if (mandatoryStream == createdProject.MemberEnd() || !mandatoryStream->value.IsString() ||
                std::string_view(mandatoryStream->value.GetString(), mandatoryStream->value.GetStringLength()) !=
                    "paks/Win64/firestar_project_test.starpak" ||
                optionalStream == createdProject.MemberEnd() || !optionalStream->value.IsString() ||
                std::string_view(optionalStream->value.GetString(), optionalStream->value.GetStringLength()) !=
                    "paks/Win64/firestar_project_test.opt.starpak")
            {
                error = "New projects did not receive generated mandatory and optional StarPak paths.";
                return false;
            }
            repakProject_.Document()["name"].SetString("firestar_package_test", repakProject_.Allocator());
            repakProject_.Document()["streamFileMandatory"].SetString(
                "paks/Win64/firestar_package_test.starpak", repakProject_.Allocator());
            repakProject_.Document()["streamFileOptional"].SetString(
                "paks/Win64/firestar_package_test.opt.starpak", repakProject_.Allocator());
            if (FirestarProjectName() != "firestar_project_test" ||
                DefaultFirestarProjectPath().filename() != L"firestar_project_test.fsp" ||
                BuiltPakPath().filename() != L"firestar_package_test.rpak" ||
                std::string_view(repakProject_.Document()["streamFileMandatory"].GetString()) !=
                    "paks/Win64/firestar_package_test.starpak" ||
                std::string_view(repakProject_.Document()["streamFileOptional"].GetString()) !=
                    "paks/Win64/firestar_package_test.opt.starpak")
            {
                error = "The Firestar project name is still coupled to the RPAK package name.";
                return false;
            }
            const size_t sourceIndex = repakProject_.AddAsset("awsr", "audio/test_source.rpak");
            rapidjson::Value* source = repakProject_.Asset(sourceIndex);
            if (!source) { error = "Could not create the audio source test asset."; return false; }
            source->AddMember("wav", "audio/test_source.wav", repakProject_.Allocator());
            const fs::path audioPath = repakProject_.AssetsDirectory() / L"audio" / L"test_source.wav";
            fs::create_directories(audioPath.parent_path(), ioError);
            std::ofstream audio(audioPath, std::ios::binary | std::ios::trunc);
            constexpr std::uint32_t frameCount = 32000;
            constexpr std::uint32_t dataSize = frameCount * sizeof(std::int16_t);
            const std::uint32_t riffSize = 36 + dataSize;
            const std::uint32_t formatSize = 16;
            const std::uint16_t pcm = 1;
            const std::uint16_t channels = 1;
            const std::uint32_t sampleRate = 16000;
            const std::uint32_t byteRate = 32000;
            const std::uint16_t blockAlign = 2;
            const std::uint16_t bits = 16;
            std::vector<std::int16_t> tone(frameCount);
            for (std::uint32_t frame = 0; frame < frameCount; ++frame)
            {
                const double phase = 2.0 * 3.14159265358979323846 * 440.0 * frame / sampleRate;
                tone[frame] = static_cast<std::int16_t>(std::sin(phase) * 10000.0);
            }
            audio.write("RIFF", 4); audio.write(reinterpret_cast<const char*>(&riffSize), 4); audio.write("WAVE", 4);
            audio.write("fmt ", 4); audio.write(reinterpret_cast<const char*>(&formatSize), 4);
            audio.write(reinterpret_cast<const char*>(&pcm), 2); audio.write(reinterpret_cast<const char*>(&channels), 2);
            audio.write(reinterpret_cast<const char*>(&sampleRate), 4); audio.write(reinterpret_cast<const char*>(&byteRate), 4);
            audio.write(reinterpret_cast<const char*>(&blockAlign), 2); audio.write(reinterpret_cast<const char*>(&bits), 2);
            audio.write("data", 4); audio.write(reinterpret_cast<const char*>(&dataSize), 4);
            audio.write(reinterpret_cast<const char*>(tone.data()), static_cast<std::streamsize>(dataSize));
            audio.close();
            const auto resolvedAudio = repakProject_.ResolvePrimarySource(sourceIndex);
            if (!resolvedAudio || !fs::is_regular_file(*resolvedAudio) ||
                _wcsicmp(resolvedAudio->c_str(), audioPath.c_str()) != 0)
            {
                error = "The audio-source resolver did not honor the manifest wav field.";
                return false;
            }
            const size_t eventIndex = repakProject_.AddAsset("aevt", "audio/test_event.rpak");
            if (!repakProject_.AppendStringToAssetArray(eventIndex, "sources", "audio/test_source.rpak"))
            {
                error = "Could not create the audio event test reference.";
                return false;
            }
            const size_t modelIndex = repakProject_.AddAsset("mdl_", "models/test_model.rmdl");
            const size_t rigIndex = repakProject_.AddAsset("arig", "animrig/test_rig.rrig");
            if (AssetMemberString(*repakProject_.Asset(modelIndex), "_path") != "models/test_model.rpak" ||
                AssetMemberString(*repakProject_.Asset(rigIndex), "_path") != "animrig/test_rig.rpak")
            {
                error = "Asset paths were not normalized to .rpak names.";
                return false;
            }
            if (!repakProject_.AppendStringToAssetArray(modelIndex, "$animrigs", "models/test_model.rrig") ||
                !repakProject_.AppendStringToAssetArray(modelIndex, "$sequences", "models/test_model.rseq") ||
                !repakProject_.AppendStringToAssetArray(rigIndex, "$sequences", "animrig/test_rig.rseq"))
            {
                error = "Could not create the model, rig, and sequence test references.";
                return false;
            }
            const std::string expectedJson = repakProject_.SerializeDocument();
            const fs::path projectPath = outputRoot / L"firestar_project_test.fsp";
            if (!SaveFirestarProjectTo(projectPath))
            {
                error = "The Firestar project container could not be written.";
                return false;
            }
            if (!fs::is_directory(outputRoot / L"assets") || !fs::is_directory(outputRoot / L"build"))
            {
                error = "The normal project save did not keep the .fsp beside assets and build.";
                return false;
            }

            AtlasApplication reopened;
            if (!reopened.LoadFirestarProject(projectPath))
            {
                error = reopened.editorError_.empty() ? "The Firestar project could not be reopened." : reopened.editorError_;
                return false;
            }
            if (reopened.repakProject_.SerializeDocument() != expectedJson)
            {
                error = "The JSON embedded in the reopened Firestar project changed.";
                return false;
            }
            if (reopened.FirestarProjectName() != "firestar_project_test" ||
                reopened.repakProject_.Name() != "firestar_package_test")
            {
                error = "The reopened project did not keep separate project and RPAK package names.";
                return false;
            }
            if (!reopened.repakProject_.ExportJson(outputRoot / L"exported_repak.json", error))
                return false;

            sprites_.clear();
            settings_.maxAtlasSize = 64;
            settings_.autoSelectSize = true;
            settings_.padding = 2;
            AddGeneratedSprite("rui/firestar_test/import", 16, 16, {255, 128, 32, 255});
            if (!Pack(error)) return false;
            const fs::path importSource = outputRoot / L"generic_import.dds";
            if (!WriteDds(importSource, error) || !CommitImportSource(importSource, "txtr"))
            {
                if (error.empty()) error = "The generic texture import self-test failed.";
                return false;
            }
            const rapidjson::Value* importedAsset = repakProject_.Asset(static_cast<size_t>(selectedAsset_));
            const std::string importedPath = importedAsset ? AssetMemberString(*importedAsset, "_path") : std::string{};
            const bool streamingDefaultIsFalse = importedAsset && importedAsset->HasMember("$disableStreaming") &&
                (*importedAsset)["$disableStreaming"].IsBool() && !(*importedAsset)["$disableStreaming"].GetBool();
            if (!importedAsset || importedPath.starts_with("imported/") || !streamingDefaultIsFalse)
            {
                error = "Generic texture import path or streaming defaults are incorrect.";
                return false;
            }

            firestar::editor::ProjectDocument buildDocument;
            if (!buildDocument.Create(outputRoot / L"embedded-build", "firestar_embedded_build_test", error))
                return false;
            firestar::repak::BuildRequest buildRequest;
            buildRequest.manifestPath = buildDocument.ManifestPath();
            buildRequest.manifestBaseDirectory = buildDocument.BuildBaseDirectory();
            buildRequest.manifestJson = buildDocument.SerializeDocument();
            const firestar::repak::BuildResult buildResult = firestar::repak::Build(buildRequest);
            if (!buildResult.succeeded)
            {
                error = "Embedded RePak build failed: " + buildResult.message;
                return false;
            }
            return true;
        }

        [[nodiscard]] bool CreateArchiveSelfTest(const fs::path& outputRoot, std::string& error)
        {
            std::error_code ioError;
            const fs::path workspace = outputRoot / L"archive-workspace";
            const fs::path extraction = outputRoot / L"archive-extracted";
            fs::remove_all(workspace, ioError);
            ioError.clear();
            fs::remove_all(extraction, ioError);
            ioError.clear();
            fs::create_directories(outputRoot, ioError);
            if (ioError || !repakProject_.Create(workspace, "firestar_archive_test", error))
            {
                if (error.empty()) error = "Unable to create the archive self-test workspace: " + ioError.message();
                return false;
            }
            const fs::path testAsset = repakProject_.AssetsDirectory() / L"models" / L"archive_test.rmdl";
            fs::create_directories(testAsset.parent_path(), ioError);
            std::ofstream assetOutput(testAsset, std::ios::binary | std::ios::trunc);
            if (!assetOutput.is_open())
            {
                error = "Unable to open the archive self-test asset " + PathToUtf8(testAsset) +
                    ": " + std::strerror(errno);
                return false;
            }
            constexpr std::array<std::uint8_t, 12> TestBytes{0x49, 0x44, 0x53, 0x54, 1, 2, 3, 4, 5, 6, 7, 8};
            assetOutput.write(reinterpret_cast<const char*>(TestBytes.data()), TestBytes.size());
            const bool assetWritten = assetOutput.good();
            assetOutput.close();
            if (ioError || !assetWritten)
            {
                error = "Unable to create the archive self-test asset.";
                return false;
            }
            (void)repakProject_.AddAsset("mdl_", "models/archive_test.rpak");
            const fs::path archive = outputRoot / L"firestar_archive_test.fspa";
            if (!PackAndExportProjectTo(archive, error))
                return false;
            fs::path extractedProject;
            if (!firestar::editor::ExtractPackedProject(archive, extraction, extractedProject, error))
                return false;
            if (!fs::is_regular_file(extractedProject) ||
                !fs::is_regular_file(extraction / L"assets" / L"models" / L"archive_test.rmdl") ||
                !fs::is_directory(extraction / L"build"))
            {
                error = "The packed project did not recreate its .fsp, assets, and build folder.";
                return false;
            }
            AtlasApplication reopened;
            if (!reopened.LoadFirestarProject(extractedProject))
            {
                error = reopened.editorError_.empty() ? "The extracted project could not be opened." : reopened.editorError_;
                return false;
            }
            const bool assetsMatch = fs::equivalent(reopened.repakProject_.AssetsDirectory(), extraction / L"assets", ioError);
            ioError.clear();
            const bool buildMatches = fs::equivalent(reopened.repakProject_.OutputDirectory(), extraction / L"build", ioError);
            if (!assetsMatch || !buildMatches || ioError)
            {
                error = "The extracted project did not resolve its portable folders. Assets=" +
                    PathToUtf8(reopened.repakProject_.AssetsDirectory()) + ", expected=" +
                    PathToUtf8((extraction / L"assets").lexically_normal()) + ", build=" +
                    PathToUtf8(reopened.repakProject_.OutputDirectory()) + ", expected=" +
                    PathToUtf8((extraction / L"build").lexically_normal());
                return false;
            }
            fs::remove(outputRoot / L"archive-self-test-error.txt", ioError);
            return true;
        }

    private:
        struct BuildLogEntry
        {
            firestar::repak::LogLevel level{};
            std::string message;
        };

        struct PendingImport
        {
            fs::path source;
            std::string type;
            fs::path preferredRelativePath;
            firestar::editor::AssetCompatibility compatibility;
        };

        struct PendingReplacement
        {
            fs::path source;
            fs::path target;
            size_t assetIndex{};
            firestar::editor::AssetCompatibility compatibility;
        };

        struct StagedSourceImport
        {
            fs::path source;
            std::string type;
            std::string relativePath;
            firestar::editor::AssetCompatibility compatibility;
        };

        struct AssetDependency
        {
            std::string kind;
            std::string name;
            std::uint64_t guid{};
            int assetIndex{-1};
        };

        struct AudioEventPreviewChoice
        {
            std::string reference;
            int assetIndex{-1};
        };

        Settings settings_{};
        WorkspaceMode workspaceMode_{WorkspaceMode::Home};
        bool showAtlasWindow_{};
        bool showModCreatorWindow_{};
        firestar::editor::ProjectDocument repakProject_;
        fs::path currentRePakProjectPath_;
        bool showHomeSavePrompt_{};
        int selectedAsset_{-1};
        std::array<char, 256> assetSearch_{};
        int newAssetTypeIndex_{};
        std::array<char, 1024> newAssetPath_{};
        std::vector<PendingImport> pendingImports_;
        std::vector<StagedSourceImport> stagedSourceImports_;
        bool importIncompatibleSources_{};
        std::string importSummary_;
        std::optional<PendingReplacement> pendingReplacement_;
        int pendingDeleteAsset_{-1};
        std::vector<fs::path> pendingDeleteSourceFiles_;
        bool openDeleteAssetPopup_{};
        std::vector<fs::path> nearbyAssetChoices_;
        std::vector<std::string> projectAssetReferenceChoices_;
        std::array<char, 256> assetReferenceSearch_{};
        std::string nearbyTargetField_;
        size_t assetReferenceTargetIndex_{};
        bool openAssetReferencePopup_{};
        fs::path loadedSourcePath_;
        std::vector<std::uint8_t> loadedSourceBytes_;
        std::string sourceLoadError_;
        fs::path assetPreviewPath_;
        ID3D11ShaderResourceView* assetPreviewTexture_{};
        UINT assetPreviewWidth_{};
        UINT assetPreviewHeight_{};
        std::string assetPreviewFormat_;
        std::string assetPreviewError_;
        firestar::editor::AudioPreviewPlayer audioPreview_;
        std::vector<AudioEventPreviewChoice> audioEventPreviewChoices_;
        int audioPreviewOwnerAsset_{-1};
        int audioPreviewSourceAsset_{-1};
        int audioEventPreviewChoice_{};
        std::string audioPreviewError_;
        firestar::editor::RmdlPreviewData modelPreview_;
        std::vector<ModelPreviewMaterialTexture> modelPreviewMaterials_;
        ComPtr<ID3D11ShaderResourceView> modelMissingTexture_;
        int modelPreviewAssetIndex_{-1};
        size_t modelPreviewLod_{};
        size_t modelPreviewSelectedBodyPart_{};
        std::vector<size_t> modelPreviewBodySelections_;
        float modelPreviewYaw_{0.55f};
        float modelPreviewPitch_{-0.25f};
        float modelPreviewZoom_{1.0f};
        std::thread repakBuildThread_;
        std::atomic<bool> repakBuilding_{false};
        std::mutex repakBuildMutex_;
        std::vector<BuildLogEntry> repakBuildLog_;
        std::string repakBuildResult_;
        bool repakBuildSucceeded_{};
        bool showEditorSettings_{};
        bool showRePakSettings_{};
        bool autosaveEnabled_{true};
        int autosaveMinutes_{2};
        double lastAutosaveTime_{};
        bool showAbout_{};
        bool showAssetTypesReference_{};
        std::array<char, 256> assetTypesReferenceSearch_{};
        std::string assetTypesReferenceMarkdown_;
        std::vector<std::string> assetTypesReferenceLines_;
        struct AssetTypesReferenceSection
        {
            std::string title;
            size_t firstLine{};
            size_t lastLine{};
            std::string searchableText;
        };
        std::vector<AssetTypesReferenceSection> assetTypesReferenceSections_;
        int assetTypesReferenceSelectedSection_{};
        int assetTypesReferenceScrollTarget_{-1};
        std::string editorError_;
        bool showEditorError_{};
        std::optional<fs::path> pendingPackedProjectPath_;
        bool showPackedProjectPrompt_{};
        std::string apexInstallPath_;
        bool deployAsMod_{true};
        std::string deployModId_{"firestar_mod"};
        bool showDeployConfirm_{};
        std::string deployError_;
        std::string modName_{"FirestarMod"};
        std::string modId_{"firestar_mod"};
        std::string modDescription_{"Created with Firestar"};
        std::string modVersion_{"1.0.0"};
        std::string modAuthor_;
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

        static std::string AssetMemberString(const rapidjson::Value& object, const char* const name)
        {
            if (!object.IsObject())
                return {};
            const auto member = object.FindMember(name);
            return member != object.MemberEnd() && member->value.IsString()
                ? std::string(member->value.GetString(), member->value.GetStringLength())
                : std::string{};
        }

        static std::uint64_t AssetGuid(const rapidjson::Value& asset)
        {
            if (!asset.IsObject()) return 0;
            const auto overrideGuid = asset.FindMember("$guid");
            if (overrideGuid != asset.MemberEnd())
            {
                if (overrideGuid->value.IsUint64()) return overrideGuid->value.GetUint64();
                if (overrideGuid->value.IsInt64() && overrideGuid->value.GetInt64() > 0)
                    return static_cast<std::uint64_t>(overrideGuid->value.GetInt64());
                if (overrideGuid->value.IsString())
                {
                    try
                    {
                        return std::stoull(overrideGuid->value.GetString(), nullptr, 0);
                    }
                    catch (...) {}
                }
            }
            const std::string path = AssetMemberString(asset, "_path");
            return path.empty() ? 0 : RTech::StringToGuid(path.c_str());
        }

        int FindProjectAssetByGuid(const std::uint64_t guid) const
        {
            if (guid == 0) return -1;
            for (size_t index = 0; index < repakProject_.AssetCount(); ++index)
            {
                const rapidjson::Value* candidate = repakProject_.Asset(index);
                if (candidate && AssetGuid(*candidate) == guid)
                    return static_cast<int>(index);
            }
            return -1;
        }

        std::vector<AssetDependency> SelectedAssetDependencies() const
        {
            std::vector<AssetDependency> dependencies;
            if (selectedAsset_ < 0) return dependencies;
            const rapidjson::Value* asset = repakProject_.Asset(static_cast<size_t>(selectedAsset_));
            if (!asset || !asset->IsObject()) return dependencies;

            const auto appendPathArray = [this, &dependencies, asset](const char* field, const char* kind) {
                const auto member = asset->FindMember(field);
                if (member == asset->MemberEnd() || !member->value.IsArray()) return;
                for (const rapidjson::Value& value : member->value.GetArray())
                {
                    if (!value.IsString()) continue;
                    AssetDependency dependency;
                    dependency.kind = kind;
                    dependency.name.assign(value.GetString(), value.GetStringLength());
                    dependency.guid = RTech::StringToGuid(dependency.name.c_str());
                    dependency.assetIndex = FindProjectAssetByGuid(dependency.guid);
                    dependencies.push_back(std::move(dependency));
                }
            };
            appendPathArray("$animrigs", "Animation rig");
            appendPathArray("$sequences", "Sequence");
            appendPathArray("$materials", "Material");
            appendPathArray("$textures", "Texture");
            appendPathArray("sources", "Audio source");

            const std::string type = AssetMemberString(*asset, "_type");
            if (type == "matl" && !loadedSourceBytes_.empty() &&
                ToLowerAscii(loadedSourcePath_.extension().string()) == ".json")
            {
                rapidjson::Document material;
                material.Parse(reinterpret_cast<const char*>(loadedSourceBytes_.data()), loadedSourceBytes_.size());
                if (material.IsObject())
                {
                    const auto textures = material.FindMember("$textures");
                    if (textures != material.MemberEnd() && (textures->value.IsArray() || textures->value.IsObject()))
                    {
                        const auto appendTexture = [this, &dependencies](const std::string& slot,
                            const rapidjson::Value& value)
                        {
                            if (!value.IsString()) return;
                            AssetDependency dependency;
                            dependency.kind = "Texture slot " + slot;
                            dependency.name.assign(value.GetString(), value.GetStringLength());
                            dependency.guid = RTech::StringToGuid(dependency.name.c_str());
                            dependency.assetIndex = FindProjectAssetByGuid(dependency.guid);
                            dependencies.push_back(std::move(dependency));
                        };
                        if (textures->value.IsArray())
                        {
                            for (rapidjson::SizeType slot = 0; slot < textures->value.Size(); ++slot)
                                appendTexture(std::to_string(slot), textures->value[slot]);
                        }
                        else
                        {
                            for (auto texture = textures->value.MemberBegin(); texture != textures->value.MemberEnd(); ++texture)
                                appendTexture(std::string(texture->name.GetString(), texture->name.GetStringLength()), texture->value);
                        }
                    }
                }
            }
            if (type == "mdl_" && loadedSourceBytes_.size() >= 216 &&
                std::memcmp(loadedSourceBytes_.data(), "IDST", 4) == 0)
            {
                const auto read32 = [this](const size_t offset) {
                    std::uint32_t value{};
                    if (offset + sizeof(value) <= loadedSourceBytes_.size())
                        std::memcpy(&value, loadedSourceBytes_.data() + offset, sizeof(value));
                    return value;
                };
                const auto read64 = [this](const size_t offset) {
                    std::uint64_t value{};
                    if (offset + sizeof(value) <= loadedSourceBytes_.size())
                        std::memcpy(&value, loadedSourceBytes_.data() + offset, sizeof(value));
                    return value;
                };
                const std::uint32_t materialCount = read32(208);
                const std::uint32_t materialOffset = read32(212);
                if (materialCount <= 4096 && materialOffset <= loadedSourceBytes_.size())
                {
                    for (std::uint32_t index = 0; index < materialCount; ++index)
                    {
                        const size_t entry = static_cast<size_t>(materialOffset) + index * 12;
                        if (entry + 12 > loadedSourceBytes_.size()) break;
                        const std::uint32_t relativeNameOffset = read32(entry);
                        const std::uint64_t guid = read64(entry + 4);
                        if (guid == 0) continue;
                        AssetDependency dependency;
                        dependency.kind = "Material slot " + std::to_string(index);
                        dependency.guid = guid;
                        const std::uint64_t nameOffset = static_cast<std::uint64_t>(entry) + relativeNameOffset;
                        if (relativeNameOffset != 0 && nameOffset < loadedSourceBytes_.size())
                        {
                            const char* text = reinterpret_cast<const char*>(loadedSourceBytes_.data() + nameOffset);
                            const size_t available = loadedSourceBytes_.size() - static_cast<size_t>(nameOffset);
                            if (const void* terminator = std::memchr(text, '\0', available))
                                dependency.name.assign(text, static_cast<const char*>(terminator));
                        }
                        dependency.assetIndex = FindProjectAssetByGuid(guid);
                        if (dependency.assetIndex >= 0)
                        {
                            const rapidjson::Value* projectAsset = repakProject_.Asset(
                                static_cast<size_t>(dependency.assetIndex));
                            dependency.name = projectAsset ? AssetMemberString(*projectAsset, "_path") : std::string{};
                        }
                        if (dependency.name.empty()) dependency.name = "External material";
                        dependencies.push_back(std::move(dependency));
                    }
                }
            }
            return dependencies;
        }

        void LoadEditorSettings()
        {
            std::ifstream input(GetUserSettingsPath(), std::ios::binary);
            if (!input) return;
            rapidjson::IStreamWrapper stream(input);
            rapidjson::Document document;
            document.ParseStream(stream);
            if (!document.IsObject()) return;
            const auto apexPath = document.FindMember("apexInstallPath");
            if (apexPath != document.MemberEnd() && apexPath->value.IsString())
                apexInstallPath_.assign(apexPath->value.GetString(), apexPath->value.GetStringLength());
            const auto asMod = document.FindMember("deployAsMod");
            if (asMod != document.MemberEnd() && asMod->value.IsBool())
                deployAsMod_ = asMod->value.GetBool();
            const auto modId = document.FindMember("deployModId");
            if (modId != document.MemberEnd() && modId->value.IsString())
                deployModId_.assign(modId->value.GetString(), modId->value.GetStringLength());
            const auto autosave = document.FindMember("autosaveEnabled");
            if (autosave != document.MemberEnd() && autosave->value.IsBool())
                autosaveEnabled_ = autosave->value.GetBool();
            const auto autosaveMinutes = document.FindMember("autosaveMinutes");
            if (autosaveMinutes != document.MemberEnd() && autosaveMinutes->value.IsInt())
                autosaveMinutes_ = (std::clamp)(autosaveMinutes->value.GetInt(), 1, 60);
        }

        [[nodiscard]] bool SaveEditorSettings(std::string& error) const
        {
            const fs::path path = GetUserSettingsPath();
            std::error_code ioError;
            fs::create_directories(path.parent_path(), ioError);
            if (ioError)
            {
                error = "Unable to create the Firestar settings folder: " + ioError.message();
                return false;
            }
            rapidjson::Document document(rapidjson::kObjectType);
            auto& allocator = document.GetAllocator();
            document.AddMember("apexInstallPath", rapidjson::Value(apexInstallPath_.c_str(), allocator), allocator);
            document.AddMember("deployAsMod", deployAsMod_, allocator);
            document.AddMember("deployModId", rapidjson::Value(deployModId_.c_str(), allocator), allocator);
            document.AddMember("autosaveEnabled", autosaveEnabled_, allocator);
            document.AddMember("autosaveMinutes", autosaveMinutes_, allocator);
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                error = "Unable to write " + path.string();
                return false;
            }
            rapidjson::OStreamWrapper stream(output);
            rapidjson::PrettyWriter<rapidjson::OStreamWrapper> writer(stream);
            writer.SetIndent(' ', 2);
            document.Accept(writer);
            output << '\n';
            if (!output.good())
            {
                error = "The Firestar settings write was incomplete.";
                return false;
            }
            return true;
        }

        [[nodiscard]] fs::path BuiltPakPath() const
        {
            return repakProject_.BuildOutputDirectory() / PathFromUtf8(repakProject_.Name() + ".rpak");
        }

        [[nodiscard]] std::string FirestarProjectName() const
        {
            if (!repakProject_.IsOpen()) return {};
            const fs::path identityPath = currentRePakProjectPath_.empty()
                ? repakProject_.ManifestPath() : currentRePakProjectPath_;
            const std::string name = PathToUtf8(identityPath.stem());
            return name.empty() ? FileStemSafe(repakProject_.Name()) : name;
        }

        void EnsureGeneratedStreamPaths()
        {
            if (!repakProject_.IsOpen()) return;
            rapidjson::Document& document = repakProject_.Document();
            auto& allocator = document.GetAllocator();
            const std::string packageName = repakProject_.Name();
            if (packageName.empty()) return;
            const std::string projectName = FirestarProjectName();
            bool changed{};
            const auto ensure = [&document, &allocator, &changed](const char* field,
                const std::string& value, const std::string& previousGeneratedValue) {
                auto member = document.FindMember(field);
                if (member != document.MemberEnd() && member->value.IsString() &&
                    member->value.GetStringLength() != 0)
                {
                    const std::string_view current(member->value.GetString(), member->value.GetStringLength());
                    if (current == value || current != previousGeneratedValue) return;
                }
                if (member != document.MemberEnd()) member->value.SetString(value.c_str(), allocator);
                else document.AddMember(rapidjson::Value(field, allocator),
                    rapidjson::Value(value.c_str(), allocator), allocator);
                changed = true;
            };
            ensure("streamFileMandatory", DefaultMandatoryStarPakPath(packageName),
                DefaultMandatoryStarPakPath(projectName));
            if (repakProject_.PakVersion() >= 8)
                ensure("streamFileOptional", DefaultOptionalStarPakPath(packageName),
                    DefaultOptionalStarPakPath(projectName));
            if (!changed) return;
            repakProject_.RefreshDerivedPaths();
            repakProject_.MarkDirty();
        }

        [[nodiscard]] std::vector<fs::path> BuiltProducts() const
        {
            std::vector<fs::path> products{BuiltPakPath()};
            for (const fs::path& streamPath : repakProject_.StreamOutputPaths())
                if (fs::is_regular_file(streamPath)) products.push_back(streamPath);
            return products;
        }

        [[nodiscard]] bool ValidDeployModId() const
        {
            return !deployModId_.empty() && deployModId_ != "." && deployModId_ != ".." &&
                std::all_of(deployModId_.begin(), deployModId_.end(), [](const unsigned char character) {
                    return std::isalnum(character) || character == '_' || character == '-' || character == '.';
                });
        }

        [[nodiscard]] fs::path DeploymentDirectory() const
        {
            const fs::path apex = PathFromUtf8(apexInstallPath_);
            return deployAsMod_
                ? apex / L"mods" / PathFromUtf8(deployModId_) / L"paks" / L"Win64"
                : apex / L"paks" / L"Win64";
        }

        [[nodiscard]] bool DeployCurrentPak(std::string& error)
        {
            const fs::path apex = PathFromUtf8(apexInstallPath_);
            const fs::path source = BuiltPakPath();
            if (!fs::is_regular_file(source))
            {
                error = "Build the current RPAK before deploying it.";
                return false;
            }
            if (!fs::is_directory(apex) || !fs::is_regular_file(apex / L"r5apex.exe"))
            {
                error = "Choose an Apex install folder containing r5apex.exe in Settings.";
                return false;
            }
            if (deployAsMod_ && !ValidDeployModId())
            {
                error = "The deployment mod ID is invalid.";
                return false;
            }
            if (deployAsMod_ && !fs::is_regular_file(apex / L"mods" / PathFromUtf8(deployModId_) / L"mod.vdf"))
            {
                error = "The selected mod does not contain mod.vdf. Create the mod first or correct its ID in Settings.";
                return false;
            }

            const fs::path destinationDirectory = DeploymentDirectory();
            const fs::path preload = destinationDirectory / L"preload.rson";
            std::string preloadText;
            if (fs::is_regular_file(preload))
            {
                std::ifstream input(preload, std::ios::binary);
                preloadText.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
                if (!input.good() && !input.eof())
                {
                    error = "Unable to read " + preload.string();
                    return false;
                }
            }

            const std::string pakName = PathToUtf8(source.filename());
            bool alreadyListed = false;
            std::istringstream lines(preloadText);
            for (std::string line; std::getline(lines, line);)
            {
                const size_t comment = line.find("//");
                if (comment != std::string::npos) line.erase(comment);
                const size_t first = line.find_first_not_of(" \t\r");
                const size_t last = line.find_last_not_of(" \t\r");
                if (first != std::string::npos && line.substr(first, last - first + 1) == pakName)
                {
                    alreadyListed = true;
                    break;
                }
            }
            if (preloadText.empty())
                preloadText = "Paks:\r\n[\r\n    " + pakName + "\r\n]\r\n";
            else if (!alreadyListed)
            {
                const size_t closingBracket = preloadText.find_last_of(']');
                if (closingBracket == std::string::npos)
                {
                    error = "The target preload.rson has no closing Paks array bracket.";
                    return false;
                }
                const std::string newline = preloadText.find("\r\n") != std::string::npos ? "\r\n" : "\n";
                preloadText.insert(closingBracket, "    " + pakName + newline);
            }

            std::error_code ioError;
            fs::create_directories(destinationDirectory, ioError);
            if (ioError)
            {
                error = "Unable to create the deployment folder: " + ioError.message();
                return false;
            }
            const std::vector<fs::path> products = BuiltProducts();
            for (const fs::path& product : products)
            {
                const fs::path destination = destinationDirectory / product.filename();
                ioError.clear();
                if (fs::is_regular_file(destination))
                    fs::copy_file(destination, fs::path(destination.wstring() + L".firestar.bak"),
                        fs::copy_options::overwrite_existing, ioError);
                if (!ioError)
                    fs::copy_file(product, destination, fs::copy_options::overwrite_existing, ioError);
                if (ioError)
                {
                    error = "Unable to deploy " + PathToUtf8(product.filename()) + ": " + ioError.message();
                    return false;
                }
                AppendRePakLog(firestar::repak::LogLevel::Info, "Deployed " + PathToUtf8(destination));
            }
            if (fs::is_regular_file(preload))
                fs::copy_file(preload, fs::path(preload.wstring() + L".firestar.bak"),
                    fs::copy_options::overwrite_existing, ioError);
            if (ioError)
            {
                error = "Unable to back up preload.rson: " + ioError.message();
                return false;
            }
            std::ofstream output(preload, std::ios::binary | std::ios::trunc);
            output.write(preloadText.data(), static_cast<std::streamsize>(preloadText.size()));
            if (!output.good())
            {
                error = "Unable to update " + preload.string();
                return false;
            }
            AppendRePakLog(firestar::repak::LogLevel::Info, "Preload entry: " + pakName);
            return true;
        }

        void AppendRePakLog(const firestar::repak::LogLevel level, std::string message)
        {
            while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
                message.pop_back();
            std::lock_guard<std::mutex> lock(repakBuildMutex_);
            repakBuildLog_.push_back({level, std::move(message)});
            if (repakBuildLog_.size() > 1000)
                repakBuildLog_.erase(repakBuildLog_.begin(), repakBuildLog_.begin() + 200);
        }

        void OpenAtlasWindow()
        {
            showAtlasWindow_ = true;
            mainLayoutInitialized_ = false;
        }

        bool LoadRePakProject(const fs::path& path)
        {
            std::string error;
            if (!repakProject_.Load(path, error))
            {
                AppendRePakLog(firestar::repak::LogLevel::Error, error);
                editorError_ = error;
                showEditorError_ = true;
                return false;
            }
            currentRePakProjectPath_.clear();
            EnsureGeneratedStreamPaths();
            workspaceMode_ = WorkspaceMode::RePakProject;
            selectedAsset_ = repakProject_.AssetCount() ? 0 : -1;
            loadedSourcePath_.clear();
            loadedSourceBytes_.clear();
            sourceLoadError_.clear();
            editorError_.clear();
            pendingImports_.clear();
            importSummary_.clear();
            RefreshSourceData();
            AppendRePakLog(firestar::repak::LogLevel::Info,
                "Opened " + PathToUtf8(repakProject_.BuildManifestPath()));
            if (repakProject_.IsBuildList())
                AppendRePakLog(firestar::repak::LogLevel::Info,
                    "Editing " + PathToUtf8(repakProject_.ManifestPath()));
            return true;
        }

        void CreateRePakProject(const HWND window)
        {
            const auto root = ChooseFolder(window, L"Choose new RPAK project folder");
            if (!root)
                return;
            std::string error;
            const std::string name = FileStemSafe(PathToUtf8(root->filename()));
            if (!repakProject_.Create(*root, name, error))
            {
                AppendRePakLog(firestar::repak::LogLevel::Error, error);
                return;
            }
            currentRePakProjectPath_.clear();
            workspaceMode_ = WorkspaceMode::RePakProject;
            selectedAsset_ = -1;
            AppendRePakLog(firestar::repak::LogLevel::Info,
                "Created project workspace " + PathToUtf8(root->filename()));
        }

        fs::path DefaultFirestarProjectPath() const
        {
            if (!repakProject_.IsOpen()) return {};
            return repakProject_.BuildBaseDirectory() /
                PathFromUtf8(FileStemSafe(FirestarProjectName()) + ".fsp");
        }

        void SaveAfterAssetAdded()
        {
            if (!autosaveEnabled_ || !repakProject_.IsOpen() || repakBuilding_) return;
            const fs::path destination = currentRePakProjectPath_.empty()
                ? DefaultFirestarProjectPath() : currentRePakProjectPath_;
            if (!destination.empty())
                static_cast<void>(SaveFirestarProjectTo(destination));
        }

        bool SaveFirestarProject(const HWND window, const bool saveAs = false)
        {
            if (!repakProject_.IsOpen())
                return false;
            fs::path destination = currentRePakProjectPath_;
            if (!saveAs && destination.empty())
                destination = DefaultFirestarProjectPath();
            if (saveAs || destination.empty())
            {
                const auto chosen = ChooseFirestarProjectFile(window, true);
                if (!chosen) return false;
                destination = *chosen;
            }
            return SaveFirestarProjectTo(destination);
        }

        bool BuildFirestarProjectBytes(const fs::path& destination, const bool portable,
            std::vector<std::uint8_t>& bytes, std::string& error) const
        {
            bytes.clear();
            error.clear();
            if (!repakProject_.IsOpen())
            {
                error = "There is no Firestar project to save.";
                return false;
            }

            fs::path baseToStore = repakProject_.BuildBaseDirectory();
            fs::path manifestToStore = repakProject_.ManifestPath();
            std::uint32_t flags = 0;
            std::string json = repakProject_.SerializeDocument();
            if (portable)
            {
                rapidjson::Document document;
                document.Parse(json.c_str(), json.size());
                if (!document.IsObject())
                {
                    error = "The in-memory RePak JSON could not be prepared for packing.";
                    return false;
                }
                auto& allocator = document.GetAllocator();
                const auto setPath = [&document, &allocator](const char* name, const char* value) {
                    const auto member = document.FindMember(name);
                    if (member != document.MemberEnd()) member->value.SetString(value, allocator);
                    else document.AddMember(rapidjson::Value(name, allocator), rapidjson::Value(value, allocator), allocator);
                };
                setPath("assetsDir", "assets/");
                setPath("outputDir", "build/");
                rapidjson::StringBuffer buffer;
                rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
                writer.SetIndent(' ', 4);
                document.Accept(writer);
                json.assign(buffer.GetString(), buffer.GetSize());
                baseToStore = L".";
                manifestToStore = PathFromUtf8(FileStemSafe(repakProject_.Name()) + ".json");
                flags = FirestarProjectBaseRelative | FirestarProjectManifestRelative;
            }
            else
            {
                const fs::path absoluteDestination = fs::absolute(destination).lexically_normal();
                const fs::path destinationDirectory = absoluteDestination.parent_path();
                std::error_code relativeError;
                fs::path relativeBase = fs::relative(baseToStore, destinationDirectory, relativeError);
                if (!relativeError && !relativeBase.empty() && *relativeBase.begin() != L"..")
                {
                    baseToStore = relativeBase;
                    flags |= FirestarProjectBaseRelative;
                }
                relativeError.clear();
                fs::path relativeManifest = fs::relative(manifestToStore,
                    repakProject_.BuildBaseDirectory(), relativeError);
                if (!relativeError && !relativeManifest.empty() && *relativeManifest.begin() != L"..")
                {
                    manifestToStore = relativeManifest;
                    flags |= FirestarProjectManifestRelative;
                }
            }

            const std::string buildBase = PathToUtf8(baseToStore);
            const std::string manifestHint = PathToUtf8(manifestToStore);
            if (buildBase.size() > FirestarProjectMaximumStringBytes ||
                manifestHint.size() > FirestarProjectMaximumStringBytes ||
                json.size() > FirestarProjectMaximumStringBytes)
            {
                error = "The Firestar project metadata is too large.";
                return false;
            }
            FirestarProjectHeader header;
            header.flags = flags;
            header.buildBaseBytes = buildBase.size();
            header.manifestHintBytes = manifestHint.size();
            header.jsonBytes = json.size();
            const size_t total = sizeof(header) + buildBase.size() + manifestHint.size() + json.size();
            bytes.resize(total);
            size_t cursor = 0;
            const auto append = [&bytes, &cursor](const void* data, const size_t size) {
                if (size) std::memcpy(bytes.data() + cursor, data, size);
                cursor += size;
            };
            append(&header, sizeof(header));
            append(buildBase.data(), buildBase.size());
            append(manifestHint.data(), manifestHint.size());
            append(json.data(), json.size());
            return true;
        }

        bool SaveFirestarProjectTo(const fs::path& destination)
        {
            if (!repakProject_.IsOpen() || destination.empty())
                return false;
            const fs::path absoluteDestination = fs::absolute(destination).lexically_normal();
            std::vector<std::uint8_t> bytes;
            std::string error;
            if (!BuildFirestarProjectBytes(absoluteDestination, false, bytes, error))
            {
                AppendRePakLog(firestar::repak::LogLevel::Error, error);
                return false;
            }
            std::error_code directoryError;
            fs::create_directories(absoluteDestination.parent_path(), directoryError);
            if (directoryError)
            {
                AppendRePakLog(firestar::repak::LogLevel::Error,
                    "Unable to create the project folder: " + directoryError.message());
                return false;
            }
            // Keep a normal workspace obvious on disk: the named .fsp sits
            // beside the project's assets and build folders.
            fs::create_directories(absoluteDestination.parent_path() / L"assets", directoryError);
            if (!directoryError)
                fs::create_directories(absoluteDestination.parent_path() / L"build", directoryError);
            if (directoryError)
            {
                AppendRePakLog(firestar::repak::LogLevel::Error,
                    "Unable to create the project assets/build folders: " + directoryError.message());
                return false;
            }

            std::ofstream output(absoluteDestination, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                AppendRePakLog(firestar::repak::LogLevel::Error,
                    "Unable to write " + PathToUtf8(absoluteDestination));
                return false;
            }
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!output.good())
            {
                AppendRePakLog(firestar::repak::LogLevel::Error, "The Firestar project save was incomplete.");
                return false;
            }
            currentRePakProjectPath_ = absoluteDestination;
            repakProject_.MarkClean();
            AppendRePakLog(firestar::repak::LogLevel::Info,
                "Saved project " + PathToUtf8(currentRePakProjectPath_));
            return true;
        }

        bool PackAndExportProjectTo(const fs::path& destination, std::string& error)
        {
            if (!repakProject_.IsOpen())
            {
                error = "Open a Firestar project before packing it.";
                return false;
            }
            const std::string archiveName = FileStemSafe(PathToUtf8(destination.stem()));
            std::vector<firestar::editor::PackedProjectEntry> entries;
            firestar::editor::PackedProjectEntry projectEntry;
            projectEntry.relativePath = PathFromUtf8(archiveName + ".fsp");
            if (!BuildFirestarProjectBytes(destination, true, projectEntry.bytes, error))
                return false;
            entries.push_back(std::move(projectEntry));

            const fs::path assetsRoot = repakProject_.AssetsDirectory();
            std::error_code ioError;
            if (fs::is_directory(assetsRoot, ioError))
            {
                fs::recursive_directory_iterator iterator(assetsRoot,
                    fs::directory_options::skip_permission_denied, ioError);
                const fs::recursive_directory_iterator end;
                for (; !ioError && iterator != end; iterator.increment(ioError))
                {
                    const fs::directory_entry& source = *iterator;
                    if (!source.is_regular_file(ioError) || source.is_symlink(ioError))
                    {
                        ioError.clear();
                        continue;
                    }
                    const std::uintmax_t fileSize = source.file_size(ioError);
                    if (ioError || fileSize > 2ull * 1024ull * 1024ull * 1024ull)
                    {
                        error = ioError ? "Unable to inspect an asset file: " + ioError.message()
                            : "An individual project asset exceeds Firestar's 2 GiB packing limit.";
                        return false;
                    }
                    const fs::path relative = fs::relative(source.path(), assetsRoot, ioError);
                    if (ioError || relative.empty())
                    {
                        error = "Unable to create a portable path for " + PathToUtf8(source.path());
                        return false;
                    }
                    firestar::editor::PackedProjectEntry entry;
                    entry.relativePath = fs::path(L"assets") / relative;
                    std::ifstream input(source.path(), std::ios::binary | std::ios::ate);
                    if (!input)
                    {
                        error = "Unable to read " + PathToUtf8(source.path());
                        return false;
                    }
                    const std::streamoff size = input.tellg();
                    if (size < 0)
                    {
                        error = "Unable to determine the size of " + PathToUtf8(source.path());
                        return false;
                    }
                    entry.bytes.resize(static_cast<size_t>(size));
                    input.seekg(0);
                    if (size > 0 && !input.read(reinterpret_cast<char*>(entry.bytes.data()), size))
                    {
                        error = "Unable to read " + PathToUtf8(source.path());
                        return false;
                    }
                    entries.push_back(std::move(entry));
                }
                if (ioError)
                {
                    error = "Unable to enumerate the project assets: " + ioError.message();
                    return false;
                }
            }
            if (!firestar::editor::WritePackedProject(destination, entries, error))
                return false;
            AppendRePakLog(firestar::repak::LogLevel::Info,
                "Packed " + std::to_string(entries.size() - 1) + " asset file(s) into " +
                PathToUtf8(destination) + ".");
            return true;
        }

        void PackAndExportProject(const HWND window)
        {
            const auto destination = ChoosePackedProjectFile(window, true);
            if (!destination) return;
            std::string error;
            if (!PackAndExportProjectTo(*destination, error))
            {
                editorError_ = error;
                showEditorError_ = true;
                AppendRePakLog(firestar::repak::LogLevel::Error, error);
            }
        }

        void OpenPackedProject(const fs::path& path)
        {
            pendingPackedProjectPath_ = fs::absolute(path).lexically_normal();
            showPackedProjectPrompt_ = true;
        }

        bool LoadFirestarProject(const fs::path& path)
        {
            const fs::path absolutePath = fs::absolute(path).lexically_normal();
            std::ifstream input(absolutePath, std::ios::binary);
            FirestarProjectHeader header;
            if (!input.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
                std::memcmp(header.magic, "FIRESP1", 7) != 0 || header.version != 1 ||
                header.buildBaseBytes > FirestarProjectMaximumStringBytes ||
                header.manifestHintBytes > FirestarProjectMaximumStringBytes ||
                header.jsonBytes > FirestarProjectMaximumStringBytes)
            {
                editorError_ = "This is not a supported Firestar project file.";
                showEditorError_ = true;
                return false;
            }
            const auto readString = [&input](const std::uint64_t length, std::string& value) {
                value.resize(static_cast<size_t>(length));
                return length == 0 || static_cast<bool>(input.read(value.data(), static_cast<std::streamsize>(length)));
            };
            std::string buildBaseText;
            std::string manifestHintText;
            std::string json;
            if (!readString(header.buildBaseBytes, buildBaseText) ||
                !readString(header.manifestHintBytes, manifestHintText) ||
                !readString(header.jsonBytes, json))
            {
                editorError_ = "The Firestar project file ended unexpectedly.";
                showEditorError_ = true;
                return false;
            }
            fs::path buildBase = PathFromUtf8(buildBaseText);
            if ((header.flags & FirestarProjectBaseRelative) != 0)
                buildBase = absolutePath.parent_path() / buildBase;
            buildBase = fs::absolute(buildBase).lexically_normal();
            fs::path manifestHint = PathFromUtf8(manifestHintText);
            if ((header.flags & FirestarProjectManifestRelative) != 0)
                manifestHint = buildBase / manifestHint;

            std::string error;
            if (!repakProject_.LoadSerialized(json, manifestHint, buildBase, error))
            {
                AppendRePakLog(firestar::repak::LogLevel::Error, error);
                editorError_ = error;
                showEditorError_ = true;
                return false;
            }
            currentRePakProjectPath_ = absolutePath;
            EnsureGeneratedStreamPaths();
            workspaceMode_ = WorkspaceMode::RePakProject;
            selectedAsset_ = repakProject_.AssetCount() ? 0 : -1;
            RefreshSourceData();
            AppendRePakLog(firestar::repak::LogLevel::Info,
                "Opened project " + PathToUtf8(currentRePakProjectPath_));
            return true;
        }

        bool ExportRePakJson(const HWND window)
        {
            const auto destination = ChooseRePakManifest(window, true);
            if (!destination) return false;
            std::string error;
            if (!repakProject_.ExportJson(*destination, error))
            {
                AppendRePakLog(firestar::repak::LogLevel::Error, error);
                return false;
            }
            AppendRePakLog(firestar::repak::LogLevel::Info,
                "Exported RePak JSON " + PathToUtf8(*destination));
            return true;
        }

        void RequestHome()
        {
            if (repakProject_.IsOpen() && repakProject_.IsDirty())
                showHomeSavePrompt_ = true;
            else
                workspaceMode_ = WorkspaceMode::Home;
        }

        void StartRePakBuild()
        {
            if (!repakProject_.IsOpen() || repakBuilding_)
                return;
            if (repakProject_.HasLeadingAnimationAsset())
            {
                AppendRePakLog(firestar::repak::LogLevel::Warning,
                    "This project contains only animation rigs/sequences. RePak animation assets are unreliable at index 0; add a real non-animation asset before building.");
            }
            if (repakBuildThread_.joinable())
                repakBuildThread_.join();
            {
                std::lock_guard<std::mutex> lock(repakBuildMutex_);
                repakBuildLog_.clear();
                repakBuildResult_.clear();
                repakBuildSucceeded_ = false;
            }
            const fs::path manifest = repakProject_.BuildManifestPath();
            const fs::path manifestBase = repakProject_.BuildBaseDirectory();
            const std::string manifestJson = repakProject_.SerializeDocument();
            repakBuilding_ = true;
            repakBuildThread_ = std::thread([this, manifest, manifestBase, manifestJson]() {
                firestar::repak::BuildRequest request;
                request.manifestPath = manifest;
                request.manifestBaseDirectory = manifestBase;
                request.manifestJson = manifestJson;
                request.log = [this](const firestar::repak::LogLevel level, const std::string_view message) {
                    AppendRePakLog(level, std::string(message));
                };
                const auto result = firestar::repak::Build(request);
                {
                    std::lock_guard<std::mutex> lock(repakBuildMutex_);
                    repakBuildSucceeded_ = result.succeeded;
                    repakBuildResult_ = result.message;
                }
                repakBuilding_ = false;
            });
        }

        void ClearAssetPreview()
        {
            if (assetPreviewTexture_)
            {
                assetPreviewTexture_->Release();
                assetPreviewTexture_ = nullptr;
            }
            assetPreviewPath_.clear();
            assetPreviewWidth_ = 0;
            assetPreviewHeight_ = 0;
            assetPreviewFormat_.clear();
            assetPreviewError_.clear();
            modelPreviewMaterials_.clear();
            modelMissingTexture_.Reset();
            modelPreview_.Clear();
        }

        std::optional<fs::path> SelectedAssetPreviewSource() const
        {
            if (selectedAsset_ < 0)
                return std::nullopt;
            const rapidjson::Value* selected = repakProject_.Asset(static_cast<size_t>(selectedAsset_));
            if (!selected || !selected->IsObject())
                return std::nullopt;
            const std::string type = AssetMemberString(*selected, "_type");
            if (type == "txtr")
                return repakProject_.ResolvePrimarySource(static_cast<size_t>(selectedAsset_));
            if (type != "uimg")
                return std::nullopt;

            const std::string atlasPath = AssetMemberString(*selected, "atlas");
            if (atlasPath.empty())
                return std::nullopt;
            for (size_t index = 0; index < repakProject_.AssetCount(); ++index)
            {
                const rapidjson::Value* asset = repakProject_.Asset(index);
                if (asset && AssetMemberString(*asset, "_type") == "txtr" &&
                    AssetMemberString(*asset, "_path") == atlasPath)
                    return repakProject_.ResolvePrimarySource(index);
            }
            return std::nullopt;
        }

        bool CreatePreviewRgbaTexture(const std::vector<std::uint8_t>& rgba,
            const UINT width, const UINT height, ID3D11ShaderResourceView** output)
        {
            if (!g_device || !output || rgba.empty() || width == 0 || height == 0)
                return false;
            *output = nullptr;
            D3D11_TEXTURE2D_DESC description{};
            description.Width = width;
            description.Height = height;
            description.MipLevels = 1;
            description.ArraySize = 1;
            // WIC returns PNG colour channels in sRGB. Firestar renders to an
            // scRGB float swapchain, so the SRV must perform the sRGB-to-linear
            // conversion or imported artwork appears far too bright.
            description.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_IMMUTABLE;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{};
            data.pSysMem = rgba.data();
            data.SysMemPitch = width * 4;
            data.SysMemSlicePitch = data.SysMemPitch * height;
            ComPtr<ID3D11Texture2D> texture;
            if (FAILED(g_device->CreateTexture2D(&description, &data, &texture)) ||
                FAILED(g_device->CreateShaderResourceView(texture.Get(), nullptr, output)))
                return false;
            return true;
        }

        bool CreateAssetPreviewRgba(const std::vector<std::uint8_t>& rgba,
            const UINT width, const UINT height)
        {
            if (!CreatePreviewRgbaTexture(rgba, width, height, &assetPreviewTexture_))
                return false;
            assetPreviewWidth_ = width;
            assetPreviewHeight_ = height;
            assetPreviewFormat_ = "RGBA8";
            return true;
        }

        static DXGI_FORMAT PreviewTypedFormat(const DXGI_FORMAT format)
        {
            switch (format)
            {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
            case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
            case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
            case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
            case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;
            case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;
            case DXGI_FORMAT_BC6H_TYPELESS: return DXGI_FORMAT_BC6H_UF16;
            case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;
            default: return format;
            }
        }

        static bool PreviewSurfacePitch(const DXGI_FORMAT format, const UINT width, const UINT height,
            UINT& rowPitch, UINT& slicePitch)
        {
            UINT blockBytes = 0;
            switch (format)
            {
            case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
            case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
                blockBytes = 8; break;
            case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
            case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
            case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
            case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
            case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
                blockBytes = 16; break;
            default: break;
            }
            if (blockBytes)
            {
                rowPitch = (std::max)(1u, (width + 3) / 4) * blockBytes;
                slicePitch = rowPitch * (std::max)(1u, (height + 3) / 4);
                return true;
            }

            UINT bits = 0;
            switch (format)
            {
            case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
            case DXGI_FORMAT_R32G32B32A32_SINT: bits = 128; break;
            case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT:
            case DXGI_FORMAT_R32G32B32_SINT: bits = 96; break;
            case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
            case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM:
            case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R32G32_FLOAT:
            case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: bits = 64; break;
            case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
            case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT:
            case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT:
            case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
            case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SNORM:
            case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R32_FLOAT:
            case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
            case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
                bits = 32; break;
            case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM:
            case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM:
            case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT:
            case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B5G5R5A1_UNORM:
            case DXGI_FORMAT_B4G4R4A4_UNORM: bits = 16; break;
            case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM:
            case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_A8_UNORM: bits = 8; break;
            case DXGI_FORMAT_R8G8_B8G8_UNORM: case DXGI_FORMAT_G8R8_G8B8_UNORM:
            case DXGI_FORMAT_YUY2:
                rowPitch = ((width + 1) / 2) * 4;
                slicePitch = rowPitch * height;
                return true;
            default: return false;
            }
            rowPitch = static_cast<UINT>((static_cast<std::uint64_t>(width) * bits + 7) / 8);
            slicePitch = rowPitch * height;
            return true;
        }

        bool CreatePreviewDdsTexture(const fs::path& path, ID3D11ShaderResourceView** output,
            UINT* outputWidth = nullptr, UINT* outputHeight = nullptr,
            std::string* outputFormat = nullptr)
        {
            if (!g_device || !output) return false;
            *output = nullptr;
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input) return false;
            const std::streamoff fileSize = input.tellg();
            if (fileSize < 128 || fileSize > 1024ll * 1024ll * 1024ll) return false;
            std::vector<std::uint8_t> bytes(static_cast<size_t>(fileSize));
            input.seekg(0);
            input.read(reinterpret_cast<char*>(bytes.data()), fileSize);
            std::uint32_t magic{};
            std::memcpy(&magic, bytes.data(), sizeof(magic));
            if (!input || magic != 0x20534444)
                return false;

            DDS_HEADER header{};
            std::memcpy(&header, bytes.data() + 4, sizeof(header));
            DXGI_FORMAT format = DXUtils::GetFormatFromHeader(header);
            size_t dataOffset = 4 + sizeof(header);
            if (header.ddspf.dwFourCC == 0x30315844u) // "DX10"
            {
                if (bytes.size() < dataOffset + sizeof(DDS_HEADER_DXT10)) return false;
                DDS_HEADER_DXT10 dx10{};
                std::memcpy(&dx10, bytes.data() + dataOffset, sizeof(dx10));
                format = dx10.dxgiFormat;
                dataOffset += sizeof(dx10);
            }
            format = PreviewTypedFormat(format);
            UINT rowPitch{};
            UINT slicePitch{};
            if (format == DXGI_FORMAT_UNKNOWN || header.dwWidth == 0 || header.dwHeight == 0 ||
                header.dwWidth > 16384 || header.dwHeight > 16384 ||
                !PreviewSurfacePitch(format, header.dwWidth, header.dwHeight, rowPitch, slicePitch) ||
                slicePitch > bytes.size() - dataOffset)
                return false;

            D3D11_TEXTURE2D_DESC description{};
            description.Width = header.dwWidth;
            description.Height = header.dwHeight;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = format;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_IMMUTABLE;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{};
            data.pSysMem = bytes.data() + dataOffset;
            data.SysMemPitch = rowPitch;
            data.SysMemSlicePitch = slicePitch;
            ComPtr<ID3D11Texture2D> texture;
            if (FAILED(g_device->CreateTexture2D(&description, &data, &texture)) ||
                FAILED(g_device->CreateShaderResourceView(texture.Get(), nullptr, output)))
                return false;
            if (outputWidth) *outputWidth = header.dwWidth;
            if (outputHeight) *outputHeight = header.dwHeight;
            if (outputFormat) *outputFormat = DXUtils::GetFormatAsString(format);
            return true;
        }

        bool CreateAssetPreviewDds(const fs::path& path)
        {
            return CreatePreviewDdsTexture(path, &assetPreviewTexture_,
                &assetPreviewWidth_, &assetPreviewHeight_, &assetPreviewFormat_);
        }

        static std::optional<std::string> MaterialTextureReference(const rapidjson::Value& material)
        {
            if (!material.IsObject()) return std::nullopt;
            const rapidjson::Value* textures = nullptr;
            const auto dollarTextures = material.FindMember("$textures");
            if (dollarTextures != material.MemberEnd() && dollarTextures->value.IsObject())
                textures = &dollarTextures->value;
            else
            {
                const auto plainTextures = material.FindMember("textures");
                if (plainTextures != material.MemberEnd() && plainTextures->value.IsObject())
                    textures = &plainTextures->value;
            }
            if (!textures) return std::nullopt;

            const rapidjson::Value* textureTypes = nullptr;
            const auto types = material.FindMember("$textureTypes");
            if (types != material.MemberEnd() && types->value.IsObject())
                textureTypes = &types->value;

            if (textureTypes)
            {
                for (auto type = textureTypes->MemberBegin(); type != textureTypes->MemberEnd(); ++type)
                {
                    if (!type->value.IsString()) continue;
                    const std::string description = ToLowerAscii(type->value.GetString());
                    if (description.find("albedo") == std::string::npos &&
                        description.find("diffuse") == std::string::npos &&
                        description.find("basecolor") == std::string::npos &&
                        description.find("base_color") == std::string::npos)
                        continue;
                    const auto texture = textures->FindMember(type->name.GetString());
                    if (texture != textures->MemberEnd() && texture->value.IsString())
                        return std::string(texture->value.GetString(), texture->value.GetStringLength());
                }
            }

            const auto slotZero = textures->FindMember("0");
            if (slotZero != textures->MemberEnd() && slotZero->value.IsString())
                return std::string(slotZero->value.GetString(), slotZero->value.GetStringLength());
            for (auto texture = textures->MemberBegin(); texture != textures->MemberEnd(); ++texture)
                if (texture->value.IsString())
                    return std::string(texture->value.GetString(), texture->value.GetStringLength());
            return std::nullopt;
        }

        std::optional<std::string> ResolveMaterialTextureReference(const size_t materialAssetIndex) const
        {
            const rapidjson::Value* material = repakProject_.Asset(materialAssetIndex);
            if (!material) return std::nullopt;
            if (const auto embedded = MaterialTextureReference(*material))
                return embedded;

            const auto source = repakProject_.ResolvePrimarySource(materialAssetIndex);
            if (!source || ToLowerAscii(source->extension().string()) != ".json" ||
                !fs::is_regular_file(*source))
                return std::nullopt;
            std::ifstream input(*source, std::ios::binary);
            rapidjson::IStreamWrapper stream(input);
            rapidjson::Document sidecar;
            sidecar.ParseStream(stream);
            return sidecar.HasParseError() ? std::nullopt : MaterialTextureReference(sidecar);
        }

        std::optional<fs::path> ResolveModelTextureSource(const std::string& reference) const
        {
            const std::string normalized = NormalizeAssetPath(reference);
            const std::string normalizedLower = ToLowerAscii(normalized);
            for (size_t index = 0; index < repakProject_.AssetCount(); ++index)
            {
                const rapidjson::Value* candidate = repakProject_.Asset(index);
                if (!candidate || AssetMemberString(*candidate, "_type") != "txtr") continue;
                const std::string candidatePath = NormalizeAssetPath(AssetMemberString(*candidate, "_path"));
                if (ToLowerAscii(candidatePath) != normalizedLower &&
                    RTech::StringToGuid(candidatePath.c_str()) != RTech::StringToGuid(normalized.c_str()))
                    continue;
                const auto source = repakProject_.ResolvePrimarySource(index);
                if (source && fs::is_regular_file(*source)) return source->lexically_normal();
            }

            fs::path relative = PathFromUtf8(normalized);
            if (!relative.empty() && ToLowerAscii(PathToUtf8(*relative.begin())) == "assets")
            {
                fs::path withoutAssets;
                auto component = relative.begin();
                for (++component; component != relative.end(); ++component)
                    withoutAssets /= *component;
                relative = std::move(withoutAssets);
            }
            const fs::path base = repakProject_.AssetsDirectory() / relative;
            const std::array<const wchar_t*, 5> extensions{L".dds", L".png", L".tga", L".jpg", L".jpeg"};
            for (const wchar_t* extension : extensions)
            {
                fs::path candidate = base;
                candidate.replace_extension(extension);
                if (fs::is_regular_file(candidate)) return candidate.lexically_normal();
            }
            return std::nullopt;
        }

        bool LoadModelPreviewTexture(const fs::path& source, ComPtr<ID3D11ShaderResourceView>& texture)
        {
            ID3D11ShaderResourceView* loaded{};
            if (ToLowerAscii(source.extension().string()) == ".dds")
            {
                if (!CreatePreviewDdsTexture(source, &loaded)) return false;
            }
            else
            {
                Sprite decoded;
                std::string error;
                if (!LoadImage(source, decoded, error) ||
                    !CreatePreviewRgbaTexture(decoded.rgba, decoded.width, decoded.height, &loaded))
                    return false;
            }
            texture.Attach(loaded);
            return true;
        }

        bool CreateMissingModelTexture()
        {
            constexpr UINT Width = 128;
            constexpr UINT Height = 128;
            constexpr UINT Block = 16;
            std::vector<std::uint8_t> pixels(static_cast<size_t>(Width) * Height * 4);
            for (UINT y = 0; y < Height; ++y)
            {
                for (UINT x = 0; x < Width; ++x)
                {
                    const bool magenta = ((x / Block) + (y / Block)) % 2 == 0;
                    const size_t offset = (static_cast<size_t>(y) * Width + x) * 4;
                    pixels[offset + 0] = magenta ? 255 : 8;
                    pixels[offset + 1] = magenta ? 0 : 8;
                    pixels[offset + 2] = magenta ? 255 : 8;
                    pixels[offset + 3] = 255;
                }
            }
            ID3D11ShaderResourceView* texture{};
            if (!CreatePreviewRgbaTexture(pixels, Width, Height, &texture)) return false;
            modelMissingTexture_.Attach(texture);
            return true;
        }

        void PrepareModelPreviewTextures()
        {
            modelPreviewMaterials_.clear();
            modelMissingTexture_.Reset();
            (void)CreateMissingModelTexture();
            for (const auto& mesh : modelPreview_.meshes)
            {
                const auto existing = std::find_if(modelPreviewMaterials_.begin(), modelPreviewMaterials_.end(),
                    [&mesh](const ModelPreviewMaterialTexture& value) { return value.guid == mesh.materialGuid; });
                if (existing != modelPreviewMaterials_.end()) continue;

                ModelPreviewMaterialTexture material;
                material.guid = mesh.materialGuid;
                const int materialAssetIndex = FindProjectAssetByGuid(mesh.materialGuid);
                if (materialAssetIndex >= 0)
                {
                    const auto reference = ResolveMaterialTextureReference(static_cast<size_t>(materialAssetIndex));
                    const auto source = reference ? ResolveModelTextureSource(*reference) : std::nullopt;
                    if (source && LoadModelPreviewTexture(*source, material.texture))
                    {
                        material.sourcePath = *source;
                        material.missing = false;
                    }
                }
                modelPreviewMaterials_.push_back(std::move(material));
            }
        }

        const ModelPreviewMaterialTexture* ModelPreviewTexture(const std::uint64_t guid) const
        {
            const auto found = std::find_if(modelPreviewMaterials_.begin(), modelPreviewMaterials_.end(),
                [guid](const ModelPreviewMaterialTexture& material) { return material.guid == guid; });
            return found == modelPreviewMaterials_.end() ? nullptr : &*found;
        }

        void UpdateAssetPreview()
        {
            const bool changedAsset = selectedAsset_ != modelPreviewAssetIndex_;
            if (changedAsset)
            {
                modelPreviewAssetIndex_ = selectedAsset_;
                modelPreviewLod_ = 0;
                modelPreviewSelectedBodyPart_ = 0;
                modelPreviewBodySelections_.clear();
            }
            ClearAssetPreview();
            if (selectedAsset_ >= 0)
            {
                const rapidjson::Value* asset = repakProject_.Asset(static_cast<size_t>(selectedAsset_));
                if (asset && AssetMemberString(*asset, "_type") == "mdl_")
                {
                    const auto source = repakProject_.ResolvePrimarySource(static_cast<size_t>(selectedAsset_));
                    if (!source || !fs::is_regular_file(*source))
                    {
                        assetPreviewError_ = "No RMDL source was found for this model.";
                        return;
                    }
                    assetPreviewPath_ = *source;
                    if (!firestar::editor::LoadRmdlPreview(*source, modelPreviewLod_, modelPreview_, assetPreviewError_))
                        modelPreview_.Clear();
                    else
                    {
                        modelPreviewLod_ = modelPreview_.selectedLod;
                        if (modelPreviewBodySelections_.size() != modelPreview_.bodyParts.size())
                            modelPreviewBodySelections_.assign(modelPreview_.bodyParts.size(), 0);
                        if (modelPreviewSelectedBodyPart_ >= modelPreview_.bodyParts.size())
                            modelPreviewSelectedBodyPart_ = 0;
                        PrepareModelPreviewTextures();
                    }
                    return;
                }
            }
            const auto previewSource = SelectedAssetPreviewSource();
            if (!previewSource || !fs::is_regular_file(*previewSource))
            {
                assetPreviewError_ = "No previewable texture source was found for this asset.";
                return;
            }
            assetPreviewPath_ = *previewSource;
            Sprite decoded;
            std::string decodeError;
            if (LoadImage(*previewSource, decoded, decodeError) &&
                CreateAssetPreviewRgba(decoded.rgba, decoded.width, decoded.height))
                return;
            if (ToLowerAscii(previewSource->extension().string()) == ".dds" && CreateAssetPreviewDds(*previewSource))
                return;
            assetPreviewError_ = "This texture format could not be previewed by DirectX or Windows Imaging Component.";
        }

        void RefreshSourceData()
        {
            ResetAudioPreview();
            loadedSourcePath_.clear();
            loadedSourceBytes_.clear();
            sourceLoadError_.clear();
            if (selectedAsset_ < 0)
            {
                ClearAssetPreview();
                return;
            }
            // Large RMDL and texture previews are expensive to decode. Load
            // them only when the Preview tab is opened so selecting assets and
            // editing their manifest data never stalls the main window.
            ClearAssetPreview();
            modelPreviewAssetIndex_ = -1;
            const auto source = repakProject_.ResolvePrimarySource(static_cast<size_t>(selectedAsset_));
            if (!source)
                return;
            loadedSourcePath_ = *source;
            std::ifstream input(*source, std::ios::binary | std::ios::ate);
            if (!input)
            {
                sourceLoadError_ = "Source file does not exist yet.";
                return;
            }
            const std::streamoff size = input.tellg();
            if (size < 0 || size > 64 * 1024 * 1024)
            {
                sourceLoadError_ = "Source viewer is limited to files up to 64 MiB.";
                return;
            }
            loadedSourceBytes_.resize(static_cast<size_t>(size));
            input.seekg(0);
            if (size > 0)
                input.read(reinterpret_cast<char*>(loadedSourceBytes_.data()), size);
            if (!input.good() && !input.eof())
            {
                loadedSourceBytes_.clear();
                sourceLoadError_ = "Unable to read the complete source file.";
            }
        }

        void CommitSourceReplacement(const PendingReplacement& replacement)
        {
            std::error_code error;
            fs::create_directories(replacement.target.parent_path(), error);
            if (!error && fs::exists(replacement.target))
                fs::copy_file(replacement.target, fs::path(replacement.target.wstring() + L".firestar.bak"),
                    fs::copy_options::overwrite_existing, error);
            error.clear();
            fs::copy_file(replacement.source, replacement.target, fs::copy_options::overwrite_existing, error);
            if (error)
                AppendRePakLog(firestar::repak::LogLevel::Error, "Replace failed: " + error.message());
            else
                AppendRePakLog(firestar::repak::LogLevel::Info,
                    "Replaced " + PathToUtf8(replacement.target) + " (backup saved beside it)");
            RefreshSourceData();
        }

        void ReplaceSelectedSource(const HWND window)
        {
            if (selectedAsset_ < 0)
                return;
            const auto target = repakProject_.ResolvePrimarySource(static_cast<size_t>(selectedAsset_));
            const auto replacement = ChooseAnySourceFile(window, L"Choose replacement source asset");
            if (!target || !replacement)
                return;
            const rapidjson::Value* asset = repakProject_.Asset(static_cast<size_t>(selectedAsset_));
            const std::string type = asset ? AssetMemberString(*asset, "_type") : std::string{};
            PendingReplacement pending{*replacement, *target, static_cast<size_t>(selectedAsset_),
                firestar::editor::InspectAssetCompatibility(*replacement, type, repakProject_.PakVersion())};
            if (!pending.compatibility.compatible)
                pendingReplacement_ = std::move(pending);
            else
                CommitSourceReplacement(pending);
        }

        void PrepareNearbyAssets(const char* const field, const std::vector<std::string>& extensions)
        {
            if (selectedAsset_ < 0)
                return;
            assetReferenceTargetIndex_ = static_cast<size_t>(selectedAsset_);
            nearbyTargetField_ = field;
            projectAssetReferenceChoices_.clear();
            assetReferenceSearch_.fill('\0');
            nearbyAssetChoices_ = repakProject_.FindNearbyAssets(assetReferenceTargetIndex_, extensions);
            openAssetReferencePopup_ = true;
        }

        void PrepareProjectAssetReferences(const char* const field, const char* const type)
        {
            if (selectedAsset_ < 0)
                return;
            assetReferenceTargetIndex_ = static_cast<size_t>(selectedAsset_);
            nearbyTargetField_ = field;
            nearbyAssetChoices_.clear();
            projectAssetReferenceChoices_.clear();
            assetReferenceSearch_.fill('\0');
            for (size_t index = 0; index < repakProject_.AssetCount(); ++index)
            {
                const rapidjson::Value* asset = repakProject_.Asset(index);
                if (asset && AssetMemberString(*asset, "_type") == type)
                {
                    const std::string path = AssetMemberString(*asset, "_path");
                    if (!path.empty()) projectAssetReferenceChoices_.push_back(path);
                }
            }
            openAssetReferencePopup_ = true;
        }

        bool IsInsideAssetsDirectory(const fs::path& path) const
        {
            std::error_code error;
            const fs::path root = fs::weakly_canonical(repakProject_.AssetsDirectory(), error);
            if (error) return false;
            const fs::path candidate = fs::weakly_canonical(path, error);
            if (error) return false;
            auto rootPart = root.begin();
            auto candidatePart = candidate.begin();
            for (; rootPart != root.end(); ++rootPart, ++candidatePart)
            {
                if (candidatePart == candidate.end() ||
                    _wcsicmp(rootPart->c_str(), candidatePart->c_str()) != 0) return false;
            }
            return true;
        }

        std::vector<fs::path> AssetSourceFiles(const size_t index) const
        {
            std::vector<fs::path> files;
            const rapidjson::Value* asset = repakProject_.Asset(index);
            if (!asset) return files;
            const std::string type = AssetMemberString(*asset, "_type");
            const auto primary = repakProject_.ResolvePrimarySource(index);
            if (!primary) return files;
            const auto appendExtension = [this, &files, primary](const char* extension) {
                fs::path candidate = *primary;
                candidate.replace_extension(extension);
                if (!fs::is_regular_file(candidate) || !IsInsideAssetsDirectory(candidate)) return;
                const auto existing = std::find_if(files.begin(), files.end(), [&candidate](const fs::path& value) {
                    return _wcsicmp(value.c_str(), candidate.c_str()) == 0;
                });
                if (existing == files.end()) files.push_back(candidate.lexically_normal());
            };
            if (type == "matl") { appendExtension(".json"); appendExtension(".uber"); }
            else if (type == "txtr") { appendExtension(".dds"); appendExtension(".json"); }
            else if (type == "mdl_") { appendExtension(".rmdl"); appendExtension(".vg"); appendExtension(".phy"); }
            else if (type == "aseq") { appendExtension(".rseq"); appendExtension(".json"); }
            else if (type == "awsr" || type == "asrc")
            {
                appendExtension(".wav"); appendExtension(".ogg"); appendExtension(".oga");
            }
            else if (fs::is_regular_file(*primary) && IsInsideAssetsDirectory(*primary))
                files.push_back(primary->lexically_normal());
            return files;
        }

        void RemoveAssetFromProject(const size_t index)
        {
            if (!repakProject_.RemoveAsset(index)) return;
            selectedAsset_ = repakProject_.AssetCount()
                ? (std::min)(selectedAsset_, static_cast<int>(repakProject_.AssetCount() - 1)) : -1;
            RefreshSourceData();
        }

        void RequestDeleteAssetSources(const size_t index)
        {
            pendingDeleteAsset_ = static_cast<int>(index);
            pendingDeleteSourceFiles_ = AssetSourceFiles(index);
            openDeleteAssetPopup_ = true;
        }

        int AssetTypeIndex(const std::string_view code) const
        {
            for (size_t index = 0; index < RePakAssetTypes.size(); ++index)
                if (code == RePakAssetTypes[index].code) return static_cast<int>(index);
            return -1;
        }

        static fs::path DefaultImportFolder(const std::string_view type)
        {
            if (type == "txtr") return "texture";
            if (type == "matl") return "material";
            if (type == "mdl_") return "models";
            if (type == "arig") return "animrig";
            if (type == "aseq") return "animseq";
            if (type == "anir") return "animrecording";
            if (type == "txan") return "texture_anim";
            if (type == "txls") return "texture_list";
            if (type == "uimg") return "ui_image_atlas";
            if (type == "ui") return "ui";
            if (type == "shdr") return "shader";
            if (type == "shds") return "shaderset";
            if (type == "dtbl") return "datatable";
            if (type == "stlt") return "settings_layout";
            if (type == "stgs") return "settings";
            if (type == "mt4a") return "material_for_aspect";
            if (type == "font") return "font";
            if (type == "rlcd") return "lcd";
            if (type == "awsr" || type == "aevt") return "audio";
            if (type == "Ptch") return "patch";
            return "assets";
        }

        static fs::path NormalizeImportedRelativePath(const fs::path& relative,
            const std::string_view type)
        {
            fs::path afterAssets;
            bool foundAssets = false;
            for (const fs::path& component : relative)
            {
                const std::string name = ToLowerAscii(PathToUtf8(component));
                if (!foundAssets && name == "assets")
                {
                    foundAssets = true;
                    continue;
                }
                if (foundAssets)
                    afterAssets /= component;
            }
            if (foundAssets && !afterAssets.empty())
                return afterAssets;

            fs::path cleaned = relative;
            auto first = cleaned.begin();
            if (first != cleaned.end() && ToLowerAscii(PathToUtf8(*first)) == "imported")
            {
                fs::path withoutImported;
                for (++first; first != cleaned.end(); ++first)
                    withoutImported /= *first;
                cleaned = std::move(withoutImported);
            }
            if (cleaned.empty())
                return DefaultImportFolder(type);
            if (std::distance(cleaned.begin(), cleaned.end()) == 1)
                return DefaultImportFolder(type) / cleaned;
            return cleaned;
        }

        std::string InferAssetType(const fs::path& source) const
        {
            const std::string extension = ToLowerAscii(source.extension().string());
            if (extension == ".dds") return "txtr";
            if (extension == ".rmdl") return "mdl_";
            if (extension == ".rrig") return "arig";
            if (extension == ".rseq") return "aseq";
            if (extension == ".anir") return "anir";
            if (extension == ".txan") return "txan";
            if (extension == ".ruip") return "ui";
            if (extension == ".csv") return "dtbl";
            if (extension == ".wav" || extension == ".ogg" || extension == ".oga") return "awsr";
            if (extension == ".uber") return "matl";
            if (extension == ".bin") return "font";
            if (extension == ".msw")
            {
                std::ifstream input(source, std::ios::binary);
                std::array<unsigned char, 5> header{};
                if (input.read(reinterpret_cast<char*>(header.data()), header.size()) &&
                    header[0] == 'M' && header[1] == 'S' && header[2] == 'W')
                    return header[4] == 1 ? "shds" : "shdr";
            }
            if (extension == ".json")
            {
                std::ifstream input(source, std::ios::binary);
                rapidjson::IStreamWrapper stream(input);
                rapidjson::Document document;
                document.ParseStream(stream);
                if (document.IsObject())
                {
                    const auto explicitType = document.FindMember("_type");
                    if (explicitType != document.MemberEnd() && explicitType->value.IsString() &&
                        AssetTypeIndex(explicitType->value.GetString()) >= 0)
                        return explicitType->value.GetString();
                    if (document.HasMember("atlas") && document.HasMember("images")) return "uimg";
                    if (document.HasMember("pixelScaleX1") || document.HasMember("pixelFlicker")) return "rlcd";
                    if (document.HasMember("subLayouts") || document.HasMember("extraDataSizeIndex")) return "stlt";
                    if (document.HasMember("shaderSet") || document.HasMember("surface") || document.HasMember("passReferences")) return "matl";
                    if (document.HasMember("materials")) return "mt4a";
                    if (document.HasMember("sources") || document.HasMember("stopEvents")) return "aevt";
                    if (document.HasMember("layout") || document.HasMember("values")) return "stgs";
                    if (document.HasMember("textures"))
                    {
                        const std::string stem = ToLowerAscii(source.stem().string());
                        return stem.find("list") != std::string::npos ? "txls" : "matl";
                    }
                }
            }
            return {};
        }

        bool CommitImportSource(const fs::path& source, const std::string& type,
            const fs::path& preferredRelativePath = {})
        {
            const int typeIndex = AssetTypeIndex(type);
            if (typeIndex < 0)
                return false;
            newAssetTypeIndex_ = typeIndex;

            fs::path relativeSource = preferredRelativePath.empty()
                ? DefaultImportFolder(type) / source.filename()
                : preferredRelativePath;
            fs::path targetSource = repakProject_.AssetsDirectory() / relativeSource;
            for (int suffix = 2; fs::exists(targetSource); ++suffix)
            {
                relativeSource = relativeSource.parent_path() /
                    (source.stem().string() + "_" + std::to_string(suffix) + source.extension().string());
                targetSource = repakProject_.AssetsDirectory() / relativeSource;
            }

            std::error_code error;
            fs::create_directories(targetSource.parent_path(), error);
            if (!error) fs::copy_file(source, targetSource, fs::copy_options::none, error);
            if (error)
            {
                AppendRePakLog(firestar::repak::LogLevel::Error, "Import failed: " + error.message());
                return false;
            }
            if (type == "mdl_" && ToLowerAscii(source.extension().string()) == ".rmdl")
            {
                fs::path companion = source;
                companion.replace_extension(".vg");
                if (fs::is_regular_file(companion))
                {
                    fs::path targetCompanion = targetSource;
                    targetCompanion.replace_extension(".vg");
                    std::error_code companionError;
                    fs::copy_file(companion, targetCompanion, fs::copy_options::overwrite_existing, companionError);
                    if (companionError)
                        AppendRePakLog(firestar::repak::LogLevel::Warning,
                            "Could not copy model VG companion: " + companionError.message());
                }
            }
            const auto copyOptionalCompanion = [&](const char* const extension, const char* const description) {
                fs::path companion = source;
                companion.replace_extension(extension);
                if (companion == source || !fs::is_regular_file(companion)) return;
                fs::path targetCompanion = targetSource;
                targetCompanion.replace_extension(extension);
                std::error_code companionError;
                fs::copy_file(companion, targetCompanion, fs::copy_options::overwrite_existing, companionError);
                if (companionError)
                    AppendRePakLog(firestar::repak::LogLevel::Warning,
                        std::string("Could not copy ") + description + ": " + companionError.message());
            };
            if (type == "matl")
            {
                copyOptionalCompanion(".json", "material JSON companion");
                copyOptionalCompanion(".uber", "material UBER companion");
            }
            else if (type == "txtr")
                copyOptionalCompanion(".json", "texture metadata companion");
            else if (type == "aseq")
                copyOptionalCompanion(".json", "animation sequence metadata companion");

            fs::path assetRelative = relativeSource;
            assetRelative.replace_extension(".rpak");
            const std::string assetPath = NormalizeAssetPath(PathToUtf8(assetRelative));
            selectedAsset_ = static_cast<int>(repakProject_.AddAsset(type, assetPath));
            rapidjson::Value* asset = repakProject_.Asset(static_cast<size_t>(selectedAsset_));
            if (asset)
            {
                auto& allocator = repakProject_.Allocator();
                if (type == "awsr")
                {
                    const char* field = ToLowerAscii(source.extension().string()) == ".wav" ? "wav" : "ogg";
                    const std::string sourcePath = NormalizeAssetPath(PathToUtf8(relativeSource));
                    asset->AddMember(rapidjson::Value(field, allocator), rapidjson::Value(sourcePath.c_str(), allocator), allocator);
                }
                // RePak material entries only identify the .rpak asset. The material
                // definition itself remains in the matching .json sidecar and is
                // loaded by Material_OpenFile during the build. Copying those fields
                // into the manifest creates a misleading, non-canonical duplicate.
                if (type != "matl" && ToLowerAscii(source.extension().string()) == ".json")
                {
                    std::ifstream input(targetSource, std::ios::binary);
                    rapidjson::IStreamWrapper stream(input);
                    rapidjson::Document imported;
                    imported.ParseStream(stream);
                    if (imported.IsObject())
                    {
                        for (auto member = imported.MemberBegin(); member != imported.MemberEnd(); ++member)
                        {
                            if (strcmp(member->name.GetString(), "_type") == 0 || strcmp(member->name.GetString(), "_path") == 0 ||
                                asset->HasMember(member->name.GetString())) continue;
                            rapidjson::Value name(member->name, allocator);
                            rapidjson::Value value(member->value, allocator);
                            asset->AddMember(name, value, allocator);
                        }
                    }
                }
                if (type == "txtr" && !asset->HasMember("$disableStreaming"))
                    asset->AddMember("$disableStreaming", false, allocator);
            }
            repakProject_.MarkDirty();
            RefreshSourceData();
            AppendRePakLog(firestar::repak::LogLevel::Info,
                "Imported " + PathToUtf8(source.filename()) + " as " + type + " " + assetPath);
            SaveAfterAssetAdded();
            return true;
        }

        bool QueueOrImportSource(const fs::path& source, const std::string& type,
            const fs::path& preferredRelativePath = {})
        {
            const firestar::editor::AssetCompatibility compatibility =
                firestar::editor::InspectAssetCompatibility(source, type, repakProject_.PakVersion());
            if (!compatibility.compatible)
            {
                pendingImports_.push_back({source, type, preferredRelativePath, compatibility});
                AppendRePakLog(firestar::repak::LogLevel::Warning,
                    "Skipped incompatible " + PathToUtf8(source.filename()) + ": " + compatibility.message);
                return false;
            }
            return CommitImportSource(source, type, preferredRelativePath);
        }

        bool ImportSourceAsset(const HWND window)
        {
            const std::vector<fs::path> selected = ChooseAnySourceFiles(window,
                L"Import RPAK source assets", true);
            if (selected.empty())
                return false;

            pendingImports_.clear();
            stagedSourceImports_.clear();
            importSummary_.clear();
            size_t unrecognizedCount{};
            size_t companionCount{};
            for (const fs::path& source : selected)
            {
                const std::string extension = ToLowerAscii(source.extension().string());
                if (extension == ".uber")
                {
                    fs::path json = source;
                    json.replace_extension(".json");
                    if (std::any_of(selected.begin(), selected.end(), [&](const fs::path& choice) {
                        return choice.lexically_normal() == json.lexically_normal();
                    }))
                    {
                        ++companionCount;
                        continue;
                    }
                }
                if (extension == ".json")
                {
                    bool selectedWithPrimary{};
                    for (const char* primaryExtension : {".dds", ".rseq"})
                    {
                        fs::path primary = source;
                        primary.replace_extension(primaryExtension);
                        selectedWithPrimary = std::any_of(selected.begin(), selected.end(), [&](const fs::path& choice) {
                            return choice.lexically_normal() == primary.lexically_normal();
                        });
                        if (selectedWithPrimary) break;
                    }
                    if (selectedWithPrimary)
                    {
                        ++companionCount;
                        continue;
                    }
                }

                const std::string type = InferAssetType(source);
                if (AssetTypeIndex(type) < 0)
                {
                    ++unrecognizedCount;
                    AppendRePakLog(firestar::repak::LogLevel::Error,
                        "Firestar could not determine an RPAK type for " + PathToUtf8(source.filename()));
                    continue;
                }
                StagedSourceImport pending;
                pending.source = source;
                pending.type = type;
                pending.relativePath = NormalizeAssetPath(PathToUtf8(DefaultImportFolder(type) / source.filename()));
                pending.compatibility = firestar::editor::InspectAssetCompatibility(
                    source, type, repakProject_.PakVersion());
                stagedSourceImports_.push_back(std::move(pending));
            }

            importSummary_ = "Review the destination path";
            if (stagedSourceImports_.size() != 1) importSummary_ += "s";
            importSummary_ += " before importing.";
            if (companionCount)
                importSummary_ += " " + std::to_string(companionCount) + " selected companion file" +
                    (companionCount == 1 ? " will be" : "s will be") + " brought in with its material or asset.";
            if (unrecognizedCount)
                importSummary_ += " " + std::to_string(unrecognizedCount) + " unrecognized file" +
                    (unrecognizedCount == 1 ? " was" : "s were") + " ignored.";
            return !stagedSourceImports_.empty();
        }

        void ImportAssetFolder(const HWND window)
        {
            const auto folder = ChooseFolder(window, L"Choose an asset folder to import");
            if (!folder)
                return;
            pendingImports_.clear();
            importSummary_.clear();

            std::vector<fs::path> sources;
            std::error_code scanError;
            for (fs::recursive_directory_iterator iterator(*folder, fs::directory_options::skip_permission_denied, scanError), end;
                 iterator != end && !scanError; iterator.increment(scanError))
            {
                if (iterator->is_regular_file(scanError)) sources.push_back(iterator->path());
            }
            std::sort(sources.begin(), sources.end());
            size_t importedCount = 0;
            size_t unrecognizedCount = 0;
            for (const fs::path& source : sources)
            {
                const std::string type = InferAssetType(source);
                if (AssetTypeIndex(type) < 0)
                {
                    const std::string extension = ToLowerAscii(source.extension().string());
                    if (extension != ".vg" && extension != ".phy") ++unrecognizedCount;
                    continue;
                }
                std::error_code relativeError;
                const fs::path sourceRelative = fs::relative(source, *folder, relativeError);
                const fs::path preferred = NormalizeImportedRelativePath(
                    relativeError ? source.filename() : sourceRelative, type);
                if (QueueOrImportSource(source, type, preferred)) ++importedCount;
            }
            if (scanError)
                AppendRePakLog(firestar::repak::LogLevel::Warning, "Folder scan stopped early: " + scanError.message());
            importSummary_ = "Imported " + std::to_string(importedCount) + " asset" + (importedCount == 1 ? "" : "s") + ".";
            if (!pendingImports_.empty())
                importSummary_ += " " + std::to_string(pendingImports_.size()) + " incompatible file" +
                    (pendingImports_.size() == 1 ? " was" : "s were") + " skipped.";
            if (unrecognizedCount)
                importSummary_ += " " + std::to_string(unrecognizedCount) + " unrecognized file" +
                    (unrecognizedCount == 1 ? " was" : "s were") + " ignored.";
            AppendRePakLog(firestar::repak::LogLevel::Info, importSummary_);
        }

        bool DrawJsonValueEditor(const char* const label, rapidjson::Value& value,
            rapidjson::Document::AllocatorType& allocator, const int depth = 0)
        {
            if (depth > 5)
            {
                ImGui::TextDisabled("%s: nested data", label);
                return false;
            }
            bool changed = false;
            ImGui::PushID(&value);
            if (value.IsString())
            {
                std::string text(value.GetString(), value.GetStringLength());
                ImGui::SetNextItemWidth(-1.0f);
                if (InputTextString(label, text, 2048))
                {
                    value.SetString(text.c_str(), static_cast<rapidjson::SizeType>(text.size()), allocator);
                    changed = true;
                }
            }
            else if (value.IsBool())
            {
                bool current = value.GetBool();
                if (ImGui::Checkbox(label, &current))
                {
                    value.SetBool(current);
                    changed = true;
                }
            }
            else if (value.IsInt())
            {
                int current = value.GetInt();
                if (ImGui::InputInt(label, &current))
                {
                    value.SetInt(current);
                    changed = true;
                }
            }
            else if (value.IsUint64())
            {
                std::uint64_t current = value.GetUint64();
                if (ImGui::InputScalar(label, ImGuiDataType_U64, &current))
                {
                    value.SetUint64(current);
                    changed = true;
                }
            }
            else if (value.IsNumber())
            {
                double current = value.GetDouble();
                if (ImGui::InputDouble(label, &current))
                {
                    value.SetDouble(current);
                    changed = true;
                }
            }
            else if (value.IsArray())
            {
                if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen, "%s [%zu]", label, value.Size()))
                {
                    for (rapidjson::SizeType index = 0; index < value.Size(); ++index)
                    {
                        ImGui::PushID(static_cast<int>(index));
                        const std::string itemLabel = "[" + std::to_string(index) + "]";
                        changed |= DrawJsonValueEditor(itemLabel.c_str(), value[index], allocator, depth + 1);
                        ImGui::PopID();
                    }
                    if (ImGui::SmallButton("+ string"))
                    {
                        value.PushBack(rapidjson::Value("", allocator), allocator);
                        changed = true;
                    }
                    ImGui::TreePop();
                }
            }
            else if (value.IsObject())
            {
                if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen))
                {
                    for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member)
                        changed |= DrawJsonValueEditor(member->name.GetString(), member->value, allocator, depth + 1);
                    ImGui::TreePop();
                }
            }
            else
            {
                ImGui::TextDisabled("%s: null", label);
            }
            ImGui::PopID();
            return changed;
        }

        bool HomeCard(const char* const id, const char* const title, const char* const description,
            const ImVec2 size = ImVec2(300.0f, 118.0f))
        {
            ImGui::PushID(id);
            ImGui::BeginChild("card", size, ImGuiChildFlags_Borders);
            ImGui::TextColored(ImVec4(1.0f, 0.60f, 0.23f, 1.0f), "%s", title);
            ImGui::Spacing();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + size.x - 30.0f);
            ImGui::TextDisabled("%s", description);
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
            const bool hovered = ImGui::IsItemHovered();
            const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
            if (hovered)
            {
                const ImVec2 minimum = ImGui::GetItemRectMin();
                const ImVec2 maximum = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRect(minimum, maximum,
                    ImGui::GetColorU32(ImVec4(1.0f, 0.60f, 0.23f, 1.0f)),
                    ImGui::GetStyle().ChildRounding, 0, 2.0f);
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            ImGui::PopID();
            return clicked;
        }

        void DrawEditorRoot(const HWND window)
        {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
            constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
            ImGui::Begin("Firestar ReEtched Workspace", nullptr, flags);

            if (workspaceMode_ == WorkspaceMode::Home)
                DrawHome(window);
            else if (workspaceMode_ == WorkspaceMode::RePakProject)
                DrawRePakProject(window);
            if (showEditorError_)
                ImGui::OpenPopup("Firestar error");
            if (ImGui::BeginPopupModal("Firestar error", &showEditorError_, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextWrapped("%s", editorError_.c_str());
                ImGui::Separator();
                if (ImGui::Button("OK", ImVec2(100.0f, 0.0f)))
                {
                    showEditorError_ = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (showPackedProjectPrompt_)
                ImGui::OpenPopup("Open packed Firestar project");
            ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopupModal("Open packed Firestar project", &showPackedProjectPrompt_,
                ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted("This is a packed Firestar project.");
                ImGui::TextWrapped("Firestar will extract its project and assets before opening it.");
                if (pendingPackedProjectPath_)
                    ImGui::TextDisabled("%s", PathToUtf8(*pendingPackedProjectPath_).c_str());
                ImGui::Separator();
                if (ImGui::Button("Choose extraction location...", ImVec2(220.0f, 0.0f)))
                {
                    if (pendingPackedProjectPath_)
                    {
                        if (const auto parent = ChooseFolder(window, L"Choose where to extract the packed Firestar project"))
                        {
                            const fs::path destination = *parent /
                                PathFromUtf8(FileStemSafe(PathToUtf8(pendingPackedProjectPath_->stem())));
                            fs::path extractedProject;
                            std::string error;
                            if (firestar::editor::ExtractPackedProject(*pendingPackedProjectPath_,
                                destination, extractedProject, error) && LoadFirestarProject(extractedProject))
                            {
                                AppendRePakLog(firestar::repak::LogLevel::Info,
                                    "Extracted packed project to " + PathToUtf8(destination) + ".");
                                showPackedProjectPrompt_ = false;
                                pendingPackedProjectPath_.reset();
                                ImGui::CloseCurrentPopup();
                            }
                            else if (!error.empty())
                            {
                                editorError_ = error;
                                showEditorError_ = true;
                            }
                        }
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
                {
                    showPackedProjectPrompt_ = false;
                    pendingPackedProjectPath_.reset();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (showHomeSavePrompt_)
                ImGui::OpenPopup("Save project before going Home?");
            if (ImGui::BeginPopupModal("Save project before going Home?", &showHomeSavePrompt_,
                ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted("This project has unsaved changes.");
                ImGui::TextDisabled("Save them before returning to Home?");
                ImGui::Separator();
                if (ImGui::Button("Save", ImVec2(110.0f, 0.0f)))
                {
                    if (SaveFirestarProject(window))
                    {
                        showHomeSavePrompt_ = false;
                        workspaceMode_ = WorkspaceMode::Home;
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Don't save", ImVec2(110.0f, 0.0f)))
                {
                    showHomeSavePrompt_ = false;
                    workspaceMode_ = WorkspaceMode::Home;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)))
                {
                    showHomeSavePrompt_ = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::End();
        }

        void DrawHome(const HWND window)
        {
            ImGui::Dummy(ImVec2(0.0f, 34.0f));
            const float contentWidth = 632.0f;
            const float contentLeft = (std::max)(20.0f, (ImGui::GetWindowWidth() - contentWidth) * 0.5f);
            ImGui::SetCursorPosX(contentLeft);
            if (g_headingFont) ImGui::PushFont(g_headingFont);
            ImGui::TextColored(ImVec4(1.0f, 0.60f, 0.23f, 1.0f), "FIRESTAR");
            if (g_headingFont) ImGui::PopFont();
            ImGui::SetCursorPosX(contentLeft);
            ImGui::TextDisabled("Apex Legends RPAK authoring workspace");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::SetCursorPosX(contentLeft);
            if (ImGui::BeginTable("HomeActions", 2, ImGuiTableFlags_None, ImVec2(contentWidth, 0.0f)))
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (HomeCard("new_rpak", "New RPAK", "Create a Firestar project with asset and build folders."))
                    CreateRePakProject(window);
                ImGui::TableNextColumn();
                if (HomeCard("open_project", "Open Firestar Project", "Reopen an editable .fsp project with its RePak JSON stored inside."))
                    if (const auto path = ChooseFirestarProjectFile(window, false)) LoadFirestarProject(*path);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (HomeCard("open_rpak", "Import RePak JSON", "Import an existing build map, inspect its assets, and save it as a Firestar project."))
                    if (const auto path = ChooseRePakManifest(window, false)) LoadRePakProject(*path);
                ImGui::TableNextColumn();
                if (HomeCard("atlas", "UI Atlas", "Pack sprites, preview the atlas, and add its texture and UIMG assets to a project."))
                    OpenAtlasWindow();
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (HomeCard("mod_creator", "Mod Creator", "Create a clean mod folder and metadata, ready to receive built RPaks."))
                    showModCreatorWindow_ = true;
                ImGui::TableNextColumn();
                if (HomeCard("open_packed", "Open Packed Project", "Extract and open a shareable .fspa project archive."))
                    if (const auto path = ChoosePackedProjectFile(window, false)) OpenPackedProject(*path);
                ImGui::EndTable();
            }
        }

        void DrawRePakProject(const HWND window)
        {
            if (!repakProject_.IsOpen())
            {
                workspaceMode_ = WorkspaceMode::Home;
                return;
            }

            ImGui::Text("%s%s", FirestarProjectName().c_str(), repakProject_.IsDirty() ? " *" : "");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", currentRePakProjectPath_.empty()
                ? "Unsaved Firestar project"
                : PathToUtf8(currentRePakProjectPath_).c_str());
            const float buildWidth = ImGui::CalcTextSize("Build RPAK").x + 28.0f;
            const float deployWidth = ImGui::CalcTextSize("Deploy").x + 28.0f;
            const float settingsWidth = ImGui::CalcTextSize("RPAK Settings").x + 28.0f;
            ImGui::SameLine((std::max)(ImGui::GetCursorPosX(),
                ImGui::GetWindowContentRegionMax().x - buildWidth - deployWidth - settingsWidth -
                ImGui::GetStyle().ItemSpacing.x * 2.0f));
            if (ImGui::Button("RPAK Settings")) showRePakSettings_ = true;
            ImGui::SameLine();
            ImGui::BeginDisabled(repakBuilding_ || repakProject_.IsDirty() || !fs::is_regular_file(BuiltPakPath()));
            if (ImGui::Button("Deploy"))
            {
                deployError_.clear();
                showDeployConfirm_ = true;
                ImGui::OpenPopup("Deploy RPAK");
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(repakBuilding_);
            if (ImGui::Button(repakBuilding_ ? "Building..." : "Build RPAK"))
                StartRePakBuild();
            ImGui::EndDisabled();
            if (showDeployConfirm_)
                ImGui::OpenPopup("Deploy RPAK");
            if (ImGui::BeginPopupModal("Deploy RPAK", &showDeployConfirm_, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted("Copy the built RPAK and add it to preload.rson?");
                ImGui::Separator();
                ImGui::Text("Source: %s", PathToUtf8(BuiltPakPath()).c_str());
                ImGui::Text("Target folder: %s", PathToUtf8(DeploymentDirectory()).c_str());
                const std::vector<fs::path> products = BuiltProducts();
                ImGui::Text("Files: %zu", products.size());
                for (const fs::path& product : products)
                    ImGui::BulletText("%s", PathToUtf8(product.filename()).c_str());
                ImGui::Text("Preload: %s", PathToUtf8(DeploymentDirectory() / L"preload.rson").c_str());
                if (!deployError_.empty())
                    ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.42f, 1.0f), "%s", deployError_.c_str());
                if (ImGui::Button("Deploy now"))
                {
                    if (DeployCurrentPak(deployError_))
                    {
                        showDeployConfirm_ = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    showDeployConfirm_ = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (repakProject_.IsBuildList())
            {
                ImGui::SetNextItemWidth(460.0f);
                const auto& pakPaths = repakProject_.PakManifestPaths();
                const size_t currentPak = repakProject_.CurrentPakIndex();
                const std::string currentLabel = currentPak < pakPaths.size()
                    ? PathToUtf8(pakPaths[currentPak].filename()) : std::string("Select RPAK");
                if (ImGui::BeginCombo("RPAK in build list", currentLabel.c_str()))
                {
                    for (size_t index = 0; index < pakPaths.size(); ++index)
                    {
                        const bool selected = index == currentPak;
                        const std::string label = PathToUtf8(pakPaths[index].filename());
                        if (ImGui::Selectable(label.c_str(), selected) && !selected)
                        {
                            std::string error;
                            if (repakProject_.SelectPak(index, error))
                            {
                                selectedAsset_ = repakProject_.AssetCount() ? 0 : -1;
                                RefreshSourceData();
                                AppendRePakLog(firestar::repak::LogLevel::Info,
                                    "Editing " + PathToUtf8(repakProject_.ManifestPath()));
                            }
                            else
                            {
                                editorError_ = error;
                                showEditorError_ = true;
                                AppendRePakLog(firestar::repak::LogLevel::Error, error);
                            }
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::Separator();

            const float workspaceHeight = (std::max)(360.0f, ImGui::GetContentRegionAvail().y);
            if (ImGui::BeginTable("RePakWorkspace", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                ImVec2(0.0f, workspaceHeight)))
            {
                ImGui::TableSetupColumn("Assets", ImGuiTableColumnFlags_WidthStretch, 0.34f);
                ImGui::TableSetupColumn("Editor", ImGuiTableColumnFlags_WidthStretch, 0.66f);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                constexpr float activityHeight = 155.0f;
                DrawAssetBrowser(window, (std::max)(190.0f, workspaceHeight - activityHeight - 26.0f));
                ImGui::SeparatorText("Build activity");
                DrawRePakBuildActivity((std::max)(95.0f, ImGui::GetContentRegionAvail().y));
                ImGui::TableNextColumn();
                if (ImGui::BeginTabBar("AssetEditorTabs"))
                {
                    if (ImGui::BeginTabItem("Inspector"))
                    {
                        DrawAssetInspector(window);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Source data"))
                    {
                        DrawSourceData(window);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Preview"))
                    {
                        DrawAssetPreview();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Asset JSON"))
                    {
                        const std::string json = selectedAsset_ >= 0
                            ? repakProject_.SerializeAsset(static_cast<size_t>(selectedAsset_)) : std::string{};
                        ImGui::BeginChild("RawAssetJson", ImVec2(0.0f, workspaceHeight - 36.0f), ImGuiChildFlags_Borders);
                        ImGui::TextUnformatted(json.c_str());
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                ImGui::EndTable();
            }
        }

        void DrawAssetBrowser(const HWND window, const float height)
        {
            ImGui::SetNextItemWidth(-92.0f);
            ImGui::InputTextWithHint("##asset_search", "Search type or path...", assetSearch_.data(), assetSearch_.size());
            ImGui::SameLine();
            if (ImGui::Button("+ Asset"))
            {
                pendingImports_.clear();
                stagedSourceImports_.clear();
                importSummary_.clear();
                ImGui::OpenPopup("Add RePak asset");
            }
            if (ImGui::BeginPopupModal("Add RePak asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                if (ImGui::Button("Choose source files..."))
                    (void)ImportSourceAsset(window);
                ImGui::SameLine();
                if (ImGui::Button("Import asset folder..."))
                    ImportAssetFolder(window);
                if (!importSummary_.empty())
                    ImGui::TextWrapped("%s", importSummary_.c_str());
                if (!stagedSourceImports_.empty())
                {
                    ImGui::SeparatorText("Import paths");
                    ImGui::TextDisabled("Paths are relative to the project's assets folder.");
                    bool hasIncompatible{};
                    bool validPaths = true;
                    if (ImGui::BeginTable("StagedSourceImports", 3,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable,
                        ImVec2(780.0f, 0.0f)))
                    {
                        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthFixed, 210.0f);
                        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                        ImGui::TableSetupColumn("Source path under assets");
                        ImGui::TableHeadersRow();
                        for (size_t index = 0; index < stagedSourceImports_.size(); ++index)
                        {
                            StagedSourceImport& pending = stagedSourceImports_[index];
                            hasIncompatible |= !pending.compatibility.compatible;
                            const fs::path relative = PathFromUtf8(pending.relativePath);
                            bool pathIsValid = !pending.relativePath.empty() && !relative.is_absolute();
                            for (const fs::path& component : relative)
                                if (component == L"..") pathIsValid = false;
                            validPaths &= pathIsValid;
                            ImGui::PushID(static_cast<int>(index));
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(PathToUtf8(pending.source.filename()).c_str());
                            if (!pending.compatibility.compatible)
                            {
                                ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.42f, 1.0f), "Incompatible");
                                if (ImGui::IsItemHovered())
                                    ImGui::SetTooltip("%s", pending.compatibility.message.c_str());
                            }
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(pending.type.c_str());
                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(-1.0f);
                            (void)InputTextString("##relative_path", pending.relativePath, 1024);
                            if (!pathIsValid)
                                ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.42f, 1.0f),
                                    "Use a relative path inside assets.");
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    if (hasIncompatible)
                        ImGui::Checkbox("Import incompatible files anyway", &importIncompatibleSources_);
                    ImGui::BeginDisabled(!validPaths || (hasIncompatible && !importIncompatibleSources_));
                    if (ImGui::Button("Import files"))
                    {
                        size_t imported{};
                        for (const StagedSourceImport& pending : stagedSourceImports_)
                            if (CommitImportSource(pending.source, pending.type,
                                PathFromUtf8(pending.relativePath))) ++imported;
                        importSummary_ = "Imported " + std::to_string(imported) + " asset" +
                            (imported == 1 ? "." : "s.");
                        stagedSourceImports_.clear();
                        importIncompatibleSources_ = false;
                        if (imported) ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::Button("Clear selection"))
                    {
                        stagedSourceImports_.clear();
                        importIncompatibleSources_ = false;
                    }
                }
                if (!pendingImports_.empty())
                {
                    ImGui::SeparatorText("Incompatible files");
                    ImGui::BeginChild("IncompatibleImports", ImVec2(640.0f, 150.0f), ImGuiChildFlags_Borders);
                    for (const PendingImport& pending : pendingImports_)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.42f, 1.0f), "%s [%s]",
                            PathToUtf8(pending.source.filename()).c_str(), pending.type.c_str());
                        ImGui::TextDisabled("%s", pending.compatibility.detectedVersion.c_str());
                        ImGui::TextWrapped("%s", pending.compatibility.message.c_str());
                        ImGui::Separator();
                    }
                    ImGui::EndChild();
                    if (ImGui::Button("Import anyway"))
                    {
                        size_t imported = 0;
                        for (const PendingImport& pending : pendingImports_)
                            if (CommitImportSource(pending.source, pending.type, pending.preferredRelativePath)) ++imported;
                        importSummary_ = "Imported " + std::to_string(imported) + " incompatible file" +
                            (imported == 1 ? "." : "s.");
                        pendingImports_.clear();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Keep skipped"))
                    {
                        pendingImports_.clear();
                        importSummary_ = "The incompatible files were not imported.";
                    }
                }
                ImGui::Separator();
                const RePakAssetTypeOption& selectedType = RePakAssetTypes[static_cast<size_t>(newAssetTypeIndex_)];
                const std::string preview = std::string(selectedType.code) + " - " + selectedType.name;
                if (ImGui::BeginCombo("Type", preview.c_str()))
                {
                    for (size_t typeIndex = 0; typeIndex < RePakAssetTypes.size(); ++typeIndex)
                    {
                        const auto& option = RePakAssetTypes[typeIndex];
                        const std::string label = std::string(option.code) + " - " + option.name;
                        if (ImGui::Selectable(label.c_str(), newAssetTypeIndex_ == static_cast<int>(typeIndex)))
                            newAssetTypeIndex_ = static_cast<int>(typeIndex);
                    }
                    ImGui::EndCombo();
                }
                ImGui::InputText("Asset path", newAssetPath_.data(), newAssetPath_.size());
                const bool valid = newAssetPath_[0] != '\0';
                ImGui::BeginDisabled(!valid);
                if (ImGui::Button("Add"))
                {
                    const std::string type = RePakAssetTypes[static_cast<size_t>(newAssetTypeIndex_)].code;
                    selectedAsset_ = static_cast<int>(repakProject_.AddAsset(type, newAssetPath_.data()));
                    if (type == "matl")
                    {
                        std::string error;
                        if (!CreateBlankMaterialSources(static_cast<size_t>(selectedAsset_), error))
                            AppendRePakLog(firestar::repak::LogLevel::Error, error);
                    }
                    SaveAfterAssetAdded();
                    newAssetPath_.fill('\0');
                    RefreshSourceData();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    pendingImports_.clear();
                    stagedSourceImports_.clear();
                    importSummary_.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::TextDisabled("%zu assets", repakProject_.AssetCount());
            ImGui::BeginChild("AssetBrowser", ImVec2(0.0f, height - 70.0f), ImGuiChildFlags_Borders);
            const std::string filter = ToLowerAscii(assetSearch_.data());
            int removeIndex = -1;
            int deleteIndex = -1;
            for (size_t index = 0; index < repakProject_.AssetCount(); ++index)
            {
                const rapidjson::Value* asset = repakProject_.Asset(index);
                if (!asset || !asset->IsObject()) continue;
                const std::string type = AssetMemberString(*asset, "_type");
                const std::string path = AssetMemberString(*asset, "_path");
                const std::string searchable = ToLowerAscii(type + " " + path);
                if (!filter.empty() && searchable.find(filter) == std::string::npos) continue;

                ImGui::PushID(static_cast<int>(index));
                const bool selected = selectedAsset_ == static_cast<int>(index);
                if (ImGui::Selectable(("[" + type + "]  " + path).c_str(), selected))
                {
                    selectedAsset_ = static_cast<int>(index);
                    RefreshSourceData();
                }
                if (ImGui::BeginPopupContextItem("Asset actions"))
                {
                    if (type != "aevt" && ImGui::MenuItem("Replace source..."))
                    {
                        selectedAsset_ = static_cast<int>(index);
                        ReplaceSelectedSource(window);
                    }
                    if (type != "aevt" && ImGui::MenuItem("Reveal source in Explorer"))
                    {
                        if (const auto source = repakProject_.ResolvePrimarySource(index))
                            ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + source->wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
                    }
                    if (type == "mdl_" || type == "arig" || type == "aevt")
                    {
                        ImGui::Separator();
                        if (type == "mdl_" && ImGui::MenuItem("Add rig..."))
                        {
                            selectedAsset_ = static_cast<int>(index);
                            PrepareNearbyAssets("$animrigs", {".rrig"});
                        }
                        if ((type == "mdl_" || type == "arig") && ImGui::MenuItem("Add sequence..."))
                        {
                            selectedAsset_ = static_cast<int>(index);
                            PrepareNearbyAssets("$sequences", {".rseq"});
                        }
                        if (type == "aevt" && ImGui::MenuItem("Add audio source..."))
                        {
                            selectedAsset_ = static_cast<int>(index);
                            PrepareProjectAssetReferences("sources", "awsr");
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Remove from RPAK")) removeIndex = static_cast<int>(index);
                    if (type != "aevt" && ImGui::MenuItem("Delete source files...")) deleteIndex = static_cast<int>(index);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            if (removeIndex >= 0)
                RemoveAssetFromProject(static_cast<size_t>(removeIndex));
            if (deleteIndex >= 0)
                RequestDeleteAssetSources(static_cast<size_t>(deleteIndex));
            ImGui::EndChild();

            if (openDeleteAssetPopup_)
            {
                ImGui::OpenPopup("Delete asset source files");
                openDeleteAssetPopup_ = false;
            }
            if (ImGui::BeginPopupModal("Delete asset source files", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted("Delete these files from the project's assets folder?");
                ImGui::TextDisabled("The asset will also be removed from the RePak JSON.");
                ImGui::Separator();
                if (pendingDeleteSourceFiles_.empty())
                    ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.42f, 1.0f), "No source files were found to delete.");
                for (const fs::path& source : pendingDeleteSourceFiles_)
                    ImGui::BulletText("%s", PathToUtf8(source).c_str());
                ImGui::BeginDisabled(pendingDeleteAsset_ < 0 || pendingDeleteSourceFiles_.empty());
                if (ImGui::Button("Delete files and remove asset"))
                {
                    bool removedAll = true;
                    for (const fs::path& source : pendingDeleteSourceFiles_)
                    {
                        std::error_code error;
                        if (!fs::remove(source, error) || error)
                        {
                            removedAll = false;
                            AppendRePakLog(firestar::repak::LogLevel::Error,
                                "Could not delete " + PathToUtf8(source) +
                                (error ? ": " + error.message() : "."));
                        }
                        else AppendRePakLog(firestar::repak::LogLevel::Info,
                            "Deleted " + PathToUtf8(source));
                    }
                    if (removedAll)
                    {
                        RemoveAssetFromProject(static_cast<size_t>(pendingDeleteAsset_));
                        pendingDeleteAsset_ = -1;
                        pendingDeleteSourceFiles_.clear();
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    pendingDeleteAsset_ = -1;
                    pendingDeleteSourceFiles_.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            if (openAssetReferencePopup_)
            {
                ImGui::OpenPopup("Add asset reference");
                openAssetReferencePopup_ = false;
            }
            if (ImGui::BeginPopupModal("Add asset reference", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("Choose assets for %s", nearbyTargetField_.c_str());
                ImGui::TextDisabled("Tick the references to include, then press Done.");
                ImGui::SetNextItemWidth(520.0f);
                ImGui::InputTextWithHint("##reference_search", "Search paths...",
                    assetReferenceSearch_.data(), assetReferenceSearch_.size());
                const std::string referenceFilter = ToLowerAscii(assetReferenceSearch_.data());
                size_t visibleReferenceCount{};
                ImGui::BeginChild("NearbyList", ImVec2(540.0f, 240.0f), ImGuiChildFlags_Borders);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
                ImGui::PushID("nearby");
                for (const fs::path& choice : nearbyAssetChoices_)
                {
                    std::error_code relativeError;
                    const fs::path relative = fs::relative(choice, repakProject_.AssetsDirectory(), relativeError);
                    const std::string label = PathToUtf8(relativeError ? choice : relative);
                    if (!referenceFilter.empty() && ToLowerAscii(label).find(referenceFilter) == std::string::npos)
                        continue;
                    ++visibleReferenceCount;
                    const std::string reference = NormalizeAssetPath(label);
                    bool included = repakProject_.AssetArrayContains(assetReferenceTargetIndex_,
                        nearbyTargetField_.c_str(), reference);
                    ImGui::PushID(reference.c_str());
                    if (ImGui::Checkbox("##included", &included))
                    {
                        if (repakProject_.SetStringInAssetArray(assetReferenceTargetIndex_,
                            nearbyTargetField_.c_str(), reference, included))
                        {
                            selectedAsset_ = static_cast<int>(assetReferenceTargetIndex_);
                            RefreshSourceData();
                            AppendRePakLog(firestar::repak::LogLevel::Info,
                                std::string(included ? "Added " : "Removed ") + reference +
                                (included ? " to " : " from ") + nearbyTargetField_ + ".");
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted(label.c_str());
                    ImGui::PopID();
                }
                ImGui::PopID();
                ImGui::PushID("project");
                for (const std::string& choice : projectAssetReferenceChoices_)
                {
                    if (!referenceFilter.empty() && ToLowerAscii(choice).find(referenceFilter) == std::string::npos)
                        continue;
                    ++visibleReferenceCount;
                    bool included = repakProject_.AssetArrayContains(assetReferenceTargetIndex_,
                        nearbyTargetField_.c_str(), choice);
                    ImGui::PushID(choice.c_str());
                    if (ImGui::Checkbox("##included", &included))
                    {
                        if (repakProject_.SetStringInAssetArray(assetReferenceTargetIndex_,
                            nearbyTargetField_.c_str(), choice, included))
                        {
                            selectedAsset_ = static_cast<int>(assetReferenceTargetIndex_);
                            RefreshSourceData();
                            AppendRePakLog(firestar::repak::LogLevel::Info,
                                std::string(included ? "Added " : "Removed ") + choice +
                                (included ? " to " : " from ") + nearbyTargetField_ + ".");
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted(choice.c_str());
                    ImGui::PopID();
                }
                ImGui::PopID();
                ImGui::PopStyleVar();
                ImGui::EndChild();
                if (nearbyAssetChoices_.empty() && projectAssetReferenceChoices_.empty())
                    ImGui::TextDisabled("No matching assets were found.");
                else if (visibleReferenceCount == 0)
                    ImGui::TextDisabled("No paths match the search.");
                if (ImGui::Button("Done")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }

        void DrawAssetInspector(const HWND window)
        {
            if (selectedAsset_ < 0)
            {
                ImGui::TextDisabled("Select an asset to inspect it.");
                return;
            }
            rapidjson::Value* asset = repakProject_.Asset(static_cast<size_t>(selectedAsset_));
            if (!asset || !asset->IsObject())
                return;
            const std::string type = AssetMemberString(*asset, "_type");
            const std::string path = AssetMemberString(*asset, "_path");
            ImGui::Text("%s", path.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", type.c_str());
            const std::uint64_t guid = AssetGuid(*asset);
            ImGui::SeparatorText("Identity");
            if (ImGui::BeginTable("AssetIdentity", 2, ImGuiTableFlags_BordersInnerH))
            {
                ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 95.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextDisabled("Type");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(type.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextDisabled("GUID");
                ImGui::TableNextColumn();
                ImGui::Text("0x%016llX", static_cast<unsigned long long>(guid));
                ImGui::SameLine();
                if (ImGui::SmallButton("Copy"))
                {
                    char text[32]{};
                    std::snprintf(text, sizeof(text), "0x%016llX", static_cast<unsigned long long>(guid));
                    ImGui::SetClipboardText(text);
                }
                ImGui::EndTable();
            }
            if (type != "aevt")
            {
                if (ImGui::Button("Replace source...")) ReplaceSelectedSource(window);
                ImGui::SameLine();
                if (ImGui::Button("Refresh data")) RefreshSourceData();
                ImGui::SameLine();
            }
            if (ImGui::Button("Remove from RPAK"))
            {
                RemoveAssetFromProject(static_cast<size_t>(selectedAsset_));
                return;
            }
            if (type != "aevt")
            {
                ImGui::SameLine();
                if (ImGui::Button("Delete source files..."))
                    RequestDeleteAssetSources(static_cast<size_t>(selectedAsset_));
            }
            if (type == "matl")
            {
                ImGui::SameLine();
                ImGui::TextDisabled("Texture paths and GUIDs can be edited in arrays below or in the material sidecar.");
            }
            if (pendingReplacement_ && pendingReplacement_->assetIndex == static_cast<size_t>(selectedAsset_))
            {
                ImGui::SeparatorText("Replacement warning");
                ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.42f, 1.0f), "%s is incompatible",
                    PathToUtf8(pendingReplacement_->source.filename()).c_str());
                ImGui::Text("Detected: %s", pendingReplacement_->compatibility.detectedVersion.c_str());
                ImGui::TextWrapped("%s", pendingReplacement_->compatibility.message.c_str());
                if (ImGui::Button("Replace anyway"))
                {
                    CommitSourceReplacement(*pendingReplacement_);
                    pendingReplacement_.reset();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel replacement")) pendingReplacement_.reset();
            }
            const auto source = repakProject_.ResolvePrimarySource(static_cast<size_t>(selectedAsset_));
            if (source && fs::is_regular_file(*source))
            {
                const firestar::editor::AssetCompatibility compatibility =
                    firestar::editor::InspectAssetCompatibility(*source, type, repakProject_.PakVersion());
                ImGui::SeparatorText("Compatibility");
                const ImVec4 color = compatibility.compatible
                    ? ImVec4(0.60f, 0.92f, 0.68f, 1.0f) : ImVec4(1.0f, 0.48f, 0.42f, 1.0f);
                ImGui::TextColored(color, "%s with RPAK %d",
                    compatibility.compatible ? "Compatible" : "Incompatible", repakProject_.PakVersion());
                ImGui::Text("Detected: %s", compatibility.detectedVersion.c_str());
                ImGui::TextWrapped("%s", compatibility.message.c_str());
            }
            else if (type == "aevt")
            {
                ImGui::SeparatorText("Source");
                ImGui::TextDisabled("Audio events are manifest-only assets; there is no standalone AEVT source file.");
                ImGui::TextWrapped("Event settings and AWSR source references are stored directly in the RePak JSON.");
            }
            else if (type == "uimg")
            {
                ImGui::SeparatorText("Source");
                ImGui::TextDisabled("UI atlas metadata is stored directly in the RPAK manifest.");
            }
            else if (source)
            {
                ImGui::SeparatorText("Compatibility");
                ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.42f, 1.0f), "Source file missing");
                ImGui::TextWrapped("%s", PathToUtf8(*source).c_str());
            }

            const std::vector<AssetDependency> dependencies = SelectedAssetDependencies();
            if (!dependencies.empty())
            {
                ImGui::SeparatorText("Dependencies");
                if (ImGui::BeginTable("AssetDependencies", 4,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
                {
                    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                    ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("GUID", ImGuiTableColumnFlags_WidthFixed, 170.0f);
                    ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthFixed, 95.0f);
                    ImGui::TableHeadersRow();
                    for (const AssetDependency& dependency : dependencies)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(dependency.kind.c_str());
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(dependency.name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("0x%016llX", static_cast<unsigned long long>(dependency.guid));
                        ImGui::TableNextColumn();
                        if (dependency.assetIndex >= 0)
                        {
                            ImGui::PushID(dependency.assetIndex);
                            if (ImGui::SmallButton("Open"))
                            {
                                selectedAsset_ = dependency.assetIndex;
                                RefreshSourceData();
                            }
                            ImGui::PopID();
                        }
                        else ImGui::TextDisabled("External");
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::Separator();
            bool changed = false;
            for (auto member = asset->MemberBegin(); member != asset->MemberEnd(); ++member)
            {
                if (std::strcmp(member->name.GetString(), "_type") == 0)
                    continue;
                changed |= DrawJsonValueEditor(member->name.GetString(), member->value, repakProject_.Allocator());
            }
            if (changed)
                repakProject_.MarkDirty();
        }

        void DrawReadOnlyJson(const char* const label, const rapidjson::Value& value, const int depth = 0)
        {
            if (depth > 7)
            {
                ImGui::TextDisabled("%s: nested data", label);
                return;
            }
            ImGui::PushID(&value);
            if (value.IsObject())
            {
                if (ImGui::TreeNodeEx(label, depth < 2 ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None))
                {
                    for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member)
                        DrawReadOnlyJson(member->name.GetString(), member->value, depth + 1);
                    ImGui::TreePop();
                }
            }
            else if (value.IsArray())
            {
                if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen, "%s [%u]", label, value.Size()))
                {
                    for (rapidjson::SizeType index = 0; index < value.Size(); ++index)
                        DrawReadOnlyJson(("[" + std::to_string(index) + "]").c_str(), value[index], depth + 1);
                    ImGui::TreePop();
                }
            }
            else if (value.IsString()) ImGui::LabelText(label, "%s", value.GetString());
            else if (value.IsBool()) ImGui::LabelText(label, "%s", value.GetBool() ? "true" : "false");
            else if (value.IsInt64()) ImGui::LabelText(label, "%lld", static_cast<long long>(value.GetInt64()));
            else if (value.IsUint64()) ImGui::LabelText(label, "%llu", static_cast<unsigned long long>(value.GetUint64()));
            else if (value.IsNumber()) ImGui::LabelText(label, "%g", value.GetDouble());
            else ImGui::LabelText(label, "null");
            ImGui::PopID();
        }

        static bool WriteJsonDocument(const fs::path& path, const rapidjson::Document& document,
            std::string& error)
        {
            const fs::path backup = path.wstring() + L".firestar.bak";
            std::error_code ioError;
            if (fs::is_regular_file(path) && !fs::exists(backup))
                fs::copy_file(path, backup, fs::copy_options::none, ioError);
            ioError.clear();
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                error = "Could not write " + PathToUtf8(path);
                return false;
            }
            rapidjson::OStreamWrapper stream(output);
            rapidjson::PrettyWriter<rapidjson::OStreamWrapper> writer(stream);
            writer.SetIndent(' ', 4);
            document.Accept(writer);
            output << '\n';
            if (!output.good())
            {
                error = "The material JSON write was incomplete.";
                return false;
            }
            return true;
        }

        bool CreateBlankMaterialSources(const size_t assetIndex, std::string& error)
        {
            const rapidjson::Value* asset = repakProject_.Asset(assetIndex);
            if (!asset || AssetMemberString(*asset, "_type") != "matl")
            {
                error = "The selected asset is not a material.";
                return false;
            }
            fs::path base = repakProject_.AssetsDirectory() /
                PathFromUtf8(AssetMemberString(*asset, "_path"));
            base.replace_extension();
            fs::path jsonPath = base;
            fs::path uberPath = base;
            jsonPath.replace_extension(".json");
            uberPath.replace_extension(".uber");
            std::error_code ioError;
            fs::create_directories(base.parent_path(), ioError);
            if (ioError)
            {
                error = "Could not create the material folder: " + ioError.message();
                return false;
            }
            if (!fs::exists(jsonPath))
            {
                std::string materialName = NormalizeAssetPath(PathToUtf8(base.lexically_relative(
                    repakProject_.AssetsDirectory())));
                if (materialName.starts_with("material/")) materialName.erase(0, 9);
                std::ofstream json(jsonPath, std::ios::binary | std::ios::trunc);
                json << "{\n"
                    << "    \"name\": \"" << JsonEscape(materialName) << "\",\n"
                    << "    \"width\": 1024,\n    \"height\": 1024,\n    \"depth\": 0,\n"
                    << "    \"glueFlags\": \"0x56000020\",\n    \"glueFlags2\": \"0x100000\",\n"
                    << "    \"blendStates\": [\"0xF0000000\", \"0xF0000000\", \"0xF0000000\", \"0xF0000000\", \"0xF0000000\", \"0xF0000000\", \"0xF0000000\", \"0xF0000000\"],\n"
                    << "    \"blendStateMask\": \"0x4\",\n    \"depthStencilFlags\": \"0x17\",\n"
                    << "    \"rasterizerFlags\": \"0x6\",\n    \"uberBufferFlags\": \"0x0\",\n"
                    << "    \"features\": \"0x0\",\n    \"samplers\": \"0x0\",\n"
                    << "    \"surfaceProp\": \"default\",\n    \"surfaceProp2\": \"\",\n"
                    << "    \"shaderType\": \"sknp\",\n    \"shaderSet\": \"0x0\",\n"
                    << "    \"$textures\": {},\n    \"$textureTypes\": {},\n"
                    << "    \"$depthShadowMaterial\": \"0x0\",\n    \"$depthPrepassMaterial\": \"0x0\",\n"
                    << "    \"$depthVSMMaterial\": \"0x0\",\n    \"$depthShadowTightMaterial\": \"0x0\",\n"
                    << "    \"$colpassMaterial\": \"0x0\",\n    \"$textureAnimation\": \"0x0\"\n}\n";
                if (!json.good())
                {
                    error = "Could not write the material JSON companion.";
                    return false;
                }
            }
            if (!fs::exists(uberPath))
            {
                const size_t uberSize = repakProject_.PakVersion() >= 8 ? 512 : 224;
                std::vector<std::uint8_t> bytes(uberSize, 0);
                std::ofstream uber(uberPath, std::ios::binary | std::ios::trunc);
                uber.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!uber.good())
                {
                    error = "Could not write the material UBER companion.";
                    return false;
                }
            }
            return true;
        }

        void DrawMaterialSourceData()
        {
            const auto primary = repakProject_.ResolvePrimarySource(static_cast<size_t>(selectedAsset_));
            if (!primary)
            {
                ImGui::TextDisabled("No material source path could be resolved.");
                return;
            }
            fs::path jsonPath = *primary;
            fs::path uberPath = *primary;
            jsonPath.replace_extension(".json");
            uberPath.replace_extension(".uber");
            if (!fs::is_regular_file(jsonPath) || !fs::is_regular_file(uberPath))
            {
                if (ImGui::Button("Create missing material files"))
                {
                    std::string error;
                    if (CreateBlankMaterialSources(static_cast<size_t>(selectedAsset_), error))
                    {
                        RefreshSourceData();
                        AppendRePakLog(firestar::repak::LogLevel::Info,
                            "Created the material JSON and UBER companions.");
                    }
                    else AppendRePakLog(firestar::repak::LogLevel::Error, error);
                }
            }
            if (!ImGui::BeginTabBar("MaterialSourceTabs")) return;
            if (ImGui::BeginTabItem("Material JSON"))
            {
                ImGui::TextDisabled("%s", PathToUtf8(jsonPath).c_str());
                if (!fs::is_regular_file(jsonPath))
                    ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.42f, 1.0f), "Material JSON companion is missing.");
                else
                {
                    std::ifstream input(jsonPath, std::ios::binary);
                    rapidjson::IStreamWrapper stream(input);
                    rapidjson::Document material;
                    material.ParseStream(stream);
                    if (!material.IsObject())
                        ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.42f, 1.0f), "The material JSON is invalid.");
                    else
                    {
                        bool changed{};
                        ImGui::SeparatorText("Material");
                        for (const char* field : {"name", "surfaceProp", "surfaceProp2", "shaderType", "shaderSet"})
                        {
                            auto member = material.FindMember(field);
                            if (member == material.MemberEnd()) continue;
                            changed |= DrawJsonValueEditor(field, member->value, material.GetAllocator());
                        }
                        const auto textures = material.FindMember("$textures");
                        if (textures != material.MemberEnd() && (textures->value.IsArray() || textures->value.IsObject()))
                        {
                            ImGui::SeparatorText("Texture slots");
                            if (ImGui::BeginTable("MaterialTextureSlots", 3,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
                            {
                                ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                                ImGui::TableSetupColumn("Texture asset");
                                ImGui::TableSetupColumn("GUID", ImGuiTableColumnFlags_WidthFixed, 170.0f);
                                ImGui::TableHeadersRow();
                                const auto textureRow = [&material, &changed](const char* slot, rapidjson::Value& texture,
                                    const int id)
                                {
                                    ImGui::PushID(id);
                                    ImGui::TableNextRow();
                                    ImGui::TableNextColumn(); ImGui::TextUnformatted(slot);
                                    ImGui::TableNextColumn();
                                    if (texture.IsString())
                                    {
                                        std::string value(texture.GetString(), texture.GetStringLength());
                                        ImGui::SetNextItemWidth(-1.0f);
                                        if (InputTextString("##texture", value, 1024))
                                        {
                                            texture.SetString(value.c_str(), material.GetAllocator());
                                            changed = true;
                                        }
                                        ImGui::TableNextColumn();
                                        ImGui::Text("0x%016llX", static_cast<unsigned long long>(
                                            value.empty() ? 0 : RTech::StringToGuid(value.c_str())));
                                    }
                                    else
                                    {
                                        ImGui::TextDisabled("Non-string texture entry");
                                        ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                                    }
                                    ImGui::PopID();
                                };
                                if (textures->value.IsArray())
                                {
                                    for (rapidjson::SizeType index = 0; index < textures->value.Size(); ++index)
                                    {
                                        const std::string slot = std::to_string(index);
                                        textureRow(slot.c_str(), textures->value[index], static_cast<int>(index));
                                    }
                                }
                                else
                                {
                                    int index{};
                                    for (auto texture = textures->value.MemberBegin(); texture != textures->value.MemberEnd(); ++texture, ++index)
                                        textureRow(texture->name.GetString(), texture->value, index);
                                }
                                ImGui::EndTable();
                            }
                        }
                        if (ImGui::CollapsingHeader("Advanced material attributes"))
                        {
                            for (auto member = material.MemberBegin(); member != material.MemberEnd(); ++member)
                            {
                                const std::string_view name(member->name.GetString(), member->name.GetStringLength());
                                if (name == "$textures" || name == "name" || name == "surfaceProp" ||
                                    name == "surfaceProp2" || name == "shaderType" || name == "shaderSet") continue;
                                changed |= DrawJsonValueEditor(member->name.GetString(), member->value,
                                    material.GetAllocator());
                            }
                        }
                        if (changed)
                        {
                            std::string error;
                            if (WriteJsonDocument(jsonPath, material, error))
                            {
                                repakProject_.MarkDirty();
                                AppendRePakLog(firestar::repak::LogLevel::Info,
                                    "Updated material JSON " + PathToUtf8(jsonPath.filename()));
                            }
                            else AppendRePakLog(firestar::repak::LogLevel::Error, error);
                        }
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("UBER parameters"))
            {
                ImGui::TextDisabled("%s", PathToUtf8(uberPath).c_str());
                std::vector<std::uint8_t> bytes;
                if (fs::is_regular_file(uberPath))
                {
                    std::ifstream input(uberPath, std::ios::binary | std::ios::ate);
                    const std::streamoff size = input.tellg();
                    if (size > 0)
                    {
                        bytes.resize(static_cast<size_t>(size));
                        input.seekg(0, std::ios::beg);
                        input.read(reinterpret_cast<char*>(bytes.data()), size);
                    }
                }
                if (bytes.size() != 224 && bytes.size() < 512)
                    ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.42f, 1.0f),
                        "A supported 224-byte or 512-byte UBER companion was not found.");
                else
                {
                    const bool v15 = bytes.size() >= 512;
                    ImGui::SeparatorText(v15 ? "Apex material parameters" : "Titanfall 2 material parameters");
                    bool changed{};
                    const auto parameter = [&bytes, &changed](const char* name, const size_t offset) {
                        float value{};
                        std::memcpy(&value, bytes.data() + offset, sizeof(value));
                        ImGui::PushID(static_cast<int>(offset));
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
                        ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f);
                        if (ImGui::DragFloat("##value", &value, 0.005f, -10000.0f, 10000.0f, "%.5g"))
                        {
                            std::memcpy(bytes.data() + offset, &value, sizeof(value));
                            changed = true;
                        }
                        ImGui::PopID();
                    };
                    if (ImGui::BeginTable("EditableUberParameters", 2,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
                    {
                        ImGui::TableSetupColumn("Attribute");
                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                        ImGui::TableHeadersRow();
                        if (v15)
                        {
                            parameter("Layer blend ramp", 140); parameter("Opacity", 144);
                            parameter("Alpha test reference", 188); parameter("Aspect ratio", 192);
                            parameter("DOF opacity luminance", 204); parameter("TSAA motion threshold", 212);
                            parameter("Glitch strength", 272); parameter("Depth blend scalar", 368);
                            parameter("Layer 0 gloss", 400); parameter("Layer 1 gloss", 416);
                        }
                        else
                        {
                            parameter("Fog color factor", 88); parameter("Layer blend ramp", 92);
                            parameter("Opacity", 108); parameter("Alpha test reference", 152);
                            parameter("Aspect ratio", 156); parameter("Shadow bias", 172);
                            parameter("TSAA motion threshold", 180); parameter("DOF opacity luminance", 192);
                            parameter("Gloss", 208);
                        }
                        ImGui::EndTable();
                    }
                    if (changed)
                    {
                        const fs::path backup = uberPath.wstring() + L".firestar.bak";
                        std::error_code ioError;
                        if (!fs::exists(backup)) fs::copy_file(uberPath, backup, fs::copy_options::none, ioError);
                        std::ofstream output(uberPath, std::ios::binary | std::ios::trunc);
                        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                        if (output.good())
                        {
                            repakProject_.MarkDirty();
                            AppendRePakLog(firestar::repak::LogLevel::Info,
                                "Updated UBER parameters " + PathToUtf8(uberPath.filename()));
                        }
                        else AppendRePakLog(firestar::repak::LogLevel::Error,
                            "The UBER parameter write was incomplete.");
                    }
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        void DrawSourceData(const HWND window)
        {
            (void)window;
            if (selectedAsset_ < 0)
            {
                ImGui::TextDisabled("Select an asset to inspect its structured source data.");
                return;
            }
            const rapidjson::Value* selected = repakProject_.Asset(static_cast<size_t>(selectedAsset_));
            const std::string selectedType = selected ? AssetMemberString(*selected, "_type") : std::string{};
            if (selectedType == "aevt")
            {
                ImGui::TextDisabled("Audio events live in the RePak manifest and do not have a separate AEVT file.");
                ImGui::SeparatorText("Event manifest");
                DrawReadOnlyJson("AEVT", *selected);
                return;
            }
            const auto expected = repakProject_.ResolvePrimarySource(static_cast<size_t>(selectedAsset_));
            if (expected && loadedSourcePath_ != *expected)
                RefreshSourceData();
            ImGui::TextWrapped("%s", loadedSourcePath_.empty() ? "No source path could be resolved." : PathToUtf8(loadedSourcePath_).c_str());
            if (!sourceLoadError_.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.72f, 1.0f), "%s", sourceLoadError_.c_str());
            if (loadedSourceBytes_.empty())
                return;

            const rapidjson::Value* asset = repakProject_.Asset(static_cast<size_t>(selectedAsset_));
            const std::string type = asset ? AssetMemberString(*asset, "_type") : std::string{};
            if (type == "matl")
            {
                DrawMaterialSourceData();
                return;
            }
            ImGui::SeparatorText("Overview");
            ImGui::Text("Type: %s", type.c_str());
            ImGui::Text("Size: %zu bytes", loadedSourceBytes_.size());

            const auto read16 = [this](const size_t offset) {
                return static_cast<std::uint16_t>(loadedSourceBytes_[offset]) |
                    static_cast<std::uint16_t>(loadedSourceBytes_[offset + 1] << 8);
            };
            const auto read32 = [this](const size_t offset) {
                return static_cast<std::uint32_t>(loadedSourceBytes_[offset]) |
                    (static_cast<std::uint32_t>(loadedSourceBytes_[offset + 1]) << 8) |
                    (static_cast<std::uint32_t>(loadedSourceBytes_[offset + 2]) << 16) |
                    (static_cast<std::uint32_t>(loadedSourceBytes_[offset + 3]) << 24);
            };
            const auto read64 = [&read32](const size_t offset) {
                return static_cast<std::uint64_t>(read32(offset)) |
                    (static_cast<std::uint64_t>(read32(offset + 4)) << 32);
            };
            const auto readFloat = [this](const size_t offset) {
                float value{};
                std::memcpy(&value, loadedSourceBytes_.data() + offset, sizeof(value));
                return value;
            };
            const auto readString = [this](const size_t offset) {
                std::string value;
                if (offset >= loadedSourceBytes_.size())
                    return value;
                const char* text = reinterpret_cast<const char*>(loadedSourceBytes_.data() + offset);
                const size_t available = loadedSourceBytes_.size() - offset;
                if (const void* terminator = std::memchr(text, '\0', available))
                    value.assign(text, static_cast<const char*>(terminator));
                return value;
            };

            if (type == "txtr" && loadedSourceBytes_.size() >= 128 && read32(0) == 0x20534444)
            {
                DdsHeader header{};
                std::memcpy(&header, loadedSourceBytes_.data() + 4, sizeof(header));
                const char fourCC[5]{
                    static_cast<char>(header.pixelFormat.fourCC & 0xFF),
                    static_cast<char>((header.pixelFormat.fourCC >> 8) & 0xFF),
                    static_cast<char>((header.pixelFormat.fourCC >> 16) & 0xFF),
                    static_cast<char>((header.pixelFormat.fourCC >> 24) & 0xFF), '\0'};
                ImGui::SeparatorText("Texture");
                if (ImGui::BeginTable("DdsDetails", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                {
                    const auto row = [](const char* name, const std::string& value) {
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(value.c_str());
                    };
                    row("Dimensions", std::to_string(header.width) + " x " + std::to_string(header.height));
                    row("Mip levels", std::to_string((std::max)(1u, header.mipMapCount)));
                    row("Pixel format", header.pixelFormat.fourCC ? fourCC : std::to_string(header.pixelFormat.rgbBitCount) + "-bit RGBA");
                    row("Pitch / linear size", std::to_string(header.pitchOrLinearSize));
                    ImGui::EndTable();
                }
            }
            else if ((type == "mdl_" || type == "arig") && loadedSourceBytes_.size() >= 216 &&
                read32(0) == 0x54534449)
            {
                const std::int32_t version = static_cast<std::int32_t>(read32(4));
                const std::int32_t bones = static_cast<std::int32_t>(read32(160));
                const std::int32_t sequences = static_cast<std::int32_t>(read32(192));
                const std::int32_t materialCount = static_cast<std::int32_t>(read32(208));
                const std::int32_t materialOffset = static_cast<std::int32_t>(read32(212));
                const char* modelName = reinterpret_cast<const char*>(loadedSourceBytes_.data() + 16);
                const size_t modelNameLength = strnlen_s(modelName, 64);
                ImGui::SeparatorText(type == "mdl_" ? "Model" : "Animation rig");
                ImGui::Text("Studio version: %d", version);
                ImGui::Text("Name: %.*s", static_cast<int>(modelNameLength), modelName);
                ImGui::Text("Bones: %d", bones);
                ImGui::Text("Local sequences: %d", sequences);
                ImGui::Text("Material slots: %d", materialCount);
                constexpr size_t StudioTextureSize = 12;
                if (materialCount > 0 && materialCount <= 4096 && materialOffset >= 0 &&
                    static_cast<size_t>(materialOffset) + static_cast<size_t>(materialCount) * StudioTextureSize <= loadedSourceBytes_.size())
                {
                    ImGui::SeparatorText("Material texture slots");
                    if (ImGui::BeginTable("ModelMaterials", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
                    {
                        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 56.0f);
                        ImGui::TableSetupColumn("Material path");
                        ImGui::TableSetupColumn("GUID", ImGuiTableColumnFlags_WidthFixed, 170.0f);
                        ImGui::TableHeadersRow();
                        for (int index = 0; index < materialCount; ++index)
                        {
                            const size_t textureOffset = static_cast<size_t>(materialOffset) + static_cast<size_t>(index) * StudioTextureSize;
                            const std::uint32_t embeddedPathOffset = read32(textureOffset);
                            const std::uint64_t materialGuid = read64(textureOffset + 4);
                            std::string materialPath = "(no embedded path)";
                            const std::int64_t pathOffset = static_cast<std::int64_t>(textureOffset) + embeddedPathOffset;
                            if (pathOffset >= 0 && static_cast<size_t>(pathOffset) < loadedSourceBytes_.size())
                            {
                                const char* text = reinterpret_cast<const char*>(loadedSourceBytes_.data() + pathOffset);
                                const size_t available = loadedSourceBytes_.size() - static_cast<size_t>(pathOffset);
                                if (const void* terminator = std::memchr(text, '\0', available))
                                    materialPath.assign(text, static_cast<const char*>(terminator));
                            }
                            char guid[32]{};
                            sprintf_s(guid, "0x%016llX", static_cast<unsigned long long>(materialGuid));
                            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%d", index);
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(materialPath.c_str());
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(guid);
                        }
                        ImGui::EndTable();
                    }
                }
            }
            else if (type == "txan" && loadedSourceBytes_.size() >= 16 && read32(0) == 0x4E415854)
            {
                ImGui::SeparatorText("Texture animation");
                ImGui::Text("File version: %u", read16(4));
                ImGui::Text("Asset version: %u", read16(6));
                ImGui::Text("Layers: %u", read32(8));
                ImGui::Text("Texture slots: %u", read32(12));
            }
            else if (type == "anir" && loadedSourceBytes_.size() >= 56 && read32(0) == 0x52494E41)
            {
                ImGui::SeparatorText("Animation recording");
                ImGui::Text("File version: %u", read16(4));
                ImGui::Text("Asset version: %u", read16(6));
                ImGui::Text("Elements: %u", read32(36));
                ImGui::Text("Sequences: %u", read32(40));
                ImGui::Text("Recorded frames: %u", read32(44));
                ImGui::Text("Recorded overlays: %u", read32(48));
            }
            else if (type == "aseq" && loadedSourceBytes_.size() >= 200)
            {
                const std::int32_t labelOffset = static_cast<std::int32_t>(read32(4));
                const std::int32_t activityOffset = static_cast<std::int32_t>(read32(8));
                const auto relativeString = [&readString](const std::int32_t offset) {
                    return offset > 0 ? readString(static_cast<size_t>(offset)) : std::string{};
                };
                const std::string label = relativeString(labelOffset);
                const std::string activityName = relativeString(activityOffset);
                ImGui::SeparatorText("Animation sequence");
                if (!label.empty()) ImGui::Text("Label: %s", label.c_str());
                if (!activityName.empty()) ImGui::Text("Activity name: %s", activityName.c_str());
                ImGui::Text("Flags: %u", read32(12));
                ImGui::Text("Activity: %d", static_cast<std::int32_t>(read32(16)));
                ImGui::Text("Activity weight: %d", static_cast<std::int32_t>(read32(20)));
                ImGui::Text("Events: %d", static_cast<std::int32_t>(read32(24)));
                ImGui::Text("Blends: %d", static_cast<std::int32_t>(read32(56)));
                ImGui::Text("Blend grid: %d x %d", static_cast<std::int32_t>(read32(68)),
                    static_cast<std::int32_t>(read32(72)));
                ImGui::Text("Fade: %.3fs in / %.3fs out", readFloat(104), readFloat(108));
                ImGui::Text("Last frame: %.3f", readFloat(132));
                ImGui::Text("Auto layers: %d", static_cast<std::int32_t>(read32(148)));
                ImGui::Text("Key/value bytes: %d", static_cast<std::int32_t>(read32(176)));
                ImGui::Text("Activity modifiers: %d", static_cast<std::int32_t>(read32(188)));
            }
            else if (type == "ui" && loadedSourceBytes_.size() >= 72 && read32(0) == 0x50495552)
            {
                float width{}; float height{};
                std::memcpy(&width, loadedSourceBytes_.data() + 16, sizeof(float));
                std::memcpy(&height, loadedSourceBytes_.data() + 20, sizeof(float));
                ImGui::SeparatorText("RUI package");
                ImGui::Text("Package version: %u", read16(4));
                ImGui::Text("RUI version: %u", read16(6));
                ImGui::Text("Canvas: %.0f x %.0f", width, height);
                ImGui::Text("Arguments: %u", read16(44));
                ImGui::Text("Render jobs: %u", read16(40));
                ImGui::Text("Keyframes: %u", read16(46));
                ImGui::Text("Style descriptors: %u", read16(36));
            }
            else if ((type == "shdr" || type == "shds") && loadedSourceBytes_.size() >= 5 &&
                loadedSourceBytes_[0] == 'M' && loadedSourceBytes_[1] == 'S' && loadedSourceBytes_[2] == 'W')
            {
                ImGui::SeparatorText("Shader wrapper");
                ImGui::Text("Wrapper version: %u", loadedSourceBytes_[3]);
                ImGui::Text("Kind: %s", loadedSourceBytes_[4] == 1 ? "Shader set" : "Shader");
                if (loadedSourceBytes_[4] == 1 && loadedSourceBytes_.size() >= 37)
                {
                    char pixelGuid[32]{};
                    char vertexGuid[32]{};
                    sprintf_s(pixelGuid, "0x%016llX", static_cast<unsigned long long>(read64(5)));
                    sprintf_s(vertexGuid, "0x%016llX", static_cast<unsigned long long>(read64(13)));
                    ImGui::SeparatorText("Shader set");
                    ImGui::Text("Pixel shader GUID: %s", pixelGuid);
                    ImGui::Text("Vertex shader GUID: %s", vertexGuid);
                    ImGui::Text("Texture slots: %u pixel / %u vertex", read16(21), read16(23));
                    ImGui::Text("Samplers: %u", read16(25));
                    ImGui::Text("Resources: %u starting at bind point %u",
                        loadedSourceBytes_[28], loadedSourceBytes_[27]);
                    const bool hasPixelShader = read32(29) != 0;
                    const bool hasVertexShader = read32(33) != 0;
                    const char* embeddedShaders = hasPixelShader && hasVertexShader ? "pixel + vertex" :
                        hasPixelShader ? "pixel" : hasVertexShader ? "vertex" : "none";
                    ImGui::Text("Embedded shaders: %s", embeddedShaders);
                }
                else if (loadedSourceBytes_[4] == 0 && loadedSourceBytes_.size() >= 21)
                {
                    const std::uint64_t packed = read64(5);
                    const std::uint32_t nameLength = read32(13);
                    const std::uint32_t nameOffset = read32(17);
                    const std::string name = nameLength && nameOffset < loadedSourceBytes_.size()
                        ? readString(nameOffset) : std::string{};
                    ImGui::SeparatorText("Shader");
                    if (!name.empty()) ImGui::Text("Name: %s", name.c_str());
                    ImGui::Text("Variants: %u", static_cast<unsigned>(packed >> 56));
                    ImGui::Text("Feature mask: %014llX",
                        static_cast<unsigned long long>(packed & 0x00FFFFFFFFFFFFFFull));
                }
            }
            else if (type == "matl" && ToLowerAscii(loadedSourcePath_.extension().string()) == ".uber" &&
                (loadedSourceBytes_.size() == 224 || loadedSourceBytes_.size() >= 512))
            {
                const bool v15 = loadedSourceBytes_.size() >= 512;
                ImGui::SeparatorText("Material shader parameters");
                ImGui::Text("Layout: %s (%zu bytes)", v15 ? "Apex material" : "Titanfall 2 material",
                    loadedSourceBytes_.size());
                const auto parameter = [&readFloat](const char* name, const size_t offset) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
                    ImGui::TableNextColumn(); ImGui::Text("%.5g", readFloat(offset));
                };
                if (ImGui::BeginTable("MaterialParameters", 2,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
                {
                    ImGui::TableSetupColumn("Parameter");
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                    ImGui::TableHeadersRow();
                    if (v15)
                    {
                        parameter("Layer blend ramp", 140);
                        parameter("Opacity", 144);
                        parameter("Alpha test reference", 188);
                        parameter("Aspect ratio", 192);
                        parameter("DOF opacity luminance", 204);
                        parameter("TSAA motion threshold", 212);
                        parameter("Glitch strength", 272);
                        parameter("Depth blend scalar", 368);
                        parameter("Layer 0 gloss", 400);
                        parameter("Layer 1 gloss", 416);
                    }
                    else
                    {
                        parameter("Fog color factor", 88);
                        parameter("Layer blend ramp", 92);
                        parameter("Opacity", 108);
                        parameter("Alpha test reference", 152);
                        parameter("Aspect ratio", 156);
                        parameter("Shadow bias", 172);
                        parameter("TSAA motion threshold", 180);
                        parameter("DOF opacity luminance", 192);
                        parameter("Gloss", 208);
                    }
                    ImGui::EndTable();
                }
            }
            else if (ToLowerAscii(loadedSourcePath_.extension().string()) == ".json")
            {
                std::string jsonText(reinterpret_cast<const char*>(loadedSourceBytes_.data()), loadedSourceBytes_.size());
                rapidjson::Document sidecar;
                sidecar.Parse(jsonText.c_str(), jsonText.size());
                ImGui::SeparatorText("Structured source data");
                if (sidecar.HasParseError()) ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.42f, 1.0f), "The source JSON is invalid.");
                else DrawReadOnlyJson("Source", sidecar);
            }
            else
            {
                ImGui::SeparatorText("Source details");
                ImGui::TextWrapped("This format does not expose a reliable structured header. Build the RPAK to run full source validation.");
            }
        }

        bool IsModelPreviewMeshVisible(const firestar::editor::PreviewMesh& mesh) const
        {
            if (mesh.bodyPartIndex < 0 || mesh.modelIndex < 0)
                return true;
            const size_t bodyPart = static_cast<size_t>(mesh.bodyPartIndex);
            return bodyPart >= modelPreviewBodySelections_.size() ||
                modelPreviewBodySelections_[bodyPart] == static_cast<size_t>(mesh.modelIndex);
        }

        void ResetAudioPreview()
        {
            audioPreview_.Reset();
            audioEventPreviewChoices_.clear();
            audioPreviewOwnerAsset_ = -1;
            audioPreviewSourceAsset_ = -1;
            audioEventPreviewChoice_ = 0;
            audioPreviewError_.clear();
        }

        int FindProjectAudioSource(const std::string& reference) const
        {
            int index = FindProjectAssetByGuid(reference.empty() ? 0 : RTech::StringToGuid(reference.c_str()));
            const auto isAudioSource = [this](const int candidate) {
                if (candidate < 0) return false;
                const rapidjson::Value* asset = repakProject_.Asset(static_cast<size_t>(candidate));
                const std::string type = asset ? AssetMemberString(*asset, "_type") : std::string{};
                return type == "awsr" || type == "asrc";
            };
            if (isAudioSource(index)) return index;
            const std::string normalized = NormalizeAssetPath(reference);
            for (size_t assetIndex = 0; assetIndex < repakProject_.AssetCount(); ++assetIndex)
            {
                const rapidjson::Value* asset = repakProject_.Asset(assetIndex);
                if (!asset) continue;
                const std::string type = AssetMemberString(*asset, "_type");
                if (type != "awsr" && type != "asrc") continue;
                if (NormalizeAssetPath(AssetMemberString(*asset, "_path")) == normalized)
                    return static_cast<int>(assetIndex);
            }
            return -1;
        }

        void PrepareAudioEventPreview(const rapidjson::Value& event)
        {
            if (audioPreviewOwnerAsset_ == selectedAsset_) return;
            audioPreview_.Reset();
            audioPreviewSourceAsset_ = -1;
            audioPreviewError_.clear();
            audioEventPreviewChoices_.clear();
            audioEventPreviewChoice_ = 0;
            audioPreviewOwnerAsset_ = selectedAsset_;
            const auto sources = event.FindMember("sources");
            if (sources == event.MemberEnd() || !sources->value.IsArray()) return;
            for (const rapidjson::Value& value : sources->value.GetArray())
            {
                if (!value.IsString()) continue;
                AudioEventPreviewChoice choice;
                choice.reference.assign(value.GetString(), value.GetStringLength());
                choice.assetIndex = FindProjectAudioSource(choice.reference);
                audioEventPreviewChoices_.push_back(std::move(choice));
            }
        }

        bool LoadAudioPreviewAsset(const int sourceAsset)
        {
            if (sourceAsset == audioPreviewSourceAsset_ && audioPreview_.IsLoaded()) return true;
            audioPreview_.Reset();
            audioPreviewSourceAsset_ = sourceAsset;
            audioPreviewError_.clear();
            if (sourceAsset < 0)
            {
                audioPreviewError_ = "This event source is not present as an audio source asset in the open project.";
                return false;
            }
            const auto source = repakProject_.ResolvePrimarySource(static_cast<size_t>(sourceAsset));
            if (!source || !fs::is_regular_file(*source))
            {
                audioPreviewError_ = "The referenced WAV or OGG source file could not be found.";
                return false;
            }
            if (!audioPreview_.Load(*source, audioPreviewError_)) return false;
            return true;
        }

        static std::string FormatAudioTime(const double seconds)
        {
            const double clamped = (std::max)(0.0, seconds);
            const int minutes = static_cast<int>(clamped / 60.0);
            const double remainder = clamped - minutes * 60.0;
            char text[32]{};
            std::snprintf(text, sizeof(text), "%d:%06.3f", minutes, remainder);
            return text;
        }

        void DrawAudioWaveform()
        {
            const double duration = audioPreview_.DurationSeconds();
            const double position = audioPreview_.PositionSeconds();
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const ImVec2 size((std::max)(240.0f, available.x), (std::max)(130.0f, available.y - 34.0f));
            ImGui::InvisibleButton("AudioWaveform", size, ImGuiButtonFlags_MouseButtonLeft);
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(minimum, maximum, ImGui::GetColorU32(ImGuiCol_ChildBg));
            draw->AddRect(minimum, maximum, ImGui::GetColorU32(ImGuiCol_Border));
            const float center = (minimum.y + maximum.y) * 0.5f;
            draw->AddLine(ImVec2(minimum.x, center), ImVec2(maximum.x, center),
                ImGui::GetColorU32(ImGuiCol_Border));
            const auto& waveform = audioPreview_.Waveform();
            const float progress = duration > 0.0 ? static_cast<float>((std::clamp)(position / duration, 0.0, 1.0)) : 0.0f;
            const float width = maximum.x - minimum.x;
            if (!waveform.empty() && width > 0.0f)
            {
                const int columns = (std::max)(1, static_cast<int>(width));
                for (int column = 0; column < columns; ++column)
                {
                    const size_t first = static_cast<size_t>(column) * waveform.size() / columns;
                    const size_t last = (std::max)(first + 1,
                        static_cast<size_t>(column + 1) * waveform.size() / columns);
                    float peak{};
                    for (size_t index = first; index < (std::min)(last, waveform.size()); ++index)
                        peak = (std::max)(peak, waveform[index]);
                    const float x = minimum.x + static_cast<float>(column) + 0.5f;
                    const float height = peak * (maximum.y - minimum.y) * 0.44f;
                    const ImU32 color = static_cast<float>(column) / columns <= progress
                        ? ImGui::GetColorU32(ImGuiCol_CheckMark)
                        : ImGui::GetColorU32(ImGuiCol_TextDisabled);
                    draw->AddLine(ImVec2(x, center - height), ImVec2(x, center + height), color);
                }
                const float playhead = minimum.x + progress * width;
                draw->AddLine(ImVec2(playhead, minimum.y), ImVec2(playhead, maximum.y),
                    ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 2.0f);
            }
            if ((ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ||
                ImGui::IsItemActive())
            {
                const float fraction = width > 0.0f
                    ? (std::clamp)((ImGui::GetMousePos().x - minimum.x) / width, 0.0f, 1.0f) : 0.0f;
                audioPreview_.Seek(duration * fraction);
            }
        }

        void DrawAudioPreview(const std::string& type, const rapidjson::Value& asset)
        {
            int sourceAsset = selectedAsset_;
            if (type == "aevt")
            {
                PrepareAudioEventPreview(asset);
                if (audioEventPreviewChoices_.empty())
                {
                    ImGui::TextDisabled("This event has no audio sources. Add one from the asset context menu.");
                    return;
                }
                audioEventPreviewChoice_ = (std::clamp)(audioEventPreviewChoice_, 0,
                    static_cast<int>(audioEventPreviewChoices_.size() - 1));
                ImGui::SetNextItemWidth((std::min)(520.0f, ImGui::GetContentRegionAvail().x));
                if (ImGui::BeginCombo("Event sound", audioEventPreviewChoices_[audioEventPreviewChoice_].reference.c_str()))
                {
                    for (size_t index = 0; index < audioEventPreviewChoices_.size(); ++index)
                    {
                        const bool selected = audioEventPreviewChoice_ == static_cast<int>(index);
                        std::string label = audioEventPreviewChoices_[index].reference;
                        if (audioEventPreviewChoices_[index].assetIndex < 0) label += "  (not in project)";
                        if (ImGui::Selectable(label.c_str(), selected) && !selected)
                        {
                            audioEventPreviewChoice_ = static_cast<int>(index);
                            audioPreviewSourceAsset_ = -1;
                            audioPreview_.Reset();
                            audioPreviewError_.clear();
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                sourceAsset = audioEventPreviewChoices_[audioEventPreviewChoice_].assetIndex;
            }
            else if (audioPreviewOwnerAsset_ != selectedAsset_)
            {
                audioPreviewOwnerAsset_ = selectedAsset_;
                audioPreviewSourceAsset_ = -1;
                audioPreview_.Reset();
                audioPreviewError_.clear();
            }

            if (!LoadAudioPreviewAsset(sourceAsset))
            {
                ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", audioPreviewError_.c_str());
                return;
            }
            audioPreview_.Update();
            ImGui::Text("%u Hz  |  %u channel%s", audioPreview_.SampleRate(), audioPreview_.Channels(),
                audioPreview_.Channels() == 1 ? "" : "s");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", PathToUtf8(audioPreview_.SourcePath()).c_str());
            if (ImGui::Button(audioPreview_.IsPlaying() ? "Pause" : "Play"))
            {
                if (audioPreview_.IsPlaying()) audioPreview_.Pause();
                else
                    static_cast<void>(audioPreview_.Play(audioPreviewError_));
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop")) audioPreview_.Stop();
            ImGui::SameLine();
            float volume = audioPreview_.Volume() * 100.0f;
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat("Volume", &volume, 0.0f, 100.0f, "%.0f%%"))
                audioPreview_.SetVolume(volume / 100.0f);
            ImGui::SameLine();
            ImGui::Text("%s / %s", FormatAudioTime(audioPreview_.PositionSeconds()).c_str(),
                FormatAudioTime(audioPreview_.DurationSeconds()).c_str());
            if (!audioPreviewError_.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.38f, 1.0f), "%s", audioPreviewError_.c_str());
            DrawAudioWaveform();
        }

        void DrawAssetPreview()
        {
            if (selectedAsset_ < 0)
            {
                ImGui::TextDisabled("Select an asset to preview it.");
                return;
            }
            const rapidjson::Value* asset = repakProject_.Asset(static_cast<size_t>(selectedAsset_));
            if (!asset) return;
            const std::string type = AssetMemberString(*asset, "_type");
            if (type == "awsr" || type == "asrc" || type == "aevt")
            {
                DrawAudioPreview(type, *asset);
                return;
            }
            if (modelPreviewAssetIndex_ != selectedAsset_)
                UpdateAssetPreview();
            if (!modelPreview_.Empty())
            {
                size_t texturedMaterials{};
                for (const auto& material : modelPreviewMaterials_)
                    if (!material.missing) ++texturedMaterials;
                ImGui::Text("%zu vertices  |  %zu triangles  |  %zu/%zu textures",
                    modelPreview_.vertices.size(), modelPreview_.indices.size() / 3,
                    texturedMaterials, modelPreviewMaterials_.size());
                ImGui::SameLine();
                ImGui::TextDisabled("%s", modelPreview_.format.c_str());

                bool reloadPreview{};
                if (modelPreview_.lodLevels.size() > 1)
                {
                    ImGui::SetNextItemWidth(112.0f);
                    const std::string lodLabel = "LOD " + std::to_string(modelPreview_.lodLevels[modelPreviewLod_]);
                    if (ImGui::BeginCombo("LOD", lodLabel.c_str()))
                    {
                        for (size_t index = 0; index < modelPreview_.lodLevels.size(); ++index)
                        {
                            const bool selected = modelPreviewLod_ == index;
                            const std::string label = "LOD " + std::to_string(modelPreview_.lodLevels[index]);
                            if (ImGui::Selectable(label.c_str(), selected) && !selected)
                            {
                                modelPreviewLod_ = index;
                                reloadPreview = true;
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                }

                if (!modelPreview_.bodyParts.empty())
                {
                    modelPreviewSelectedBodyPart_ = (std::min)(modelPreviewSelectedBodyPart_,
                        modelPreview_.bodyParts.size() - 1);
                    const auto& selectedBodyPart = modelPreview_.bodyParts[modelPreviewSelectedBodyPart_];
                    ImGui::SetNextItemWidth(180.0f);
                    if (ImGui::BeginCombo("Body group", selectedBodyPart.name.c_str()))
                    {
                        for (size_t index = 0; index < modelPreview_.bodyParts.size(); ++index)
                        {
                            const bool selected = modelPreviewSelectedBodyPart_ == index;
                            if (ImGui::Selectable(modelPreview_.bodyParts[index].name.c_str(), selected))
                                modelPreviewSelectedBodyPart_ = index;
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    const auto& activeBodyPart = modelPreview_.bodyParts[modelPreviewSelectedBodyPart_];
                    if (activeBodyPart.models.size() > 1)
                    {
                        size_t& selectedModel = modelPreviewBodySelections_[modelPreviewSelectedBodyPart_];
                        selectedModel = (std::min)(selectedModel, activeBodyPart.models.size() - 1);
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(190.0f);
                        if (ImGui::BeginCombo("Model", activeBodyPart.models[selectedModel].name.c_str()))
                        {
                            for (size_t index = 0; index < activeBodyPart.models.size(); ++index)
                            {
                                const bool selected = selectedModel == index;
                                if (ImGui::Selectable(activeBodyPart.models[index].name.c_str(), selected))
                                    selectedModel = index;
                                if (selected) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    }
                }
                if (reloadPreview)
                {
                    UpdateAssetPreview();
                    return;
                }
                ImGui::TextDisabled("Drag to orbit  |  Mouse wheel to zoom");

                const ImVec2 available = ImGui::GetContentRegionAvail();
                const ImVec2 canvasSize((std::max)(260.0f, available.x), (std::max)(220.0f, available.y));
                ImGui::InvisibleButton("RmdlPreviewCanvas", canvasSize,
                    ImGuiButtonFlags_MouseButtonLeft);
                ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
                const bool hovered = ImGui::IsItemHovered();
                if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    const ImVec2 delta = ImGui::GetIO().MouseDelta;
                    modelPreviewYaw_ += delta.x * 0.008f;
                    modelPreviewPitch_ = (std::clamp)(modelPreviewPitch_ + delta.y * 0.008f, -1.45f, 1.45f);
                }
                if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
                    modelPreviewZoom_ = (std::clamp)(modelPreviewZoom_ * std::pow(1.12f, ImGui::GetIO().MouseWheel), 0.15f, 8.0f);

                const ImVec2 canvasMin = ImGui::GetItemRectMin();
                const ImVec2 canvasMax = ImGui::GetItemRectMax();
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                drawList->AddRectFilled(canvasMin, canvasMax, ImGui::GetColorU32(ImGuiCol_ChildBg));
                drawList->AddRect(canvasMin, canvasMax, ImGui::GetColorU32(ImGuiCol_Border));
                drawList->PushClipRect(canvasMin, canvasMax, true);

                const firestar::editor::PreviewVertex center{
                    (modelPreview_.minimum.x + modelPreview_.maximum.x) * 0.5f,
                    (modelPreview_.minimum.y + modelPreview_.maximum.y) * 0.5f,
                    (modelPreview_.minimum.z + modelPreview_.maximum.z) * 0.5f};
                const float extent = (std::max)({
                    modelPreview_.maximum.x - modelPreview_.minimum.x,
                    modelPreview_.maximum.y - modelPreview_.minimum.y,
                    modelPreview_.maximum.z - modelPreview_.minimum.z,
                    0.001f});
                const float scale = (std::min)(canvasSize.x, canvasSize.y) * 0.43f * modelPreviewZoom_ / extent;
                const float cy = std::cos(modelPreviewYaw_);
                const float sy = std::sin(modelPreviewYaw_);
                const float cp = std::cos(modelPreviewPitch_);
                const float sp = std::sin(modelPreviewPitch_);
                struct ProjectedVertex
                {
                    ImVec2 position;
                    ImVec2 uv;
                    float depth{};
                };
                std::vector<ProjectedVertex> projected;
                projected.reserve(modelPreview_.vertices.size());
                for (const auto& source : modelPreview_.vertices)
                {
                    const float x = source.x - center.x;
                    const float y = source.y - center.y;
                    const float z = source.z - center.z;
                    const float rotatedX = x * cy - y * sy;
                    const float rotatedY = x * sy + y * cy;
                    const float rotatedZ = z * cp - rotatedY * sp;
                    const float depth = z * sp + rotatedY * cp;
                    const float perspective = (std::clamp)(1.0f + depth / (extent * 3.0f), 0.55f, 1.5f);
                    ProjectedVertex vertex;
                    vertex.position = {
                        (canvasMin.x + canvasMax.x) * 0.5f + rotatedX * scale / perspective,
                        (canvasMin.y + canvasMax.y) * 0.5f - rotatedZ * scale / perspective};
                    vertex.uv = {source.u - std::floor(source.u), source.v - std::floor(source.v)};
                    vertex.depth = depth;
                    projected.push_back(vertex);
                }

                size_t visibleTriangles{};
                for (const auto& mesh : modelPreview_.meshes)
                    if (IsModelPreviewMeshVisible(mesh)) visibleTriangles += mesh.indexCount / 3;
                // Keep orbiting responsive even for very dense RMDLs. The
                // preview is an authoring aid, not the final game renderer.
                constexpr size_t MaximumPreviewTriangles = 16000;
                const size_t step = (std::max<size_t>)(1, visibleTriangles / MaximumPreviewTriangles + 1);
                for (const auto& mesh : modelPreview_.meshes)
                {
                    if (!IsModelPreviewMeshVisible(mesh)) continue;
                    struct Triangle
                    {
                        std::uint32_t a{};
                        std::uint32_t b{};
                        std::uint32_t c{};
                        float depth{};
                    };
                    std::vector<Triangle> drawTriangles;
                    const size_t triangles = mesh.indexCount / 3;
                    drawTriangles.reserve(triangles / step + 1);
                    for (size_t triangle = 0; triangle < triangles; triangle += step)
                    {
                        const size_t offset = static_cast<size_t>(mesh.firstIndex) + triangle * 3;
                        if (offset + 2 >= modelPreview_.indices.size()) break;
                        const std::uint32_t a = modelPreview_.indices[offset];
                        const std::uint32_t b = modelPreview_.indices[offset + 1];
                        const std::uint32_t c = modelPreview_.indices[offset + 2];
                        if (a >= projected.size() || b >= projected.size() || c >= projected.size()) continue;
                        drawTriangles.push_back({a, b, c,
                            (projected[a].depth + projected[b].depth + projected[c].depth) / 3.0f});
                    }
                    std::sort(drawTriangles.begin(), drawTriangles.end(), [](const Triangle& left, const Triangle& right) {
                        return left.depth > right.depth;
                    });

                    const ModelPreviewMaterialTexture* material = ModelPreviewTexture(mesh.materialGuid);
                    ID3D11ShaderResourceView* texture = material && !material->missing
                        ? material->texture.Get() : modelMissingTexture_.Get();
                    if (texture)
                        drawList->PushTexture(reinterpret_cast<void*>(texture));
                    for (const Triangle& triangle : drawTriangles)
                    {
                        if (!texture)
                        {
                            drawList->AddTriangleFilled(projected[triangle.a].position,
                                projected[triangle.b].position, projected[triangle.c].position,
                                IM_COL32(255, 0, 255, 255));
                            continue;
                        }
                        drawList->PrimReserve(3, 3);
                        const ImDrawIdx base = static_cast<ImDrawIdx>(drawList->_VtxCurrentIdx);
                        drawList->PrimWriteIdx(base);
                        drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
                        drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
                        drawList->PrimWriteVtx(projected[triangle.a].position, projected[triangle.a].uv, IM_COL32_WHITE);
                        drawList->PrimWriteVtx(projected[triangle.b].position, projected[triangle.b].uv, IM_COL32_WHITE);
                        drawList->PrimWriteVtx(projected[triangle.c].position, projected[triangle.c].uv, IM_COL32_WHITE);
                    }
                    if (texture) drawList->PopTexture();
                }
                drawList->PopClipRect();
                return;
            }
            if (assetPreviewTexture_ && assetPreviewWidth_ && assetPreviewHeight_)
            {
                ImGui::Text("%ux%u  %s", assetPreviewWidth_, assetPreviewHeight_, assetPreviewFormat_.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("%s", PathToUtf8(assetPreviewPath_).c_str());
                const ImVec2 available = ImGui::GetContentRegionAvail();
                const float scale = (std::min)(available.x / static_cast<float>(assetPreviewWidth_),
                    available.y / static_cast<float>(assetPreviewHeight_));
                const float fittedScale = (std::min)(1.0f, (std::max)(0.01f, scale));
                const ImVec2 size(assetPreviewWidth_ * fittedScale, assetPreviewHeight_ * fittedScale);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (std::max)(0.0f, (available.x - size.x) * 0.5f));
                ImGui::Image(reinterpret_cast<ImTextureID>(assetPreviewTexture_), size);
                return;
            }

            if (!assetPreviewError_.empty())
                ImGui::TextWrapped("%s", assetPreviewError_.c_str());
            else if (type == "mdl_" || type == "arig")
                ImGui::TextDisabled("Select Inspector to view this asset's GUID and dependencies.");
            else
                ImGui::TextDisabled("A visual preview is not available for this asset type.");
        }

        void DrawRePakBuildActivity(const float height)
        {
            std::vector<BuildLogEntry> entries;
            std::string result;
            bool succeeded = false;
            {
                std::lock_guard<std::mutex> lock(repakBuildMutex_);
                entries = repakBuildLog_;
                result = repakBuildResult_;
                succeeded = repakBuildSucceeded_;
            }
            ImGui::BeginChild("RePakBuildLog", ImVec2(0.0f, height), ImGuiChildFlags_Borders,
                ImGuiWindowFlags_HorizontalScrollbar);
            for (const auto& entry : entries)
            {
                ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                if (entry.level == firestar::repak::LogLevel::Error) color = ImVec4(1.0f, 0.52f, 0.62f, 1.0f);
                else if (entry.level == firestar::repak::LogLevel::Warning) color = ImVec4(1.0f, 0.78f, 0.40f, 1.0f);
                else if (entry.level == firestar::repak::LogLevel::Progress) color = ImVec4(0.82f, 0.66f, 1.0f, 1.0f);
                ImGui::TextColored(color, "%s", entry.message.c_str());
            }
            if (!result.empty())
                ImGui::TextColored(succeeded ? ImVec4(0.62f, 1.0f, 0.72f, 1.0f) : ImVec4(1.0f, 0.52f, 0.62f, 1.0f), "%s", result.c_str());
            ImGui::EndChild();
        }

        void DrawModCreator(const HWND window)
        {
            if (!showModCreatorWindow_)
                return;
            ImGui::SetNextWindowSize(ImVec2(720.0f, 450.0f), ImGuiCond_FirstUseEver);
            ImGuiWindowClass modCreatorWindowClass{};
            modCreatorWindowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoTaskBarIcon |
                ImGuiViewportFlags_NoAutoMerge;
            ImGui::SetNextWindowClass(&modCreatorWindowClass);
            if (!ImGui::Begin("Mod Creator", &showModCreatorWindow_))
            {
                ImGui::End();
                return;
            }
            if (g_headingFont) ImGui::PushFont(g_headingFont);
            ImGui::TextUnformatted("Mod Creator");
            if (g_headingFont) ImGui::PopFont();
            ImGui::TextDisabled("Create a mod folder with metadata, scripts, and RPAK preload folders.");
            ImGui::Separator();
            ImGui::SetNextItemWidth(520.0f); (void)InputTextString("Name", modName_);
            ImGui::SetNextItemWidth(520.0f); (void)InputTextString("ID", modId_);
            ImGui::SetNextItemWidth(520.0f); (void)InputTextString("Version", modVersion_);
            ImGui::SetNextItemWidth(520.0f); (void)InputTextString("Author", modAuthor_);
            ImGui::SetNextItemWidth(680.0f); (void)InputTextString("Description", modDescription_, 1024);
            const auto hasInvalidName = [](const std::string& value) {
                return value.empty() || value.find_first_of("<>:\"/\\|?*") != std::string::npos ||
                    std::any_of(value.begin(), value.end(), [](const unsigned char c) { return c < 32; });
            };
            const auto hasInvalidId = [](const std::string& value) {
                return value.empty() || value == "." || value == ".." ||
                    std::any_of(value.begin(), value.end(), [](const unsigned char c) {
                        return !std::isalnum(c) && c != '_' && c != '-' && c != '.';
                    });
            };
            const bool validName = !hasInvalidName(modName_);
            const bool validId = !hasInvalidId(modId_);
            if (!validName) ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.42f, 1.0f), "Name contains invalid Windows filename characters.");
            if (!validId) ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.42f, 1.0f), "ID may only contain letters, numbers, dots, underscores, and hyphens.");
            ImGui::BeginDisabled(!validName || !validId);
            if (ImGui::Button("Create mod folder..."))
            {
                if (const auto folder = ChooseFolder(window, L"Choose a parent folder for the mod"))
                {
                    const fs::path root = *folder / PathFromUtf8(modId_);
                    std::error_code error;
                    fs::create_directories(root / "paks" / "Win64", error);
                    if (!error) fs::create_directories(root / "scripts" / "vscripts", error);
                    if (!error) fs::create_directories(root / "resource", error);
                    std::ofstream manifest;
                    std::ofstream preload;
                    if (!error)
                    {
                        manifest.open(root / "mod.vdf", std::ios::binary | std::ios::trunc);
                        manifest << "\"mod\"\n{\n"
                            << "\t\"name\" \"" << JsonEscape(modName_) << "\"\n"
                            << "\t\"id\" \"" << JsonEscape(modId_) << "\"\n"
                            << "\t\"description\" \"" << JsonEscape(modDescription_) << "\"\n"
                            << "\t\"version\" \"" << JsonEscape(modVersion_) << "\"\n"
                            << "\t\"author\" \"" << JsonEscape(modAuthor_) << "\"\n"
                            << "}\n";
                        preload.open(root / "paks" / "Win64" / "preload.rson", std::ios::binary | std::ios::trunc);
                        preload << "Paks:\n[\n]\n";
                    }
                    const bool created = !error && manifest.good() && preload.good();
                    if (created) deployModId_ = modId_;
                    AppendRePakLog(created ? firestar::repak::LogLevel::Info : firestar::repak::LogLevel::Error,
                        created ? "Created mod at " + PathToUtf8(root)
                            : "Could not create mod" + std::string(error ? ": " + error.message() : "."));
                }
            }
            ImGui::EndDisabled();
            ImGui::End();
        }

        static std::string TrimAssetReferenceText(const std::string_view value)
        {
            size_t first{};
            while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
            size_t last = value.size();
            while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
            return std::string(value.substr(first, last - first));
        }

        static std::string FormatAssetReferenceInline(const std::string_view source)
        {
            std::string text = TrimAssetReferenceText(source);
            for (const char* marker : {"**", "__"})
            {
                size_t position{};
                while ((position = text.find(marker, position)) != std::string::npos)
                    text.erase(position, 2);
            }
            text.erase(std::remove(text.begin(), text.end(), '`'), text.end());

            size_t search{};
            while ((search = text.find('[', search)) != std::string::npos)
            {
                const size_t labelEnd = text.find("](", search + 1);
                if (labelEnd == std::string::npos) break;
                const size_t urlEnd = text.find(')', labelEnd + 2);
                if (urlEnd == std::string::npos) break;
                const std::string label = text.substr(search + 1, labelEnd - search - 1);
                const std::string url = text.substr(labelEnd + 2, urlEnd - labelEnd - 2);
                const std::string replacement = label + " (" + url + ")";
                text.replace(search, urlEnd - search + 1, replacement);
                search += replacement.size();
            }
            return text;
        }

        static bool IsAssetReferenceOrderedList(const std::string_view line)
        {
            size_t index{};
            while (index < line.size() && std::isdigit(static_cast<unsigned char>(line[index]))) ++index;
            return index > 0 && index + 1 < line.size() && line[index] == '.' && line[index + 1] == ' ';
        }

        static bool IsAssetReferenceStructuralLine(const std::string_view line)
        {
            if (line.empty() || line.front() == '#' || line.front() == '|' || line.front() == '>') return true;
            if (line.rfind("```", 0) == 0 || line == "---" || line == "***") return true;
            if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) return true;
            return IsAssetReferenceOrderedList(line);
        }

        static std::vector<std::string> SplitAssetReferenceTableRow(const std::string_view source)
        {
            std::string row = TrimAssetReferenceText(source);
            if (!row.empty() && row.front() == '|') row.erase(row.begin());
            if (!row.empty() && row.back() == '|') row.pop_back();
            std::vector<std::string> cells;
            size_t begin{};
            for (;;)
            {
                const size_t separator = row.find('|', begin);
                cells.push_back(TrimAssetReferenceText(std::string_view(row).substr(begin,
                    separator == std::string::npos ? std::string::npos : separator - begin)));
                if (separator == std::string::npos) break;
                begin = separator + 1;
            }
            return cells;
        }

        static bool IsAssetReferenceTableSeparator(const std::vector<std::string>& cells)
        {
            if (cells.empty()) return false;
            for (const std::string& cell : cells)
            {
                size_t dashes{};
                for (const unsigned char character : cell)
                {
                    if (character == '-') ++dashes;
                    else if (character != ':' && !std::isspace(character)) return false;
                }
                if (dashes < 3) return false;
            }
            return true;
        }

        bool EnsureAssetTypesReferenceLoaded()
        {
            if (!assetTypesReferenceLines_.empty()) return true;
            const EmbeddedResource resource = LoadEmbeddedResource(ResourceRePakAssetTypesReference);
            if (!resource) return false;

            assetTypesReferenceMarkdown_.assign(reinterpret_cast<const char*>(resource.bytes), resource.size);
            if (assetTypesReferenceMarkdown_.size() >= 3 &&
                static_cast<unsigned char>(assetTypesReferenceMarkdown_[0]) == 0xEF &&
                static_cast<unsigned char>(assetTypesReferenceMarkdown_[1]) == 0xBB &&
                static_cast<unsigned char>(assetTypesReferenceMarkdown_[2]) == 0xBF)
                assetTypesReferenceMarkdown_.erase(0, 3);

            std::istringstream input(assetTypesReferenceMarkdown_);
            std::string line;
            while (std::getline(input, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                assetTypesReferenceLines_.push_back(std::move(line));
            }
            if (assetTypesReferenceLines_.empty()) return false;

            size_t sectionStart{};
            std::string sectionTitle = "Getting started";
            const auto addSection = [&](const size_t first, const size_t last, const std::string& title) {
                if (first >= last) return;
                AssetTypesReferenceSection section;
                section.title = title;
                section.firstLine = first;
                section.lastLine = last;
                section.searchableText = ToLowerAscii(title);
                for (size_t index = first; index < last; ++index)
                {
                    section.searchableText.push_back('\n');
                    section.searchableText += ToLowerAscii(assetTypesReferenceLines_[index]);
                }
                assetTypesReferenceSections_.push_back(std::move(section));
            };
            for (size_t index = 0; index < assetTypesReferenceLines_.size(); ++index)
            {
                const std::string& candidate = assetTypesReferenceLines_[index];
                if (candidate.rfind("## ", 0) != 0) continue;
                addSection(sectionStart, index, sectionTitle);
                sectionStart = index;
                sectionTitle = FormatAssetReferenceInline(std::string_view(candidate).substr(3));
            }
            addSection(sectionStart, assetTypesReferenceLines_.size(), sectionTitle);
            return !assetTypesReferenceSections_.empty();
        }

        void DrawAssetReferenceMarkdown(const size_t firstLine, const size_t lastLine)
        {
            size_t index = firstLine;
            while (index < lastLine)
            {
                const std::string& line = assetTypesReferenceLines_[index];
                if (line.empty())
                {
                    ImGui::Spacing();
                    ++index;
                    continue;
                }

                if (line.rfind("```", 0) == 0)
                {
                    const std::string language = TrimAssetReferenceText(std::string_view(line).substr(3));
                    const size_t blockLine = index++;
                    std::string code;
                    size_t codeLines{};
                    while (index < lastLine && assetTypesReferenceLines_[index].rfind("```", 0) != 0)
                    {
                        if (!code.empty()) code.push_back('\n');
                        code += assetTypesReferenceLines_[index++];
                        ++codeLines;
                    }
                    if (index < lastLine) ++index;
                    ImGui::PushID(static_cast<int>(blockLine));
                    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(code.c_str());
                    if (!language.empty())
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", language.c_str());
                    }
                    const float height = (std::min)(360.0f,
                        (std::max)(52.0f, static_cast<float>(codeLines) * ImGui::GetTextLineHeightWithSpacing() + 14.0f));
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.045f, 0.048f, 0.055f, 1.0f));
                    ImGui::BeginChild("code", ImVec2(0.0f, height), ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_HorizontalScrollbar);
                    ImGui::TextUnformatted(code.c_str());
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                    continue;
                }

                if (line.front() == '|')
                {
                    const size_t tableLine = index;
                    std::vector<std::vector<std::string>> rows;
                    while (index < lastLine && !assetTypesReferenceLines_[index].empty() &&
                        assetTypesReferenceLines_[index].front() == '|')
                        rows.push_back(SplitAssetReferenceTableRow(assetTypesReferenceLines_[index++]));
                    if (!rows.empty() && !rows.front().empty())
                    {
                        const int columns = static_cast<int>(rows.front().size());
                        const std::string tableId = "##asset_reference_table_" + std::to_string(tableLine);
                        if (ImGui::BeginTable(tableId.c_str(), columns,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollX))
                        {
                            for (const std::string& cell : rows.front())
                            {
                                const std::string heading = FormatAssetReferenceInline(cell);
                                ImGui::TableSetupColumn(heading.c_str(), ImGuiTableColumnFlags_WidthStretch);
                            }
                            ImGui::TableHeadersRow();
                            size_t row = rows.size() > 1 && IsAssetReferenceTableSeparator(rows[1]) ? 2 : 1;
                            for (; row < rows.size(); ++row)
                            {
                                ImGui::TableNextRow();
                                for (int column = 0; column < columns; ++column)
                                {
                                    ImGui::TableSetColumnIndex(column);
                                    const std::string cell = column < static_cast<int>(rows[row].size())
                                        ? FormatAssetReferenceInline(rows[row][column]) : std::string{};
                                    ImGui::TextWrapped("%s", cell.c_str());
                                }
                            }
                            ImGui::EndTable();
                        }
                    }
                    continue;
                }

                if (line.front() == '#')
                {
                    size_t level{};
                    while (level < line.size() && line[level] == '#') ++level;
                    const std::string heading = FormatAssetReferenceInline(std::string_view(line).substr(level));
                    if (level <= 2 && g_headingFont) ImGui::PushFont(g_headingFont);
                    ImGui::TextColored(level <= 3 ? ImVec4(1.0f, 0.59f, 0.24f, 1.0f)
                        : ImVec4(0.94f, 0.94f, 0.96f, 1.0f), "%s", heading.c_str());
                    if (level <= 2 && g_headingFont) ImGui::PopFont();
                    if (level <= 2) ImGui::Separator();
                    ++index;
                    continue;
                }

                if (line == "---" || line == "***")
                {
                    ImGui::Separator();
                    ++index;
                    continue;
                }

                if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0)
                {
                    ImGui::Bullet();
                    ImGui::SameLine();
                    const std::string item = FormatAssetReferenceInline(std::string_view(line).substr(2));
                    ImGui::TextWrapped("%s", item.c_str());
                    ++index;
                    continue;
                }

                if (IsAssetReferenceOrderedList(line))
                {
                    const std::string item = FormatAssetReferenceInline(line);
                    ImGui::Indent(12.0f);
                    ImGui::TextWrapped("%s", item.c_str());
                    ImGui::Unindent(12.0f);
                    ++index;
                    continue;
                }

                if (line.front() == '>')
                {
                    const std::string quote = FormatAssetReferenceInline(std::string_view(line).substr(1));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.73f, 0.76f, 0.82f, 1.0f));
                    ImGui::Indent(14.0f);
                    ImGui::TextWrapped("%s", quote.c_str());
                    ImGui::Unindent(14.0f);
                    ImGui::PopStyleColor();
                    ++index;
                    continue;
                }

                std::string paragraph = line;
                ++index;
                while (index < lastLine && !IsAssetReferenceStructuralLine(assetTypesReferenceLines_[index]))
                {
                    paragraph.push_back(' ');
                    paragraph += assetTypesReferenceLines_[index++];
                }
                paragraph = FormatAssetReferenceInline(paragraph);
                ImGui::TextWrapped("%s", paragraph.c_str());
            }
        }

        void DrawAssetTypesReferenceWindow()
        {
            if (!showAssetTypesReference_) return;
            ImGuiWindowClass referenceWindowClass{};
            referenceWindowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoTaskBarIcon |
                ImGuiViewportFlags_NoAutoMerge;
            ImGui::SetNextWindowClass(&referenceWindowClass);
            ImGui::SetNextWindowSize(ImVec2(1180.0f, 760.0f), ImGuiCond_FirstUseEver);
            const float maximumWindowSize = (std::numeric_limits<float>::max)();
            ImGui::SetNextWindowSizeConstraints(ImVec2(760.0f, 480.0f),
                ImVec2(maximumWindowSize, maximumWindowSize));
            if (!ImGui::Begin("RePak Asset Reference", &showAssetTypesReference_, ImGuiWindowFlags_NoCollapse))
            {
                ImGui::End();
                return;
            }

            if (g_headingFont) ImGui::PushFont(g_headingFont);
            ImGui::TextColored(ImVec4(1.0f, 0.59f, 0.24f, 1.0f), "RePak asset reference");
            if (g_headingFont) ImGui::PopFont();
            ImGui::TextDisabled("Build maps, supported inputs, asset schemas, examples, and troubleshooting.");
            ImGui::SetNextItemWidth(-80.0f);
            ImGui::InputTextWithHint("##asset_reference_search", "Search asset types, fields, errors, or examples...",
                assetTypesReferenceSearch_.data(), assetTypesReferenceSearch_.size());
            ImGui::SameLine();
            if (ImGui::Button("Clear")) assetTypesReferenceSearch_.fill('\0');
            ImGui::Separator();

            if (!EnsureAssetTypesReferenceLoaded())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.42f, 1.0f),
                    "The RePak asset reference could not be loaded.");
                ImGui::End();
                return;
            }

            const std::string query = ToLowerAscii(assetTypesReferenceSearch_.data());
            const auto matches = [&](const AssetTypesReferenceSection& section) {
                return query.empty() || section.searchableText.find(query) != std::string::npos;
            };

            ImGui::BeginChild("AssetReferenceOutline", ImVec2(275.0f, 0.0f), ImGuiChildFlags_Borders);
            ImGui::TextDisabled("CONTENTS");
            ImGui::Separator();
            for (size_t section = 0; section < assetTypesReferenceSections_.size(); ++section)
            {
                if (!matches(assetTypesReferenceSections_[section])) continue;
                const bool selected = assetTypesReferenceSelectedSection_ == static_cast<int>(section);
                if (ImGui::Selectable(assetTypesReferenceSections_[section].title.c_str(), selected))
                {
                    assetTypesReferenceSelectedSection_ = static_cast<int>(section);
                    assetTypesReferenceScrollTarget_ = static_cast<int>(section);
                }
            }
            ImGui::EndChild();
            ImGui::SameLine();

            ImGui::BeginChild("AssetReferenceContent", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders,
                ImGuiWindowFlags_HorizontalScrollbar);
            bool anyMatch{};
            for (size_t section = 0; section < assetTypesReferenceSections_.size(); ++section)
            {
                const AssetTypesReferenceSection& referenceSection = assetTypesReferenceSections_[section];
                if (!matches(referenceSection)) continue;
                anyMatch = true;
                ImGui::PushID(static_cast<int>(section));
                ImGui::Dummy(ImVec2(0.0f, 1.0f));
                if (assetTypesReferenceScrollTarget_ == static_cast<int>(section))
                {
                    ImGui::SetScrollHereY(0.0f);
                    assetTypesReferenceScrollTarget_ = -1;
                }
                DrawAssetReferenceMarkdown(referenceSection.firstLine, referenceSection.lastLine);
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::PopID();
            }
            if (!anyMatch) ImGui::TextDisabled("No reference sections match that search.");
            ImGui::EndChild();
            ImGui::End();
        }

        void DrawAboutWindow()
        {
            if (!showAbout_)
                return;
            ImGui::SetNextWindowSize(ImVec2(520.0f, 310.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("About Firestar", &showAbout_, ImGuiWindowFlags_NoCollapse))
            {
                if (g_headingFont) ImGui::PushFont(g_headingFont);
                ImGui::TextColored(ImVec4(1.0f, 0.60f, 0.23f, 1.0f), "FIRESTAR");
                if (g_headingFont) ImGui::PopFont();
                ImGui::TextUnformatted("RPAK tools for Apex Legends");
                ImGui::Text("Version %s", FIRESTAR_VERSION_STRING);
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", FIRESTAR_UPDATE_STRING);
                ImGui::Separator();
                ImGui::TextUnformatted("Made by WateryContinent");
                ImGui::TextDisabled("GitHub: @WateryContinent");
                if (ImGui::Button("Open GitHub profile"))
                    ShellExecuteW(nullptr, L"open", L"https://github.com/WateryContinent", nullptr, nullptr, SW_SHOWNORMAL);
                ImGui::Spacing();
                ImGui::TextWrapped("Create RPAKs, edit asset manifests, build UI atlases, and share packed Firestar projects.");
                ImGui::Spacing();
                ImGui::TextDisabled("Copyright (c) 2026 WateryContinent");
                ImGui::TextDisabled("Licensed under the MIT License.");
            }
            ImGui::End();
        }

        void HandleAutosave()
        {
            const double now = ImGui::GetTime();
            if (lastAutosaveTime_ == 0.0)
            {
                lastAutosaveTime_ = now;
                return;
            }
            if (!autosaveEnabled_ || !repakProject_.IsOpen() || !repakProject_.IsDirty() ||
                currentRePakProjectPath_.empty() || repakBuilding_) return;
            if (now - lastAutosaveTime_ < static_cast<double>(autosaveMinutes_) * 60.0) return;
            lastAutosaveTime_ = now;
            if (SaveFirestarProjectTo(currentRePakProjectPath_))
                AppendRePakLog(firestar::repak::LogLevel::Info, "Autosaved the current project.");
        }

        void DrawEditorSettings(const HWND window)
        {
            if (!showEditorSettings_)
                return;
            ImGuiWindowClass settingsWindowClass{};
            settingsWindowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoTaskBarIcon |
                ImGuiViewportFlags_NoAutoMerge | ImGuiViewportFlags_NoDecoration;
            ImGui::SetNextWindowClass(&settingsWindowClass);
            ImGui::SetNextWindowSize(ImVec2(720.0f, 450.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Firestar Settings", &showEditorSettings_))
            {
                ImGui::SeparatorText("Apex deployment");
                ImGui::TextDisabled("Firestar only writes here after an explicit Deploy confirmation.");
                ImGui::SetNextItemWidth(-96.0f);
                (void)InputTextString("##apex_install", apexInstallPath_, 2048);
                ImGui::SameLine();
                if (ImGui::Button("Browse..."))
                    if (const auto folder = ChooseFolder(window)) apexInstallPath_ = PathToUtf8(*folder);
                ImGui::Checkbox("Deploy as a mod", &deployAsMod_);
                if (deployAsMod_)
                {
                    ImGui::SetNextItemWidth(360.0f);
                    (void)InputTextString("Mod ID", deployModId_, 128);
                    if (!ValidDeployModId())
                        ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.42f, 1.0f),
                            "Use only letters, numbers, dots, underscores, and hyphens.");
                }
                ImGui::TextDisabled(deployAsMod_
                    ? "Deploys to mods/<ID>/paks/Win64 and updates that mod's preload.rson."
                    : "Deploys to paks/Win64 and updates preload.rson.");
                ImGui::SeparatorText("Autosave");
                if (ImGui::Checkbox("Autosave Firestar projects", &autosaveEnabled_))
                    lastAutosaveTime_ = ImGui::GetTime();
                ImGui::BeginDisabled(!autosaveEnabled_);
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::InputInt("Minutes between saves", &autosaveMinutes_))
                {
                    autosaveMinutes_ = (std::clamp)(autosaveMinutes_, 1, 60);
                    lastAutosaveTime_ = ImGui::GetTime();
                }
                ImGui::EndDisabled();
                ImGui::TextDisabled(currentRePakProjectPath_.empty()
                    ? "Autosave starts after the project has been saved once."
                    : "Autosaves to the current .fsp file while the project has changes.");
                if (ImGui::Button("Save settings"))
                {
                    std::string error;
                    AppendRePakLog(SaveEditorSettings(error) ? firestar::repak::LogLevel::Info : firestar::repak::LogLevel::Error,
                        error.empty() ? "Saved Firestar settings." : error);
                }
            }
            ImGui::End();
        }

        void DrawRePakSettings()
        {
            if (!showRePakSettings_ || !repakProject_.IsOpen()) return;
            ImGui::SetNextWindowSize(ImVec2(760.0f, 680.0f), ImGuiCond_FirstUseEver);
            if (!ImGui::Begin("RPAK Settings", &showRePakSettings_))
            {
                ImGui::End();
                return;
            }
            rapidjson::Document& document = repakProject_.Document();
            auto& allocator = document.GetAllocator();
            const auto getString = [&document](const char* name, const char* fallback = "") {
                const auto member = document.FindMember(name);
                return member != document.MemberEnd() && member->value.IsString()
                    ? std::string(member->value.GetString(), member->value.GetStringLength())
                    : std::string(fallback);
            };
            const auto getBool = [&document](const char* name, const bool fallback) {
                const auto member = document.FindMember(name);
                return member != document.MemberEnd() && member->value.IsBool()
                    ? member->value.GetBool() : fallback;
            };
            const auto getInt = [&document](const char* name, const int fallback) {
                const auto member = document.FindMember(name);
                return member != document.MemberEnd() && member->value.IsInt()
                    ? member->value.GetInt() : fallback;
            };
            const auto setString = [&document, &allocator](const char* name, const std::string& value,
                const bool optional = false) {
                auto member = document.FindMember(name);
                if (optional && value.empty())
                {
                    if (member != document.MemberEnd()) document.RemoveMember(member);
                    return;
                }
                if (member != document.MemberEnd()) member->value.SetString(value.c_str(), allocator);
                else document.AddMember(rapidjson::Value(name, allocator),
                    rapidjson::Value(value.c_str(), allocator), allocator);
            };
            const auto setBool = [&document, &allocator](const char* name, const bool value) {
                auto member = document.FindMember(name);
                if (member != document.MemberEnd()) member->value.SetBool(value);
                else
                {
                    rapidjson::Value key(name, allocator);
                    rapidjson::Value stored(value);
                    document.AddMember(key, stored, allocator);
                }
            };
            const auto setInt = [&document, &allocator](const char* name, const int value) {
                auto member = document.FindMember(name);
                if (member != document.MemberEnd()) member->value.SetInt(value);
                else
                {
                    rapidjson::Value key(name, allocator);
                    rapidjson::Value stored(value);
                    document.AddMember(key, stored, allocator);
                }
            };
            const auto commit = [this]() {
                repakProject_.RefreshDerivedPaths();
                repakProject_.MarkDirty();
            };
            const auto drawStringRow = [](const char* label, const char* id, std::string& value,
                const size_t bufferSize) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-1.0f);
                return InputTextString(id, value, bufferSize);
            };
            const auto drawIntRow = [](const char* label, const char* id, int& value) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-1.0f);
                return ImGui::InputInt(id, &value);
            };

            ImGui::TextDisabled("Settings written at the top of the active RePak build map.");
            ImGui::SeparatorText("Package");
            std::string name = getString("name", repakProject_.Name().c_str());
            std::string assetsDirectory = getString("assetsDir", "assets/");
            std::string outputDirectory = getString("outputDir", "build/");
            std::string mandatory = getString("streamFileMandatory");
            std::string optional = getString("streamFileOptional");
            if (ImGui::BeginTable("PackageSettings", 2, ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Project name");
                ImGui::TableSetColumnIndex(1);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(FirestarProjectName().c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The project name comes from the .fsp filename. Use Save As to rename it.");
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("RPAK version");
                ImGui::TableSetColumnIndex(1);
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%d", repakProject_.PakVersion());
                const std::string previousName = name;
                if (drawStringRow("Package name", "##package_name", name, 512))
                {
                    name = FileStemSafe(name);
                    const bool mandatoryWasGenerated = mandatory.empty() ||
                        mandatory == DefaultMandatoryStarPakPath(previousName);
                    const bool optionalWasGenerated = optional.empty() ||
                        optional == DefaultOptionalStarPakPath(previousName);
                    setString("name", name);
                    if (mandatoryWasGenerated)
                    {
                        mandatory = DefaultMandatoryStarPakPath(name);
                        setString("streamFileMandatory", mandatory);
                    }
                    if (repakProject_.PakVersion() >= 8 && optionalWasGenerated)
                    {
                        optional = DefaultOptionalStarPakPath(name);
                        setString("streamFileOptional", optional);
                    }
                    commit();
                }
                if (drawStringRow("Assets directory", "##assets_directory", assetsDirectory, 2048))
                { setString("assetsDir", assetsDirectory); commit(); }
                if (drawStringRow("Output directory", "##output_directory", outputDirectory, 2048))
                { setString("outputDir", outputDirectory); commit(); }
                ImGui::EndTable();
            }

            ImGui::SeparatorText("Content");
            bool keepDev = getBool("keepDevOnly", false);
            if (ImGui::Checkbox("Keep development names", &keepDev)) { setBool("keepDevOnly", keepDev); commit(); }
            bool keepServer = getBool("keepServerOnly", true);
            if (ImGui::Checkbox("Keep server content", &keepServer)) { setBool("keepServerOnly", keepServer); commit(); }
            bool keepClient = getBool("keepClientOnly", true);
            if (ImGui::Checkbox("Keep client content", &keepClient)) { setBool("keepClientOnly", keepClient); commit(); }
            bool dynamicLibrary = getBool("hasDynamicLibrary", false);
            if (ImGui::Checkbox("Package has a matching DLL", &dynamicLibrary))
            { setBool("hasDynamicLibrary", dynamicLibrary); commit(); }
            bool debugInfo = getBool("showDebugInfo", false);
            if (ImGui::Checkbox("Verbose build logging", &debugInfo)) { setBool("showDebugInfo", debugInfo); commit(); }

            ImGui::SeparatorText("Streaming");
            if (ImGui::BeginTable("StreamingSettings", 2, ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                if (drawStringRow("Mandatory starpak", "##mandatory_starpak", mandatory, 2048))
                { setString("streamFileMandatory", mandatory, true); commit(); }
                ImGui::BeginDisabled(repakProject_.PakVersion() < 8);
                if (drawStringRow("Optional starpak", "##optional_starpak", optional, 2048))
                { setString("streamFileOptional", optional, true); commit(); }
                ImGui::EndDisabled();
                ImGui::EndTable();
            }
            if (ImGui::Button("Use package StarPak names"))
            {
                mandatory = DefaultMandatoryStarPakPath(name);
                optional = DefaultOptionalStarPakPath(name);
                setString("streamFileMandatory", mandatory);
                if (repakProject_.PakVersion() >= 8)
                    setString("streamFileOptional", optional);
                commit();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Defaults: <package>.starpak and <package>.opt.starpak");
            ImGui::TextDisabled("Packed audio and mandatory-streamed assets require a mandatory StarPak path.");

            ImGui::SeparatorText("Input performance");
            int ioWorkers = (std::clamp)(getInt("ioWorkers", 0), 0, 64);
            int fileCache = (std::clamp)(getInt("fileCacheMB", 0), 0, 32768);
            if (ImGui::BeginTable("InputPerformanceSettings", 2, ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                if (drawIntRow("I/O workers", "##io_workers", ioWorkers))
                { ioWorkers = (std::clamp)(ioWorkers, 0, 64); setInt("ioWorkers", ioWorkers); commit(); }
                if (drawIntRow("File cache (MiB)", "##file_cache", fileCache))
                { fileCache = (std::clamp)(fileCache, 0, 32768); setInt("fileCacheMB", fileCache); commit(); }
                ImGui::EndTable();
            }

            ImGui::SeparatorText("Compression");
            int compressionLevel = getInt("compressLevel", 0);
            bool compressionEnabled = compressionLevel > 0;
            if (ImGui::Checkbox("Compress RPAK", &compressionEnabled))
            {
                compressionLevel = compressionEnabled ? 3 : 0;
                setInt("compressLevel", compressionLevel);
                commit();
            }
            ImGui::BeginDisabled(!compressionEnabled);
            compressionLevel = (std::clamp)(compressionLevel, 1, 22);
            int compressionWorkers = (std::clamp)(getInt("compressWorkers", 0), 0, 64);
            if (ImGui::BeginTable("CompressionSettings", 2, ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Compression level");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderInt("##compression_level", &compressionLevel, 1, 22))
                { setInt("compressLevel", compressionLevel); commit(); }
                if (drawIntRow("Compression workers", "##compression_workers", compressionWorkers))
                {
                    compressionWorkers = (std::clamp)(compressionWorkers, 0, 64);
                    setInt("compressWorkers", compressionWorkers);
                    commit();
                }
                ImGui::EndTable();
            }
            ImGui::EndDisabled();
            ImGui::End();
        }

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
            // Atlas source images are sRGB artwork. Sampling them through an
            // sRGB view preserves their appearance in Firestar's scRGB UI.
            description.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
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

        [[nodiscard]] bool AddAtlasToOpenRePak(std::string& error)
        {
            if (!repakProject_.IsOpen())
            {
                error = "Open an RPAK project first.";
                return false;
            }
            if (!Validate(error) || (!packed_ && !Pack(error)))
                return false;

            const std::string texturePath = NormalizeAssetPath(settings_.texturePath);
            const std::string uimgPath = NormalizeAssetPath(settings_.uimgPath);
            const std::string textureWithoutRpak = texturePath.substr(0, texturePath.size() - std::string_view(".rpak").size());
            const fs::path ddsPath = repakProject_.AssetsDirectory() / PathFromUtf8(textureWithoutRpak + ".dds");
            if (!WriteDds(ddsPath, error))
                return false;

            const auto findAsset = [this](const std::string& type, const std::string& path) -> std::optional<size_t> {
                for (size_t index = 0; index < repakProject_.AssetCount(); ++index)
                {
                    const rapidjson::Value* asset = repakProject_.Asset(index);
                    if (asset && AssetMemberString(*asset, "_type") == type && AssetMemberString(*asset, "_path") == path)
                        return index;
                }
                return std::nullopt;
            };

            const std::optional<size_t> existingTexture = findAsset("txtr", texturePath);
            const size_t textureIndex = existingTexture ? *existingTexture : repakProject_.AddAsset("txtr", texturePath);
            rapidjson::Value* texture = repakProject_.Asset(textureIndex);
            auto& allocator = repakProject_.Allocator();
            if (!texture->HasMember("$disableStreaming")) texture->AddMember("$disableStreaming", true, allocator);
            else (*texture)["$disableStreaming"].SetBool(true);

            const std::optional<size_t> existingUimg = findAsset("uimg", uimgPath);
            const size_t uimgIndex = existingUimg ? *existingUimg : repakProject_.AddAsset("uimg", uimgPath);
            rapidjson::Value* uimg = repakProject_.Asset(uimgIndex);
            if (uimg->HasMember("atlas")) (*uimg)["atlas"].SetString(texturePath.c_str(), allocator);
            else uimg->AddMember("atlas", rapidjson::Value(texturePath.c_str(), allocator), allocator);
            rapidjson::Value images(rapidjson::kArrayType);
            for (const Sprite& sprite : sprites_)
            {
                rapidjson::Value image(rapidjson::kObjectType);
                const std::string path = NormalizeAssetPath(sprite.ruiPath);
                image.AddMember("path", rapidjson::Value(path.c_str(), allocator), allocator);
                image.AddMember("posX", sprite.packed.x, allocator);
                image.AddMember("posY", sprite.packed.y, allocator);
                image.AddMember("width", sprite.width, allocator);
                image.AddMember("height", sprite.height, allocator);
                images.PushBack(image, allocator);
            }
            if (uimg->HasMember("images")) (*uimg)["images"] = std::move(images);
            else uimg->AddMember("images", images, allocator);
            repakProject_.MarkDirty();
            selectedAsset_ = static_cast<int>(uimgIndex);
            RefreshSourceData();
            AppendRePakLog(firestar::repak::LogLevel::Info,
                "Added atlas texture and UIMG to the open project.");
            SaveAfterAssetAdded();
            return true;
        }

        void DrawMenu(const HWND window)
        {
            if (!ImGui::BeginMainMenuBar())
                return;
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Home")) RequestHome();
                ImGui::Separator();
                if (ImGui::MenuItem("New RPAK...", "Ctrl+N")) CreateRePakProject(window);
                if (ImGui::MenuItem("Open Firestar project...", "Ctrl+O"))
                    if (const auto path = ChooseFirestarProjectFile(window, false)) LoadFirestarProject(*path);
                if (ImGui::MenuItem("Open packed project..."))
                    if (const auto path = ChoosePackedProjectFile(window, false)) OpenPackedProject(*path);
                if (ImGui::MenuItem("Import RePak JSON..."))
                    if (const auto path = ChooseRePakManifest(window, false)) LoadRePakProject(*path);
                ImGui::BeginDisabled(!repakProject_.IsOpen());
                if (ImGui::MenuItem("Save project", "Ctrl+S")) SaveFirestarProject(window);
                if (ImGui::MenuItem("Save project as...", "Ctrl+Shift+S")) SaveFirestarProject(window, true);
                if (ImGui::MenuItem("Export RePak JSON...")) ExportRePakJson(window);
                if (ImGui::MenuItem("Pack and export project...")) PackAndExportProject(window);
                ImGui::EndDisabled();
                ImGui::Separator();
                if (ImGui::MenuItem("Open atlas project..."))
                    if (const auto path = ChooseProjectFile(window, false))
                    {
                        OpenAtlasWindow();
                        LoadProjectWithFeedback(*path);
                    }
                if (showAtlasWindow_ && ImGui::MenuItem("Save atlas project"))
                {
                    if (currentProjectPath_.empty())
                    {
                        if (const auto path = ChooseProjectFile(window, true)) SaveProjectWithFeedback(*path);
                    }
                    else SaveProjectWithFeedback(currentProjectPath_);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit"))
                    PostMessageW(window, WM_CLOSE, 0, 0);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Build"))
            {
                ImGui::BeginDisabled(!repakProject_.IsOpen() || repakBuilding_);
                if (ImGui::MenuItem("Build open RPAK", "Ctrl+B")) StartRePakBuild();
                ImGui::EndDisabled();
                if (showAtlasWindow_)
                {
                    ImGui::Separator();
                    if (ImGui::MenuItem("Pack atlas preview", "Ctrl+P")) PackWithFeedback();
                    if (ImGui::MenuItem("Export atlas DDS + JSON", "Ctrl+Shift+E")) ExportWithFeedback();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Tools"))
            {
                if (ImGui::MenuItem("UI Atlas")) OpenAtlasWindow();
                if (ImGui::MenuItem("Mod Creator")) showModCreatorWindow_ = true;
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Settings"))
            {
                showEditorSettings_ = true;
            }
            if (ImGui::BeginMenu("Help"))
            {
                if (ImGui::MenuItem("RePak asset reference")) showAssetTypesReference_ = true;
                ImGui::Separator();
                if (ImGui::MenuItem("About Firestar")) showAbout_ = true;
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        void HandleShortcuts(const HWND window)
        {
            if (ImGui::GetIO().WantTextInput)
                return;
            constexpr ImGuiInputFlags route = ImGuiInputFlags_RouteGlobal;
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, route))
            {
                if (const auto path = ChooseFirestarProjectFile(window, false))
                    LoadFirestarProject(*path);
            }
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, route))
                CreateRePakProject(window);
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, route) && repakProject_.IsOpen())
                (void)SaveFirestarProject(window, true);
            else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, route) && repakProject_.IsOpen())
                (void)SaveFirestarProject(window);
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, route) && repakProject_.IsOpen())
                StartRePakBuild();
            if (showAtlasWindow_ && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, route))
                PackWithFeedback();
            if (showAtlasWindow_ && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_E, route))
                ExportWithFeedback();
        }

        void DrawSettingsWindow(const HWND window)
        {
            if (!showSettingsWindow_)
                return;

            ImGuiWindowClass settingsWindowClass{};
            settingsWindowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge |
                ImGuiViewportFlags_NoTaskBarIcon | ImGuiViewportFlags_NoDecoration;
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
            ImGui::SameLine();
            ImGui::BeginDisabled(!repakProject_.IsOpen());
            if (ImGui::Button("Add to RPAK"))
            {
                std::string error;
                if (AddAtlasToOpenRePak(error))
                    SetStatus("Added atlas assets to " + repakProject_.Name() + ".");
                else
                    SetStatus(error, true);
            }
            ImGui::EndDisabled();
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
                ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
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

    [[nodiscard]] int RunProjectSelfTest(const fs::path& outputDirectory)
    {
        AtlasApplication app;
        std::string error;
        if (!app.CreateProjectSelfTest(outputDirectory, error))
        {
            OutputDebugStringA(("Firestar project self-test failed: " + error + "\n").c_str());
            return 1;
        }
        OutputDebugStringA("Firestar project self-test passed.\n");
        return 0;
    }

    [[nodiscard]] int RunArchiveSelfTest(const fs::path& outputDirectory)
    {
        AtlasApplication app;
        std::string error;
        if (!app.CreateArchiveSelfTest(outputDirectory, error))
        {
            OutputDebugStringA(("Firestar archive self-test failed: " + error + "\n").c_str());
            std::error_code ioError;
            fs::create_directories(outputDirectory, ioError);
            std::ofstream(outputDirectory / L"archive-self-test-error.txt", std::ios::trunc) << error;
            return 1;
        }
        OutputDebugStringA("Firestar archive self-test passed.\n");
        return 0;
    }

    [[nodiscard]] int RunModelPreviewSelfTest(const fs::path& modelPath)
    {
        firestar::editor::RmdlPreviewData preview;
        std::string error;
        if (!firestar::editor::LoadRmdlPreview(modelPath, 0, preview, error))
        {
            OutputDebugStringA(("Firestar model preview self-test failed: " + error + "\n").c_str());
            std::ofstream(GetExecutableDirectory() / L"model-preview-self-test-error.txt", std::ios::trunc) << error;
            return 1;
        }
        if (preview.vertices.empty() || preview.indices.size() < 3 || preview.meshes.empty())
        {
            OutputDebugStringA("Firestar model preview self-test failed: no renderable geometry.\n");
            return 1;
        }
        for (const auto& vertex : preview.vertices)
        {
            if (!std::isfinite(vertex.u) || !std::isfinite(vertex.v))
            {
                OutputDebugStringA("Firestar model preview self-test failed: invalid texture coordinates.\n");
                return 1;
            }
        }
        const std::string message = "Firestar model preview self-test passed: " +
            std::to_string(preview.vertices.size()) + " vertices, " +
            std::to_string(preview.indices.size() / 3) + " triangles.\n";
        OutputDebugStringA(message.c_str());
        std::error_code cleanupError;
        fs::remove(GetExecutableDirectory() / L"model-preview-self-test-error.txt", cleanupError);
        return 0;
    }

    [[nodiscard]] int RunAudioPreviewSelfTest(const fs::path& audioPath)
    {
        firestar::editor::AudioPreviewPlayer player;
        std::string error;
        if (!player.Load(audioPath, error))
        {
            OutputDebugStringA(("Firestar audio preview self-test failed to load: " + error + "\n").c_str());
            return 1;
        }
        if (!player.Play(error))
        {
            OutputDebugStringA(("Firestar audio preview self-test failed to start: " + error + "\n").c_str());
            return 1;
        }
        Sleep(250);
        player.Update();
        const double position = player.PositionSeconds();
        if (position < 0.05)
        {
            OutputDebugStringA("Firestar audio preview self-test failed: the XAudio2 voice did not advance.\n");
            return 1;
        }
        player.Stop();
        const std::string message = "Firestar audio preview self-test passed: " +
            std::to_string(player.DurationSeconds()) + " seconds, advanced to " +
            std::to_string(position) + " seconds.\n";
        OutputDebugStringA(message.c_str());
        return 0;
    }
}

int APIENTRY wWinMain(const HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    int argumentCount{};
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    std::optional<fs::path> projectToOpen;
    std::optional<fs::path> repakToOpen;
    std::optional<fs::path> firestarProjectToOpen;
    std::optional<fs::path> packedProjectToOpen;
    if (arguments)
    {
        bool selfTest = false;
        bool projectSelfTest = false;
        bool archiveSelfTest = false;
        std::optional<fs::path> modelPreviewSelfTest;
        std::optional<fs::path> audioPreviewSelfTest;
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
            else if (_wcsicmp(arguments[index], L"--project-self-test") == 0)
            {
                projectSelfTest = true;
                if (index + 1 < argumentCount && arguments[index + 1][0] != L'-')
                    selfTestOutput = arguments[++index];
            }
            else if (_wcsicmp(arguments[index], L"--archive-self-test") == 0)
            {
                archiveSelfTest = true;
                if (index + 1 < argumentCount && arguments[index + 1][0] != L'-')
                    selfTestOutput = arguments[++index];
            }
            else if (_wcsicmp(arguments[index], L"--model-preview-self-test") == 0)
            {
                if (index + 1 < argumentCount)
                    modelPreviewSelfTest = fs::path(arguments[++index]);
            }
            else if (_wcsicmp(arguments[index], L"--audio-preview-self-test") == 0)
            {
                if (index + 1 < argumentCount)
                    audioPreviewSelfTest = fs::path(arguments[++index]);
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
                else if (_wcsicmp(candidate.extension().c_str(), L".fsp") == 0)
                    firestarProjectToOpen = candidate;
                else if (_wcsicmp(candidate.extension().c_str(), L".fspa") == 0)
                    packedProjectToOpen = candidate;
                else if (_wcsicmp(candidate.extension().c_str(), L".json") == 0)
                    repakToOpen = candidate;
            }
        }
        LocalFree(arguments);
        if (registerFsa)
        {
            std::string error;
            return RegisterFsaFileAssociation(error) ? 0 : 1;
        }
        if (selfTest || projectSelfTest || archiveSelfTest || modelPreviewSelfTest || audioPreviewSelfTest)
        {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const int result = audioPreviewSelfTest
                ? RunAudioPreviewSelfTest(*audioPreviewSelfTest)
                : modelPreviewSelfTest
                ? RunModelPreviewSelfTest(*modelPreviewSelfTest)
                : archiveSelfTest
                    ? RunArchiveSelfTest(selfTestOutput)
                    : projectSelfTest
                        ? RunProjectSelfTest(selfTestOutput)
                        : RunSelfTest(selfTestOutput);
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
    g_imguiIniPath = PathToUtf8(GetImGuiIniPath());
    io.IniFilename = g_imguiIniPath.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
    ApplyFirestarStyle();
    ImGui::GetStyle().WindowRounding = 0.0f;
    LoadFirestarFonts();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_context);
    DXGI_SWAP_CHAIN_DESC viewportSwapChain{};
    viewportSwapChain.BufferDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    viewportSwapChain.SampleDesc.Count = 1;
    viewportSwapChain.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    viewportSwapChain.BufferCount = 2;
    viewportSwapChain.Windowed = TRUE;
    viewportSwapChain.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ImGui_ImplDX11_SetSwapChainDescs(&viewportSwapChain, 1);

    AtlasApplication app;
    if (packedProjectToOpen)
        app.OpenPackedFromCommandLine(*packedProjectToOpen);
    else if (firestarProjectToOpen)
        app.OpenFirestarFromCommandLine(*firestarProjectToOpen);
    else if (projectToOpen)
        app.OpenProjectFromCommandLine(*projectToOpen);
    else if (repakToOpen)
        app.OpenRePakFromCommandLine(*repakToOpen);
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

        constexpr float clearColor[] = {0.035f, 0.038f, 0.044f, 1.0f};
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
