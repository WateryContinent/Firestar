#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace firestar::editor
{
    struct PackedProjectEntry
    {
        std::filesystem::path relativePath;
        std::vector<std::uint8_t> bytes;
    };

    [[nodiscard]] bool WritePackedProject(const std::filesystem::path& destination,
        const std::vector<PackedProjectEntry>& entries, std::string& error);

    [[nodiscard]] bool ReadPackedProject(const std::filesystem::path& source,
        std::vector<PackedProjectEntry>& entries, std::string& error);

    [[nodiscard]] bool ExtractPackedProject(const std::filesystem::path& source,
        const std::filesystem::path& destinationRoot, std::filesystem::path& extractedProject,
        std::string& error);
}
