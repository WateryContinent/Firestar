#pragma once

#include <filesystem>
#include <string>

namespace firestar::editor
{
    struct AssetCompatibility
    {
        bool compatible{true};
        bool versionDetected{};
        std::string detectedVersion;
        std::string message;
    };

    [[nodiscard]] AssetCompatibility InspectAssetCompatibility(
        const std::filesystem::path& sourcePath, const std::string& assetType, int pakVersion);
}
