#include "editor/ProjectDocument.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using firestar::editor::ProjectDocument;

namespace
{
    void Require(const bool condition, const std::string& message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    struct TemporaryDirectory
    {
        fs::path path;

        TemporaryDirectory()
        {
            const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() /
                ("firestar-project-document-tests-" + std::to_string(timestamp));
            Require(fs::create_directory(path), "Unable to create test directory");
        }

        ~TemporaryDirectory()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
    };

    void WriteJson(const fs::path& path, const std::string_view json)
    {
        std::ofstream output(path, std::ios::binary);
        output.write(json.data(), static_cast<std::streamsize>(json.size()));
        output.close();
        Require(static_cast<bool>(output), "Unable to write test fixture");
    }

    struct Snapshot
    {
        std::string json;
        bool open;
        bool dirty;
        bool buildList;
        fs::path manifest;
        fs::path buildManifest;
        fs::path buildBase;
        fs::path buildOutput;
        std::vector<fs::path> streams;
        std::vector<fs::path> paks;
        size_t currentPak;
        int version;

        explicit Snapshot(const ProjectDocument& project)
            : json(project.SerializeDocument()), open(project.IsOpen()), dirty(project.IsDirty()),
              buildList(project.IsBuildList()), manifest(project.ManifestPath()),
              buildManifest(project.BuildManifestPath()), buildBase(project.BuildBaseDirectory()),
              buildOutput(project.BuildOutputDirectory()), streams(project.StreamOutputPaths()),
              paks(project.PakManifestPaths()), currentPak(project.CurrentPakIndex()),
              version(project.PakVersion())
        {
        }

        void Check(const ProjectDocument& project, const std::string& context) const
        {
            const Snapshot actual(project);
            Require(json == actual.json && open == actual.open && dirty == actual.dirty &&
                buildList == actual.buildList && manifest == actual.manifest &&
                buildManifest == actual.buildManifest && buildBase == actual.buildBase &&
                buildOutput == actual.buildOutput && streams == actual.streams &&
                paks == actual.paks && currentPak == actual.currentPak && version == actual.version,
                context + ": failed load changed the current project");
        }
    };

    void CheckRejectedLoads(ProjectDocument& project, const fs::path& root)
    {
        const Snapshot before(project);
        const std::string_view invalidManifests[]{
            "{",
            "[]",
            R"({"files":[]})",
            R"({"version":"8","files":[]})",
            R"({"version":8,"files":false})"
        };
        std::string error;
        for (size_t index = 0; index < std::size(invalidManifests); ++index)
        {
            const fs::path path = root / ("invalid-" + std::to_string(index) + ".json");
            WriteJson(path, invalidManifests[index]);
            error.clear();
            Require(!project.Load(path, error) && !error.empty(), "Invalid JSON import was accepted");
            before.Check(project, "JSON import " + std::to_string(index));
            error.clear();
            Require(!project.LoadSerialized(invalidManifests[index], path, root / "other-base", error) &&
                !error.empty(), "Invalid embedded manifest was accepted");
            before.Check(project, "Embedded manifest " + std::to_string(index));
        }

        error.clear();
        Require(!project.Load(root / "missing.json", error) && !error.empty(), "Missing file was accepted");
        before.Check(project, "Missing file");

        WriteJson(root / "invalid-child.json", R"({"files":false})");
        const std::string_view invalidBuildLists[]{
            R"({"version":8,"paks":[]})",
            R"({"version":8,"paks":[42]})",
            R"({"version":8,"paks":["missing-child.json"]})",
            R"({"version":8,"paks":["invalid-child.json"],"outputDir":"other-build/"})"
        };
        for (size_t index = 0; index < std::size(invalidBuildLists); ++index)
        {
            const fs::path path = root / ("invalid-build-list-" + std::to_string(index) + ".json");
            WriteJson(path, invalidBuildLists[index]);
            error.clear();
            Require(!project.Load(path, error) && !error.empty(), "Invalid build list was accepted");
            before.Check(project, "Build list " + std::to_string(index));
        }
    }

    void RunTests(const fs::path& root)
    {
        ProjectDocument project;
        std::string error;
        CheckRejectedLoads(project, root);

        Require(project.Create(root / "original", "original", error), "Cannot create original project: " + error);
        (void)project.AddAsset("txtr", "textures/unsaved.dds");
        CheckRejectedLoads(project, root);
        Require(project.Save(error), "Preserved unsaved project cannot be saved: " + error);
        CheckRejectedLoads(project, root);

        WriteJson(root / "first.json", R"({"name":"first","files":[]})");
        WriteJson(root / "second.json", R"({"name":"second","files":[]})");
        WriteJson(root / "build-list.json", R"({
            "version":7,"paks":["first.json","second.json"],"outputDir":"custom-build/",
            "streamFileMandatory":"streams/required.starpak","streamFileOptional":"streams/optional.starpak"
        })");
        Require(project.Load(root / "build-list.json", error), "Valid build list failed: " + error);
        Require(project.IsBuildList() && project.PakVersion() == 7 && !project.IsDirty() &&
            project.StreamOutputPaths().size() == 2, "Valid build-list state was lost");
        Require(project.SelectPak(1, error), "Unable to select second pak: " + error);
        (void)project.AddAsset("txtr", "textures/unsaved.dds");
        CheckRejectedLoads(project, root);

        const auto replacementPath = root / "replacement.json";
        constexpr std::string_view replacement = R"({"version":8,"name":"replacement","files":[]})";
        WriteJson(replacementPath, replacement);
        Require(project.Load(replacementPath, error), "Valid replacement failed: " + error);
        Require(project.Name() == "replacement" && !project.IsBuildList() && !project.IsDirty() &&
            project.AssetCount() == 0 && project.CurrentPakIndex() == 0 && project.PakVersion() == 8 &&
            project.PakManifestPaths() == std::vector<fs::path>{replacementPath} &&
            project.StreamOutputPaths().empty(), "Valid replacement retained old build-list state");

        constexpr std::string_view legacy = R"({"version":8,"files":[
            {"_type":"arig","_path":"models/rig.rrig"},
            {"_type":"txtr","_path":"textures/source.dds"}
        ]})";
        const auto legacyPath = root / "legacy.json";
        WriteJson(legacyPath, legacy);
        Require(project.Load(legacyPath, error), "Legacy JSON import failed: " + error);
        Require(project.IsDirty() && !project.HasLeadingAnimationAsset() &&
            project.SerializeDocument().find("textures/source.rpak") != std::string::npos,
            "Successful JSON import lost normalization");

        Require(project.LoadSerialized(legacy, root / "embedded.json", root / "embedded-base", error),
            "Valid embedded replacement failed: " + error);
        Require(project.IsDirty() && !project.HasLeadingAnimationAsset() && project.AssetCount() == 2 &&
            project.ManifestPath() == root / "embedded.json" &&
            project.BuildBaseDirectory() == root / "embedded-base" && !project.IsBuildList(),
            "Successful embedded replacement lost state or normalization");
        Require(project.LoadSerialized(replacement, {}, {}, error), "Default embedded paths failed: " + error);
        Require(project.Name() == "replacement" && !project.IsDirty() &&
            project.BuildBaseDirectory() == project.ManifestPath().parent_path(),
            "Default embedded replacement retained old state");
    }
}

int main()
{
    try
    {
        const TemporaryDirectory temporary;
        RunTests(temporary.path);
        std::cout << "ProjectDocument tests passed (60 rejected loads plus successful replacements).\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
