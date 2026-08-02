#include "editor/ProjectDocument.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <rapidjson/error/en.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

namespace fs = std::filesystem;

namespace firestar::editor
{
    namespace
    {
        std::string SanitizeName(std::string value)
        {
            for (char& character : value)
            {
                const unsigned char c = static_cast<unsigned char>(character);
                if (!std::isalnum(c) && character != '_' && character != '-')
                    character = '_';
            }
            value.erase(std::unique(value.begin(), value.end(), [](const char left, const char right) {
                return left == '_' && right == '_';
            }), value.end());
            while (!value.empty() && value.front() == '_') value.erase(value.begin());
            while (!value.empty() && value.back() == '_') value.pop_back();
            return value.empty() ? "new_rpak" : value;
        }

        std::string MemberString(const rapidjson::Value& object, const char* const name,
            const char* const fallback = "")
        {
            const auto iterator = object.FindMember(name);
            return iterator != object.MemberEnd() && iterator->value.IsString()
                ? std::string(iterator->value.GetString(), iterator->value.GetStringLength())
                : fallback;
        }

        fs::path ReplaceAssetExtension(fs::path value, const char* const extension)
        {
            value.replace_extension(extension);
            return value;
        }

        std::string GenericPath(const fs::path& path)
        {
            const auto text = path.generic_u8string();
            return {reinterpret_cast<const char*>(text.data()), text.size()};
        }

        fs::path PathFromUtf8(const std::string_view value)
        {
            const auto* begin = reinterpret_cast<const char8_t*>(value.data());
            return fs::path(std::u8string(begin, begin + value.size()));
        }

        std::string NormalizeRePakAssetPath(std::string value)
        {
            std::replace(value.begin(), value.end(), '\\', '/');
            while (value.find("//") != std::string::npos)
                value.replace(value.find("//"), 2, "/");
            while (!value.empty() && value.front() == '/') value.erase(value.begin());
            const size_t slash = value.find_last_of('/');
            const size_t dot = value.find_last_of('.');
            if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
                value.resize(dot);
            if (!value.empty()) value += ".rpak";
            return value;
        }

        bool NormalizeDocumentAssetPaths(rapidjson::Document& document)
        {
            if (!document.IsObject()) return false;
            const auto files = document.FindMember("files");
            if (files == document.MemberEnd() || !files->value.IsArray()) return false;
            bool changed{};
            auto& allocator = document.GetAllocator();
            for (rapidjson::Value& asset : files->value.GetArray())
            {
                if (!asset.IsObject()) continue;
                const auto path = asset.FindMember("_path");
                if (path == asset.MemberEnd() || !path->value.IsString()) continue;
                const std::string original(path->value.GetString(), path->value.GetStringLength());
                const std::string normalized = NormalizeRePakAssetPath(original);
                if (normalized.empty() || normalized == original) continue;
                path->value.SetString(normalized.c_str(), allocator);
                changed = true;
            }
            return changed;
        }

        bool ParseDocument(std::istream& input, rapidjson::Document& document,
            const fs::path& path, std::string& error)
        {
            rapidjson::IStreamWrapper stream(input);
            constexpr unsigned flags = rapidjson::kParseCommentsFlag |
                rapidjson::kParseTrailingCommasFlag |
                rapidjson::kParseIterativeFlag |
                rapidjson::kParseValidateEncodingFlag;
            document.ParseStream<flags>(stream);
            if (!document.HasParseError())
                return true;
            error = "JSON parse error in " + path.string() + " at byte " +
                std::to_string(document.GetErrorOffset()) + ": " +
                rapidjson::GetParseError_En(document.GetParseError());
            return false;
        }
    }

