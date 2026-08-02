#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace firestar::editor
{
    struct PreviewVertex
    {
        float x{};
        float y{};
        float z{};
        float u{};
        float v{};
    };

    struct PreviewMesh
    {
        std::uint32_t firstIndex{};
        std::uint32_t indexCount{};
        std::uint64_t materialGuid{};
        std::uint32_t sourceMeshIndex{};
        int bodyPartIndex{-1};
        int modelIndex{-1};
    };

    struct PreviewBodyModel
    {
        std::string name;
    };

    struct PreviewBodyPart
    {
        std::string name;
        std::vector<PreviewBodyModel> models;
    };

    struct RmdlPreviewData
    {
        std::vector<PreviewVertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<PreviewMesh> meshes;
        std::vector<PreviewBodyPart> bodyParts;
        std::vector<int> lodLevels;
        PreviewVertex minimum{};
        PreviewVertex maximum{};
        size_t selectedLod{};
        std::string format;

        void Clear();
        [[nodiscard]] bool Empty() const { return vertices.empty() || indices.empty(); }
    };

    // Uses the Respawn VG/RMDL layouts and packed Vector64 position decoding
    // used by the local RSX model preview implementation.
    [[nodiscard]] bool LoadRmdlPreview(const std::filesystem::path& rmdlPath,
        size_t lodSelection, RmdlPreviewData& preview, std::string& error);
}
