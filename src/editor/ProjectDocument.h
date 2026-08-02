#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#define RAPIDJSON_NO_SIZETYPEDEFINE
namespace rapidjson { typedef ::std::size_t SizeType; }
#include <rapidjson/document.h>

namespace firestar::editor
{
    class ProjectDocument
    {
    public:
        [[nodiscard]] bool Create(const std::filesystem::path& rootDirectory,
            std::string projectName, std::string& error);
        [[nodiscard]] bool Load(const std::filesystem::path& manifestPath, std::string& error);
        [[nodiscard]] bool LoadSerialized(std::string_view json,
            const std::filesystem::path& manifestPathHint,
            const std::filesystem::path& buildBaseDirectory,
            std::string& error);
        [[nodiscard]] bool Save(std::string& error);
        [[nodiscard]] bool SaveAs(const std::filesystem::path& manifestPath, std::string& error);
        [[nodiscard]] bool ExportJson(const std::filesystem::path& manifestPath, std::string& error) const;
        void Close();

        [[nodiscard]] bool IsOpen() const { return document_.IsObject() && !manifestPath_.empty(); }
        [[nodiscard]] bool IsDirty() const { return dirty_; }
        void MarkDirty() { dirty_ = true; }
        void MarkClean() { dirty_ = false; }

        [[nodiscard]] const std::filesystem::path& ManifestPath() const { return manifestPath_; }
        [[nodiscard]] const std::filesystem::path& BuildManifestPath() const { return buildManifestPath_; }
        [[nodiscard]] const std::filesystem::path& BuildBaseDirectory() const { return buildBaseDirectory_; }
        [[nodiscard]] bool IsBuildList() const { return pakManifestPaths_.size() > 1 || buildManifestPath_ != manifestPath_; }
        [[nodiscard]] const std::vector<std::filesystem::path>& PakManifestPaths() const { return pakManifestPaths_; }
        [[nodiscard]] size_t CurrentPakIndex() const { return currentPakIndex_; }
        [[nodiscard]] bool SelectPak(size_t index, std::string& error);
        [[nodiscard]] std::filesystem::path RootDirectory() const;
        [[nodiscard]] std::filesystem::path AssetsDirectory() const;
        [[nodiscard]] std::filesystem::path OutputDirectory() const;
        void RefreshDerivedPaths();
        [[nodiscard]] const std::filesystem::path& BuildOutputDirectory() const { return buildOutputDirectory_; }
        [[nodiscard]] const std::vector<std::filesystem::path>& StreamOutputPaths() const { return streamOutputPaths_; }
        [[nodiscard]] std::string Name() const;
        [[nodiscard]] int PakVersion() const;

        [[nodiscard]] size_t AssetCount() const;
        [[nodiscard]] rapidjson::Value* Asset(size_t index);
        [[nodiscard]] const rapidjson::Value* Asset(size_t index) const;
        [[nodiscard]] size_t AddAsset(std::string type, std::string path);
        [[nodiscard]] bool RemoveAsset(size_t index);

        [[nodiscard]] rapidjson::Document& Document() { return document_; }
        [[nodiscard]] const rapidjson::Document& Document() const { return document_; }
        [[nodiscard]] rapidjson::Document::AllocatorType& Allocator() { return document_.GetAllocator(); }

        [[nodiscard]] std::optional<std::filesystem::path> ResolvePrimarySource(size_t assetIndex) const;
        [[nodiscard]] std::vector<std::filesystem::path> FindNearbyAssets(size_t assetIndex,
            const std::vector<std::string>& extensions) const;
        [[nodiscard]] bool AssetArrayContains(size_t assetIndex, const char* field,
            const std::string& value) const;
        [[nodiscard]] bool AppendStringToAssetArray(size_t assetIndex, const char* field,
            const std::string& value);
        [[nodiscard]] bool SetStringInAssetArray(size_t assetIndex, const char* field,
            const std::string& value, bool included);

        [[nodiscard]] std::string SerializeAsset(size_t assetIndex) const;
        [[nodiscard]] std::string SerializeDocument() const;

    private:
        [[nodiscard]] bool LoadActiveManifest(const std::filesystem::path& manifestPath,
            bool versionMayBeInherited, std::string& error);
        [[nodiscard]] bool Validate(bool versionMayBeInherited, std::string& error);
        [[nodiscard]] std::filesystem::path ResolveManifestPath(const char* member,
            const std::filesystem::path& fallback) const;

        rapidjson::Document document_;
        std::filesystem::path manifestPath_;
        std::filesystem::path buildManifestPath_;
        std::filesystem::path buildBaseDirectory_;
        std::filesystem::path buildOutputDirectory_;
        std::vector<std::filesystem::path> streamOutputPaths_;
        std::vector<std::filesystem::path> pakManifestPaths_;
        size_t currentPakIndex_ = 0;
        int inheritedPakVersion_ = 0;
        bool dirty_ = false;
    };
}