    bool ProjectDocument::Create(const fs::path& rootDirectory, std::string projectName, std::string& error)
    {
        projectName = SanitizeName(std::move(projectName));
        std::error_code ioError;
        fs::create_directories(rootDirectory / "assets", ioError);
        if (ioError)
        {
            error = "Unable to create the assets directory: " + ioError.message();
            return false;
        }
        fs::create_directories(rootDirectory / "build", ioError);
        if (ioError)
        {
            error = "Unable to create the build directory: " + ioError.message();
            return false;
        }
        document_.SetObject();
        auto& allocator = document_.GetAllocator();
        document_.AddMember("version", 8, allocator);
        document_.AddMember("name", rapidjson::Value(projectName.c_str(), allocator), allocator);
        document_.AddMember("assetsDir", "assets/", allocator);
        document_.AddMember("outputDir", "build/", allocator);
        document_.AddMember("keepDevOnly", true, allocator);
        document_.AddMember("keepServerOnly", true, allocator);
        document_.AddMember("keepClientOnly", true, allocator);
        document_.AddMember("files", rapidjson::Value(rapidjson::kArrayType), allocator);
        manifestPath_ = rootDirectory / (projectName + ".json");
        buildManifestPath_ = manifestPath_;
        buildBaseDirectory_ = rootDirectory;
        buildOutputDirectory_ = rootDirectory / "build";
        streamOutputPaths_.clear();
        pakManifestPaths_ = {manifestPath_};
        currentPakIndex_ = 0;
        inheritedPakVersion_ = 8;
        dirty_ = true;
        return true;
    }

