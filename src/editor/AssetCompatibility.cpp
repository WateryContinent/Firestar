#include "editor/AssetCompatibility.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <set>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace firestar::editor
{
    namespace
    {
        std::uint16_t ReadU16(const std::vector<std::uint8_t>& bytes, const size_t offset)
        {
            return static_cast<std::uint16_t>(bytes[offset]) |
                static_cast<std::uint16_t>(bytes[offset + 1] << 8);
        }

        std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes, const size_t offset)
        {
            return static_cast<std::uint32_t>(bytes[offset]) |
                (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
                (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
                (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
        }

        bool SupportsPakVersion(const std::string& type, const int pakVersion)
        {
            if (pakVersion == 8)
                return true;
            if (pakVersion != 7)
                return false;
            static const std::set<std::string> supported{
                "anir", "txtr", "uimg", "font", "rlcd", "matl", "shdr", "shds",
                "dtbl", "Ptch", "ui"
            };
            return supported.contains(type);
        }

        void Reject(AssetCompatibility& result, std::string message)
        {
            result.compatible = false;
            result.message = std::move(message);
        }
    }

    AssetCompatibility InspectAssetCompatibility(const fs::path& sourcePath,
        const std::string& assetType, const int pakVersion)
    {
        AssetCompatibility result;
        result.detectedVersion = "No embedded source version";
        result.message = "The source format has no version marker Firestar can validate before building.";

        std::error_code sizeError;
        const std::uintmax_t fileSize = fs::file_size(sourcePath, sizeError);
        if (sizeError)
        {
            Reject(result, "The source file cannot be opened.");
            return result;
        }

        std::ifstream input(sourcePath, std::ios::binary);
        std::vector<std::uint8_t> bytes(static_cast<size_t>((std::min<std::uintmax_t>)(fileSize, 256)));
        if (!bytes.empty())
            input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input && !input.eof())
        {
            Reject(result, "The source header could not be read.");
            return result;
        }

        if (assetType == "mdl_" || assetType == "arig")
        {
            if (bytes.size() < 84 || ReadU32(bytes, 0) != 0x54534449)
                Reject(result, "Expected an IDST studio model header.");
            else
            {
                const std::uint32_t version = ReadU32(bytes, 4);
                result.versionDetected = true;
                result.detectedVersion = "IDST version " + std::to_string(version);
                if (version != 54)
                    Reject(result, "Embedded RePak requires studio model version 54.");
                else
                {
                    const std::uint32_t declaredLength = ReadU32(bytes, 80);
                    if (declaredLength > fileSize)
                        Reject(result, "The studio header length is larger than the file.");
                    else
                        result.message = "Studio model version 54 is supported.";
                }
            }
        }
        else if (assetType == "txan" || assetType == "anir")
        {
            const std::uint32_t expectedMagic = assetType == "txan" ? 0x4E415854 : 0x52494E41;
            const char* const label = assetType == "txan" ? "TXAN" : "ANIR";
            const size_t minimumSize = assetType == "txan" ? 16 : 56;
            if (bytes.size() <= minimumSize || ReadU32(bytes, 0) != expectedMagic)
                Reject(result, std::string("Expected a valid ") + label + " header.");
            else
            {
                const std::uint16_t fileVersion = ReadU16(bytes, 4);
                const std::uint16_t assetVersion = ReadU16(bytes, 6);
                result.versionDetected = true;
                result.detectedVersion = std::string(label) + " file v" + std::to_string(fileVersion) +
                    ", asset v" + std::to_string(assetVersion);
                if (fileVersion != 1 || assetVersion != 1)
                    Reject(result, std::string("Embedded RePak supports ") + label + " file v1 / asset v1.");
                else
                    result.message = std::string(label) + " version 1 is supported.";
            }
        }
        else if (assetType == "ui")
        {
            if (bytes.size() < 8 || ReadU32(bytes, 0) != 0x50495552)
                Reject(result, "Expected a RUIP package header.");
            else
            {
                const std::uint16_t packageVersion = ReadU16(bytes, 4);
                const std::uint16_t ruiVersion = ReadU16(bytes, 6);
                result.versionDetected = true;
                result.detectedVersion = "RUIP v" + std::to_string(packageVersion) +
                    ", RUI v" + std::to_string(ruiVersion);
                if (packageVersion != 1 && packageVersion != 2)
                    Reject(result, "Embedded RePak supports RUIP package versions 1 and 2.");
                else if (ruiVersion != 30 && ruiVersion != 39 && ruiVersion != 40)
                    Reject(result, ruiVersion == 42
                        ? "RUI v42 needs an explicit $ruiVersion 39 conversion before it can be built."
                        : "Embedded RePak supports RUI versions 30, 39, and 40.");
                else
                    result.message = "The RUIP package and RUI versions are supported.";
            }
        }
        else if (assetType == "shdr" || assetType == "shds")
        {
            if (bytes.size() < 5 || bytes[0] != 'M' || bytes[1] != 'S' || bytes[2] != 'W')
                Reject(result, "Expected a MultiShaderWrapper (MSW) header.");
            else
            {
                const std::uint8_t wrapperVersion = bytes[3];
                const std::uint8_t wrapperType = bytes[4];
                const std::uint8_t expectedType = assetType == "shds" ? 1 : 0;
                result.versionDetected = true;
                result.detectedVersion = "MSW v" + std::to_string(wrapperVersion) +
                    (wrapperType == 1 ? ", shader set" : ", shader");
                if (wrapperVersion != 3)
                    Reject(result, "Embedded RePak supports MultiShaderWrapper version 3.");
                else if (wrapperType != expectedType)
                    Reject(result, "The wrapper kind does not match the selected RePak asset type.");
                else
                    result.message = "MultiShaderWrapper version 3 is supported.";
            }
        }
        else if (assetType == "txtr")
        {
            if (bytes.size() < 128 || ReadU32(bytes, 0) != 0x20534444 || ReadU32(bytes, 4) != 124)
                Reject(result, "Expected a complete DDS header.");
            else
            {
                result.detectedVersion = "DDS header v124";
                result.versionDetected = true;
                result.message = "The DDS container header is supported; texture format validation runs during build.";
            }
        }
        else if (assetType == "awsr")
        {
            const bool wav = bytes.size() >= 12 && ReadU32(bytes, 0) == 0x46464952 && ReadU32(bytes, 8) == 0x45564157;
            const bool ogg = bytes.size() >= 4 && ReadU32(bytes, 0) == 0x5367674F;
            if (!wav && !ogg)
                Reject(result, "Expected a RIFF/WAVE or OggS audio source.");
            else
            {
                result.detectedVersion = wav ? "RIFF/WAVE" : "Ogg bitstream";
                result.versionDetected = true;
                result.message = "The audio container is recognized; codec validation runs during build.";
            }
        }

        if (!SupportsPakVersion(assetType, pakVersion))
        {
            if (pakVersion != 7 && pakVersion != 8)
                Reject(result, "Firestar only supports RePak manifest versions 7 and 8.");
            else
                Reject(result, "Asset type " + assetType + " is not supported by RPAK " + std::to_string(pakVersion) + ".");
        }
        return result;
    }
}