    bool ProjectDocument::Load(const fs::path& manifestPath, std::string& error)
    {
        const fs::path absolutePath = fs::absolute(manifestPath).lexically_normal();
        std::ifstream input(absolutePath, std::ios::binary);
        if (!input)
        {
            error = "Unable to open " + absolutePath.string();
            return false;
        }
        rapidjson::Document loaded;
        if (!ParseDocument(input, loaded, absolutePath, error))
            return false;
        if (!loaded.IsObject())
        {
            error = "The RePak manifest root must be an object.";
            return false;
        }

        const auto paks = loaded.FindMember("paks");
        if (paks != loaded.MemberEnd())
        {
            const auto version = loaded.FindMember("version");
            if (version == loaded.MemberEnd() || !version->value.IsInt())
            {
                error = "The RePak build list needs an integer version field.";
                return false;
            }
            if (!paks->value.IsArray() || paks->value.Empty())
            {
                error = "The RePak build list paks field must contain at least one JSON path.";
                return false;
            }

            std::vector<fs::path> relativePaths;
            relativePaths.reserve(paks->value.Size());
            for (const rapidjson::Value& value : paks->value.GetArray())
            {
                if (!value.IsString() || value.GetStringLength() == 0)
                {
                    error = "Every RePak build-list entry must be a JSON path string.";
                    return false;
                }
                fs::path listed = PathFromUtf8(std::string_view(value.GetString(), value.GetStringLength()));
                relativePaths.push_back(std::move(listed));
            }

            buildManifestPath_ = absolutePath;
            buildBaseDirectory_ = absolutePath.parent_path();
            size_t bestMatchCount = 0;
            fs::path candidateBase = absolutePath.parent_path();
            for (size_t depth = 0; depth < 8 && !candidateBase.empty(); ++depth)
            {
                size_t matchCount = 0;
                for (const fs::path& listed : relativePaths)
                {
                    const fs::path candidate = listed.is_absolute() ? listed : candidateBase / listed;
                    if (fs::is_regular_file(candidate)) ++matchCount;
                }
                if (matchCount > bestMatchCount)
                {
                    bestMatchCount = matchCount;
                    buildBaseDirectory_ = candidateBase;
                }
                if (matchCount == relativePaths.size()) break;
                const fs::path parent = candidateBase.parent_path();
                if (parent == candidateBase) break;
                candidateBase = parent;
            }

            std::vector<fs::path> listedPaths;
            listedPaths.reserve(relativePaths.size());
            for (fs::path listed : relativePaths)
            {
                if (listed.is_relative()) listed = buildBaseDirectory_ / listed;
                listedPaths.push_back(fs::absolute(listed).lexically_normal());
            }
            fs::path output = PathFromUtf8(MemberString(loaded, "outputDir", "build/"));
            if (output.is_relative()) output = buildBaseDirectory_ / output;
            buildOutputDirectory_ = fs::absolute(output).lexically_normal();
            streamOutputPaths_.clear();
            for (const char* field : {"streamFileMandatory", "streamFileOptional"})
            {
                const std::string configured = MemberString(loaded, field);
                if (configured.empty()) continue;
                fs::path streamPath = PathFromUtf8(configured);
                if (streamPath.is_relative()) streamPath = buildBaseDirectory_ / streamPath;
                streamOutputPaths_.push_back(fs::absolute(streamPath).lexically_normal());
            }
            pakManifestPaths_ = std::move(listedPaths);
            currentPakIndex_ = 0;
            inheritedPakVersion_ = version->value.GetInt();
            if (!LoadActiveManifest(pakManifestPaths_.front(), true, error))
            {
                Close();
                return false;
            }
            return true;
        }

        buildManifestPath_ = absolutePath;
        buildBaseDirectory_ = absolutePath.parent_path();
        pakManifestPaths_ = {absolutePath};
        currentPakIndex_ = 0;
        inheritedPakVersion_ = 0;
        document_.Swap(loaded);
        const bool normalizedAssetPaths = NormalizeDocumentAssetPaths(document_);
        manifestPath_ = absolutePath;
        if (Validate(false, error))
        {
            const fs::path configuredAssets = PathFromUtf8(MemberString(document_, "assetsDir", "assets"));
            if (configuredAssets.is_relative() && !fs::is_directory(buildBaseDirectory_ / configuredAssets))
            {
                fs::path candidateBase = buildBaseDirectory_.parent_path();
                for (size_t depth = 0; depth < 7 && !candidateBase.empty(); ++depth)
                {
                    if (fs::is_directory(candidateBase / configuredAssets))
                    {
                        buildBaseDirectory_ = candidateBase;
                        break;
                    }
                    const fs::path parent = candidateBase.parent_path();
                    if (parent == candidateBase) break;
                    candidateBase = parent;
                }
            }
            inheritedPakVersion_ = PakVersion();
            buildOutputDirectory_ = OutputDirectory();
            streamOutputPaths_.clear();
            for (const char* field : {"streamFileMandatory", "streamFileOptional"})
            {
                const std::string configured = MemberString(document_, field);
                if (configured.empty()) continue;
                fs::path streamPath = PathFromUtf8(configured);
                if (streamPath.is_relative()) streamPath = buildBaseDirectory_ / streamPath;
                streamOutputPaths_.push_back(fs::absolute(streamPath).lexically_normal());
            }
            dirty_ = normalizedAssetPaths;
            return true;
        }
        Close();
        return false;
    }

    bool ProjectDocument::LoadSerialized(const std::string_view json,
        const fs::path& manifestPathHint, const fs::path& buildBaseDirectory, std::string& error)
    {
        rapidjson::Document loaded;
        constexpr unsigned flags = rapidjson::kParseCommentsFlag |
            rapidjson::kParseTrailingCommasFlag |
            rapidjson::kParseIterativeFlag |
            rapidjson::kParseValidateEncodingFlag;
        loaded.Parse<flags>(json.data(), json.size());
        if (loaded.HasParseError())
        {
            error = "Embedded JSON parse error at byte " +
                std::to_string(loaded.GetErrorOffset()) + ": " +
                rapidjson::GetParseError_En(loaded.GetParseError());
            return false;
        }
        if (!loaded.IsObject())
        {
            error = "The embedded RePak manifest root must be an object.";
            return false;
        }

        document_.Swap(loaded);
        const bool normalizedAssetPaths = NormalizeDocumentAssetPaths(document_);
        manifestPath_ = manifestPathHint.empty()
            ? fs::absolute(fs::path("firestar_project.json")).lexically_normal()
            : fs::absolute(manifestPathHint).lexically_normal();
        buildManifestPath_ = manifestPath_;
        buildBaseDirectory_ = buildBaseDirectory.empty()
            ? manifestPath_.parent_path()
            : fs::absolute(buildBaseDirectory).lexically_normal();
        pakManifestPaths_ = {manifestPath_};
        currentPakIndex_ = 0;
        inheritedPakVersion_ = 0;
        if (!Validate(false, error))
        {
            Close();
            return false;
        }
        inheritedPakVersion_ = PakVersion();
        buildOutputDirectory_ = OutputDirectory();
        streamOutputPaths_.clear();
        for (const char* field : {"streamFileMandatory", "streamFileOptional"})
        {
            const std::string configured = MemberString(document_, field);
            if (configured.empty()) continue;
            fs::path streamPath = PathFromUtf8(configured);
            if (streamPath.is_relative()) streamPath = buildBaseDirectory_ / streamPath;
            streamOutputPaths_.push_back(fs::absolute(streamPath).lexically_normal());
        }
        dirty_ = normalizedAssetPaths;
        return true;
    }

    bool ProjectDocument::LoadActiveManifest(const fs::path& manifestPath,
        const bool versionMayBeInherited, std::string& error)
    {
        std::ifstream input(manifestPath, std::ios::binary);
        if (!input)
        {
            error = "Unable to open listed RePak manifest " + manifestPath.string();
            return false;
        }
        rapidjson::Document loaded;
        if (!ParseDocument(input, loaded, manifestPath, error))
            return false;
        document_.Swap(loaded);
        const bool normalizedAssetPaths = NormalizeDocumentAssetPaths(document_);
        manifestPath_ = fs::absolute(manifestPath).lexically_normal();
        if (!Validate(versionMayBeInherited, error))
            return false;
        dirty_ = normalizedAssetPaths;
        return true;
    }

    bool ProjectDocument::SelectPak(const size_t index, std::string& error)
    {
        if (index >= pakManifestPaths_.size())
        {
            error = "The selected RePak build-list entry is out of range.";
            return false;
        }
        if (dirty_)
        {
            error = "Save the current Firestar project before switching RPAKs in this build list.";
            return false;
        }

        rapidjson::Document previousDocument;
        previousDocument.Swap(document_);
        const fs::path previousManifestPath = manifestPath_;
        const bool previousDirty = dirty_;
        if (!LoadActiveManifest(pakManifestPaths_[index], IsBuildList(), error))
        {
            document_.Swap(previousDocument);
            manifestPath_ = previousManifestPath;
            dirty_ = previousDirty;
            return false;
        }
        currentPakIndex_ = index;
        return true;
    }

    bool ProjectDocument::Save(std::string& error)
    {
        return SaveAs(manifestPath_, error);
    }

    bool ProjectDocument::SaveAs(const fs::path& manifestPath, std::string& error)
    {
        if (manifestPath.empty())
        {
            error = "No manifest path is set.";
            return false;
        }
        if (!Validate(IsBuildList(), error))
            return false;

        std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            error = "Unable to write " + manifestPath.string();
            return false;
        }
        rapidjson::OStreamWrapper stream(output);
        rapidjson::PrettyWriter<rapidjson::OStreamWrapper> writer(stream);
        writer.SetIndent(' ', 2);
        document_.Accept(writer);
        output << '\n';
        if (!output.good())
        {
            error = "The manifest write was incomplete.";
            return false;
        }
        manifestPath_ = fs::absolute(manifestPath).lexically_normal();
        dirty_ = false;
        return true;
    }

    bool ProjectDocument::ExportJson(const fs::path& manifestPath, std::string& error) const
    {
        if (manifestPath.empty())
        {
            error = "No JSON export path is set.";
            return false;
        }
        if (!document_.IsObject())
        {
            error = "No RePak project is open.";
            return false;
        }
        std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            error = "Unable to write " + manifestPath.string();
            return false;
        }
        const std::string json = SerializeDocument();
        output.write(json.data(), static_cast<std::streamsize>(json.size()));
        output.put('\n');
        if (!output.good())
        {
            error = "The JSON export was incomplete.";
            return false;
        }
        return true;
    }

    void ProjectDocument::Close()
    {
        document_.SetNull();
        manifestPath_.clear();
        buildManifestPath_.clear();
        buildBaseDirectory_.clear();
        buildOutputDirectory_.clear();
        streamOutputPaths_.clear();
        pakManifestPaths_.clear();
        currentPakIndex_ = 0;
        inheritedPakVersion_ = 0;
        dirty_ = false;
    }

    bool ProjectDocument::Validate(const bool versionMayBeInherited, std::string& error)
    {
        if (!document_.IsObject())
        {
            error = "The RePak manifest root must be an object.";
            return false;
        }
        const auto version = document_.FindMember("version");
        if (version == document_.MemberEnd() && !versionMayBeInherited)
        {
            error = "The manifest needs an integer version field.";
            return false;
        }
        if (version != document_.MemberEnd() && !version->value.IsInt())
        {
            error = "The manifest version field must be an integer.";
            return false;
        }
        auto files = document_.FindMember("files");
        if (files == document_.MemberEnd())
            document_.AddMember("files", rapidjson::Value(rapidjson::kArrayType), document_.GetAllocator());
        else if (!files->value.IsArray())
        {
            error = "The manifest files field must be an array.";
            return false;
        }
        return true;
    }

    fs::path ProjectDocument::RootDirectory() const
    {
        // RePak resolves child-map assetsDir/outputDir members from the staged
        // build-list path, not from each child JSON's own folder.
        return !buildBaseDirectory_.empty() ? buildBaseDirectory_ : manifestPath_.parent_path();
    }

    fs::path ProjectDocument::ResolveManifestPath(const char* const member, const fs::path& fallback) const
    {
        if (!document_.IsObject())
            return {};
        const std::string configured = MemberString(document_, member);
        fs::path result = configured.empty() ? fallback : PathFromUtf8(configured);
        if (result.is_relative())
            result = RootDirectory() / result;
        return result.lexically_normal();
    }

    fs::path ProjectDocument::AssetsDirectory() const
    {
        return ResolveManifestPath("assetsDir", "assets");
    }

    fs::path ProjectDocument::OutputDirectory() const
    {
        return ResolveManifestPath("outputDir", "build");
    }

    void ProjectDocument::RefreshDerivedPaths()
    {
        buildOutputDirectory_ = OutputDirectory();
        streamOutputPaths_.clear();
        for (const char* field : {"streamFileMandatory", "streamFileOptional"})
        {
            const std::string configured = MemberString(document_, field);
            if (configured.empty()) continue;
            fs::path streamPath = PathFromUtf8(configured);
            if (streamPath.is_relative()) streamPath = buildBaseDirectory_ / streamPath;
            streamOutputPaths_.push_back(fs::absolute(streamPath).lexically_normal());
        }
    }

    std::string ProjectDocument::Name() const
    {
        return document_.IsObject() ? MemberString(document_, "name", manifestPath_.stem().string().c_str()) : std::string{};
    }

    int ProjectDocument::PakVersion() const
    {
        if (!document_.IsObject()) return 0;
        const auto version = document_.FindMember("version");
        return version != document_.MemberEnd() && version->value.IsInt()
            ? version->value.GetInt() : inheritedPakVersion_;
    }

    size_t ProjectDocument::AssetCount() const
    {
        if (!document_.IsObject()) return 0;
        const auto files = document_.FindMember("files");
        return files != document_.MemberEnd() && files->value.IsArray() ? files->value.Size() : 0;
    }

    rapidjson::Value* ProjectDocument::Asset(const size_t index)
    {
        if (index >= AssetCount()) return nullptr;
        return &document_["files"][static_cast<rapidjson::SizeType>(index)];
    }

    const rapidjson::Value* ProjectDocument::Asset(const size_t index) const
    {
        if (index >= AssetCount()) return nullptr;
        return &document_["files"][static_cast<rapidjson::SizeType>(index)];
    }

    size_t ProjectDocument::AddAsset(std::string type, std::string path)
    {
        path = NormalizeRePakAssetPath(std::move(path));
        auto& allocator = document_.GetAllocator();
        rapidjson::Value asset(rapidjson::kObjectType);
        asset.AddMember("_type", rapidjson::Value(type.c_str(), allocator), allocator);
        asset.AddMember("_path", rapidjson::Value(path.c_str(), allocator), allocator);
        document_["files"].PushBack(asset, allocator);
        dirty_ = true;
        return AssetCount() - 1;
    }

    bool ProjectDocument::RemoveAsset(const size_t index)
    {
        if (index >= AssetCount()) return false;
        auto& files = document_["files"];
        files.Erase(files.Begin() + static_cast<rapidjson::SizeType>(index));
        dirty_ = true;
        return true;
    }

    std::optional<fs::path> ProjectDocument::ResolvePrimarySource(const size_t assetIndex) const
    {
        const rapidjson::Value* asset = Asset(assetIndex);
        if (!asset || !asset->IsObject()) return std::nullopt;
        const std::string type = MemberString(*asset, "_type");
        const std::string pathText = MemberString(*asset, "_path");
        if (pathText.empty() || type == "aevt" || type == "uimg") return std::nullopt;

        fs::path source = AssetsDirectory() / PathFromUtf8(pathText);
        std::vector<fs::path> candidates;
        if (type == "txtr")
            candidates = {ReplaceAssetExtension(source, ".dds"), ReplaceAssetExtension(source, ".json")};
        else if (type == "matl")
            candidates = {ReplaceAssetExtension(source, ".json"), ReplaceAssetExtension(source, ".uber")};
        else if (type == "mdl_")
            candidates = {ReplaceAssetExtension(source, ".rmdl")};
        else if (type == "arig")
            candidates = {ReplaceAssetExtension(source, ".rrig")};
        else if (type == "aseq")
            candidates = {ReplaceAssetExtension(source, ".rseq")};
        else if (type == "anir")
            candidates = {ReplaceAssetExtension(source, ".anir")};
        else if (type == "txan")
            candidates = {ReplaceAssetExtension(source, ".txan")};
        else if (type == "ui")
            candidates = {ReplaceAssetExtension(source, ".ruip")};
        else if (type == "dtbl")
            candidates = {ReplaceAssetExtension(source, ".csv")};
        else if (type == "font")
            candidates = {ReplaceAssetExtension(source, ".bin")};
        else if (type == "awsr" || type == "asrc")
        {
            const std::string wav = MemberString(*asset, "wav");
            const std::string ogg = MemberString(*asset, "ogg");
            const std::string file = MemberString(*asset, "file");
            if (!wav.empty()) candidates.push_back((AssetsDirectory() / PathFromUtf8(wav)).lexically_normal());
            if (!ogg.empty()) candidates.push_back((AssetsDirectory() / PathFromUtf8(ogg)).lexically_normal());
            if (!file.empty()) candidates.push_back((AssetsDirectory() / PathFromUtf8(file)).lexically_normal());
            if (candidates.empty())
            {
                candidates.push_back(ReplaceAssetExtension(source, ".wav"));
                candidates.push_back(ReplaceAssetExtension(source, ".ogg"));
                candidates.push_back(ReplaceAssetExtension(source, ".oga"));
            }
        }
        else if (type == "shdr" || type == "shds")
            candidates = {ReplaceAssetExtension(source, ".msw"), source};
        else
            candidates = {source, ReplaceAssetExtension(source, ".json")};

        for (const auto& candidate : candidates)
            if (fs::is_regular_file(candidate)) return candidate.lexically_normal();
        return candidates.empty() ? std::nullopt : std::optional<fs::path>(candidates.front().lexically_normal());
    }

    std::vector<fs::path> ProjectDocument::FindNearbyAssets(const size_t assetIndex,
        const std::vector<std::string>& extensions) const
    {
        std::vector<fs::path> results;
        fs::path searchRoot = AssetsDirectory();
        if (const auto primary = ResolvePrimarySource(assetIndex); primary && fs::exists(primary->parent_path()))
            searchRoot = primary->parent_path();
        std::error_code error;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        size_t visited{};
        for (fs::recursive_directory_iterator iterator(searchRoot, fs::directory_options::skip_permission_denied, error), end;
             iterator != end && !error; iterator.increment(error))
        {
            if (++visited >= 20000 || (visited % 128 == 0 && std::chrono::steady_clock::now() >= deadline))
                break;
            if (!iterator->is_regular_file(error)) continue;
            std::string extension = iterator->path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (std::find(extensions.begin(), extensions.end(), extension) != extensions.end())
                results.push_back(iterator->path());
            if (results.size() >= 512) break;
        }
        std::sort(results.begin(), results.end());
        return results;
    }

    bool ProjectDocument::AssetArrayContains(const size_t assetIndex, const char* const field,
        const std::string& value) const
    {
        const rapidjson::Value* asset = Asset(assetIndex);
        if (!asset || !asset->IsObject()) return false;
        const auto member = asset->FindMember(field);
        if (member == asset->MemberEnd() || !member->value.IsArray()) return false;
        for (const auto& existing : member->value.GetArray())
            if (existing.IsString() && value == existing.GetString()) return true;
        return false;
    }

    bool ProjectDocument::AppendStringToAssetArray(const size_t assetIndex, const char* const field,
        const std::string& value)
    {
        rapidjson::Value* asset = Asset(assetIndex);
        if (!asset || !asset->IsObject()) return false;
        auto member = asset->FindMember(field);
        if (member == asset->MemberEnd())
        {
            rapidjson::Value name(field, document_.GetAllocator());
            rapidjson::Value array(rapidjson::kArrayType);
            asset->AddMember(name, array, document_.GetAllocator());
            member = asset->FindMember(field);
        }
        if (!member->value.IsArray()) return false;
        for (const auto& existing : member->value.GetArray())
            if (existing.IsString() && value == existing.GetString()) return true;
        member->value.PushBack(rapidjson::Value(value.c_str(), document_.GetAllocator()), document_.GetAllocator());
        dirty_ = true;
        return true;
    }

    bool ProjectDocument::SetStringInAssetArray(const size_t assetIndex, const char* const field,
        const std::string& value, const bool included)
    {
        if (included) return AppendStringToAssetArray(assetIndex, field, value);
        rapidjson::Value* asset = Asset(assetIndex);
        if (!asset || !asset->IsObject()) return false;
        auto member = asset->FindMember(field);
        if (member == asset->MemberEnd()) return true;
        if (!member->value.IsArray()) return false;
        for (auto existing = member->value.Begin(); existing != member->value.End(); ++existing)
        {
            if (!existing->IsString() || value != existing->GetString()) continue;
            member->value.Erase(existing);
            if (member->value.Empty()) asset->RemoveMember(field);
            dirty_ = true;
            return true;
        }
        return true;
    }

    std::string ProjectDocument::SerializeAsset(const size_t assetIndex) const
    {
        const rapidjson::Value* asset = Asset(assetIndex);
        if (!asset) return {};
        rapidjson::StringBuffer buffer;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
        writer.SetIndent(' ', 2);
        asset->Accept(writer);
        return {buffer.GetString(), buffer.GetSize()};
    }

    std::string ProjectDocument::SerializeDocument() const
    {
        if (!document_.IsObject()) return {};
        rapidjson::StringBuffer buffer;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
        writer.SetIndent(' ', 2);
        document_.Accept(writer);
        return {buffer.GetString(), buffer.GetSize()};
    }
}
