#include "pch.h"
#include <windows.h>
#include "assets/assets.h"
#include "logic/buildsettings.h"
#include "logic/pakfile.h"
#include "logic/streamfile.h"
#include "logic/streamcache.h"
#include "utils/zstdutils.h"
#include "application/embedded.h"

#define REPAK_DEFAULT_COMPRESS_LEVEL 6
#define REPAK_DEFAULT_COMPRESS_WORKERS 16
#define REPAK_MAX_COMPRESS_WORKERS 256

#define REPAK_STR_TO_GUID_COMMAND "-pakguid"
#define REPAK_STR_TO_UIMG_HASH_COMMAND "-uimghash"
#define REPAK_COMPRESS_PAK_COMMAND "-compress"
#define REPAK_DECOMPRESS_PAK_COMMAND "-decompress"
#define REPAK_PCF_TO_EFCT_COMMAND "-pcf"
#define REPAK_VERBOSE_COMMAND "-v"
#define REPAK_VERBOSE_LONG_COMMAND "--verbose"

static void RePak_InitBuilder(const js::Document& doc, const char* const mapPath, CBuildSettings& settings, CStreamFileBuilder& streamBuilder)
{
    settings.Init(doc, mapPath);

    const bool keepClient = settings.IsFlagSet(PF_KEEP_CLIENT);

    // Server-only paks never uses streaming assets.
    if (keepClient)
        streamBuilder.Init(doc, settings.GetPakVersion() >= 8);
}

static void RePak_ShutdownBuilder(CBuildSettings& settings, CStreamFileBuilder& streamBuilder)
{
    const bool keepClient = settings.IsFlagSet(PF_KEEP_CLIENT);

    if (keepClient)
        streamBuilder.Shutdown();
}

static void RePak_ParseListedDocument(js::Document& doc, const char* const docPath, const char* const docName)
{
    Log("*** parsing listed build map \"%s\".\n", docName);
    std::string finalName = docName;

    Utils::ResolvePath(finalName, docPath);
    JSON_ParseFromFile(finalName.c_str(), "listed build map", doc, true);
}

static void RePak_BuildSingle(const js::Document& doc, const char* const mapPath)
{
    CBuildSettings settings;
    CStreamFileBuilder streamBuilder(&settings);

    RePak_InitBuilder(doc, mapPath, settings, streamBuilder);

    CPakFileBuilder pakFile(&settings, &streamBuilder);
    pakFile.BuildFromMap(doc);

    RePak_ShutdownBuilder(settings, streamBuilder);
}

static void RePak_BuildFromList(const js::Document& doc, const js::Value& list, const char* const mapPath)
{
    if (!list.IsArray())
    {
        Error("Pak build list is of type %s, but code expects %s.\n",
            JSON_TypeToString(JSON_ExtractType(list)), JSON_TypeToString(JSONFieldType_e::kArray));
    }

    CBuildSettings settings;
    CStreamFileBuilder streamBuilder(&settings);

    RePak_InitBuilder(doc, mapPath, settings, streamBuilder);

    ssize_t i = -1;

    for (const js::Value& pak : list.GetArray())
    {
        i++;

        if (!pak.IsString())
        {
            Error("Pak #%zd in build list is of type %s, but code expects %s.\n",
                i, JSON_TypeToString(JSON_ExtractType(pak)), JSON_TypeToString(JSONFieldType_e::kString));
        }

        js::Document pakDoc;
        RePak_ParseListedDocument(pakDoc, settings.GetBuildMapPath(), pak.GetString());

        CPakFileBuilder pakFile(&settings, &streamBuilder);
        pakFile.BuildFromMap(pakDoc);
    }

    RePak_ShutdownBuilder(settings, streamBuilder);
}

static void RePak_HandleBuildFromPath(const char* const inputPath, const char* const manifestBaseDirectory = nullptr)
{
    fs::path starmapPath(inputPath);

    // If the path is a directory, we generate a StarMap manifest of all starpaks in the directory
    if (std::filesystem::is_directory(starmapPath))
    {
        starmapPath.append("pc_roots.starmap");
        const std::string starmapStreamStr = starmapPath.string();

        CStreamCache writeCache;
        writeCache.BuildStarMapFromPaksDirectory(starmapStreamStr.c_str());
    }
    else
    {
        // load and parse map file, this file is essentially the
        // control file; deciding what is getting packed, etc..
        js::Document doc;
        JSON_ParseFromFile(inputPath, "main build map", doc, true);

        fs::path effectiveMapPath(inputPath);
        if (manifestBaseDirectory && manifestBaseDirectory[0])
            effectiveMapPath = fs::path(manifestBaseDirectory) / effectiveMapPath.filename();
        const std::string effectiveMapPathText = effectiveMapPath.string();

        js::Value::ConstMemberIterator paksIt;

        if (JSON_GetIterator(doc, "paks", paksIt))
            RePak_BuildFromList(doc, paksIt->value, effectiveMapPathText.c_str());
        else
            RePak_BuildSingle(doc, effectiveMapPathText.c_str());
    }
}

static std::string RePak_QuoteProcessArgument(const std::string& argument)
{
    if (argument.empty())
        return "\"\"";

    if (argument.find_first_of(" \t\n\v\"") == std::string::npos)
        return argument;

    std::string quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back('"');

    size_t backslashCount = 0;
    for (const char character : argument)
    {
        if (character == '\\')
        {
            backslashCount++;
            continue;
        }

        if (character == '"')
        {
            quoted.append(backslashCount * 2 + 1, '\\');
            quoted.push_back('"');
            backslashCount = 0;
            continue;
        }

        quoted.append(backslashCount, '\\');
        backslashCount = 0;
        quoted.push_back(character);
    }

    quoted.append(backslashCount * 2, '\\');
    quoted.push_back('"');
    return quoted;
}

static fs::path RePak_FindPcfCompiler()
{
    char* configuredPath = nullptr;
    size_t configuredLength = 0;
    if (_dupenv_s(&configuredPath, &configuredLength, "REPAK_PCF_COMPILER") == 0
        && configuredPath)
    {
        const fs::path candidate(configuredPath);
        free(configuredPath);

        if (fs::is_regular_file(candidate))
            return fs::absolute(candidate);

        Error(
            "REPAK_PCF_COMPILER points to a missing file: \"%s\".\n",
            candidate.string().c_str());
    }

    std::vector<fs::path> searchRoots;
    searchRoots.push_back(fs::current_path());

    std::array<char, 32768> executablePath{};
    const DWORD pathLength = GetModuleFileNameA(
        nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    if (pathLength > 0 && pathLength < executablePath.size())
        searchRoots.push_back(fs::path(executablePath.data()).parent_path());

    for (fs::path root : searchRoots)
    {
        for (size_t depth = 0; depth < 10 && !root.empty(); depth++)
        {
            const fs::path candidate = root / "efct_work" / "pcf_to_efct.py";
            if (fs::is_regular_file(candidate))
                return fs::absolute(candidate);

            const fs::path parent = root.parent_path();
            if (parent == root)
                break;

            root = parent;
        }
    }

    Error(
        "Could not find efct_work\\pcf_to_efct.py. Run RePak from the Apex "
        "Recharged workspace or set REPAK_PCF_COMPILER to its full path.\n");
    return {};
}

static void RePak_HandlePcfToEfct(
    const char* const pcfPath,
    const char* const referencePath,
    const char* const outputPath,
    const char* const projectPath)
{
    const fs::path compilerPath = RePak_FindPcfCompiler();
    fs::path finalProjectPath;

    if (projectPath && projectPath[0])
    {
        finalProjectPath = projectPath;
    }
    else
    {
        finalProjectPath = pcfPath;
        finalProjectPath.replace_extension(".efct.json");
    }

    std::vector<std::string> arguments{
        "py.exe",
        compilerPath.string(),
        "compile",
        "--pcf",
        fs::absolute(pcfPath).string(),
        "--reference",
        fs::absolute(referencePath).string(),
        "--project",
        fs::absolute(finalProjectPath).string(),
        "--output",
        fs::absolute(outputPath).string(),
    };

    std::string commandLine;
    for (const std::string& argument : arguments)
    {
        if (!commandLine.empty())
            commandLine.push_back(' ');
        commandLine += RePak_QuoteProcessArgument(argument);
    }

    std::vector<char> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back('\0');

    STARTUPINFOA startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};

    Log("*** compiling PCF \"%s\" through project \"%s\".\n",
        pcfPath, finalProjectPath.string().c_str());

    if (!CreateProcessA(
            nullptr,
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            0,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo))
    {
        Error(
            "Failed to start the PCF compiler with Windows error %lu. "
            "Make sure the Python launcher (py.exe) is installed.\n",
            GetLastError());
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);

    DWORD exitCode = EXIT_FAILURE;
    if (!GetExitCodeProcess(processInfo.hProcess, &exitCode))
    {
        const DWORD errorCode = GetLastError();
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        Error("Failed to read the PCF compiler exit code (Windows error %lu).\n", errorCode);
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    if (exitCode != EXIT_SUCCESS)
        Error("PCF compiler failed with exit code %lu.\n", exitCode);

    Log("*** PCF EFCT build complete: \"%s\".\n", outputPath);
}

static void RePak_ExplainUsage()
{
    Log(
        "*** RePak ( built on " __DATE__ " at " __TIME__" ) usage guide ***\n"
        "Global options:\n"
        "\t%s, %s\t- show verbose packing details and asset-level progress\n"
        "\n"
        "For building pak files, run 'repak' with the following parameter:\n"
        "\t<%s>\t- path to a map file containing the build parameters for the pak to build\n"

        "For creating stream caches, run 'repak' with the following parameter:\n"
        "\t<%s>\t- path to a directory containing streaming files to be cached\n"

        "For calculating Pak Asset guids, run 'repak %s' with the following parameter:\n"
        "\t<%s>\t- the string to compute the asset guid from\n"

        "For calculating UI Image hashes, run 'repak %s' with the following parameter:\n"
        "\t<%s>\t- the string to compute the uimg hash from\n"

        "For compressing standalone paks, run 'repak %s' with the following parameters:\n"
        "\t<%s>\t- the target pak file to compress\n"
        "\t<%s>\t- ( optional ) the level of compression [ %d, %d ]; default = %d\n"
        "\t<%s>\t- ( optional ) the number of compression workers [ %d, %d ]; default = %d\n"

        "For decompressing standalone paks, run 'repak %s' with the following parameter:\n"
        "\t<%s>\t- the target pak file to decompress\n"

        "For compiling a Source/Titanfall PCF into a standalone Season 3 EFCT pak:\n"
        "\trepak %s <%s> <%s> <%s> [<%s>]\n"
        "\t\t%s - binary DMX v5 / PCF v2 source\n"
        "\t\t%s - decoded Season 3 effects rpak used as the native EFCT template\n"
        "\t\t%s - output standalone rpak\n"
        "\t\t%s - optional editable JSON sidecar; defaults beside the PCF\n",

        REPAK_VERBOSE_COMMAND, REPAK_VERBOSE_LONG_COMMAND,

        "buildMapPath",
        "streamingPath",

        REPAK_STR_TO_GUID_COMMAND, "strToGuid",
        REPAK_STR_TO_UIMG_HASH_COMMAND, "strToHash",

        REPAK_COMPRESS_PAK_COMMAND, "pakFilePath", "compressLevel",
        -5, // See https://github.com/facebook/zstd/issues/3032
        ZSTD_maxCLevel(), REPAK_DEFAULT_COMPRESS_LEVEL,

        "workerCount",
        1, REPAK_MAX_COMPRESS_WORKERS, REPAK_DEFAULT_COMPRESS_WORKERS,

        REPAK_DECOMPRESS_PAK_COMMAND,
        "pakFilePath",

        REPAK_PCF_TO_EFCT_COMMAND,
        "pcfPath", "decodedS3EffectsRpak", "outputRpak", "projectJson",
        "pcfPath", "decodedS3EffectsRpak", "outputRpak", "projectJson"
    );
}

static inline void RePak_ValidateArguments(const char* const argName, const int argc, const int required)
{
    const int delta = required - argc;

    if (delta > 0)
    {
        if (delta == 1)
            Error("Invalid usage; \"%s\" requires an additional argument.\n", argName);

        Error("Invalid usage; \"%s\" requires %i additional arguments.\n", argName, delta);
    }
}

static bool RePak_CheckCommandLine(const char* arg, const char* const target, const int argc, const int required)
{
    if (strcmp(arg, target) == 0)
    {
        RePak_ValidateArguments(target, argc, required);
        return true;
    }

    return false;
}

static std::vector<const char*> RePak_ParseGlobalOptions(const int argc, char** argv)
{
    std::vector<const char*> args;
    args.reserve(argc);
    args.push_back(argv[0]);

    for (int i = 1; i < argc; i++)
    {
        if ((strcmp(argv[i], REPAK_VERBOSE_COMMAND) == 0) || (strcmp(argv[i], REPAK_VERBOSE_LONG_COMMAND) == 0))
        {
            g_showDebugLogs = true;
            continue;
        }

        args.push_back(argv[i]);
    }

    return args;
}

static uint16_t RePak_OpenPakAndValidateHeader(BinaryIO& bio, const char* const pakPath)
{
    if (!bio.Open(pakPath, BinaryIO::Mode_e::ReadWrite))
        Error("Failed to open pak file \"%s\" for encode job.\n", pakPath);

    const std::streamoff size = bio.GetSize();
    const std::streamoff toConsume = 6; // size of magic( 4 ) + version( 2 ).

    if (size < toConsume)
        Error("Short read on pak file \"%s\"; header criteria unavailable!\n", pakPath);

    const uint32_t magic = bio.Read<uint32_t>();

    if (magic != RPAK_MAGIC)
        Error("Pak file \"%s\" has invalid magic! ( %x != %x ).\n", pakPath, magic, RPAK_MAGIC);

    const uint16_t version = bio.Read<uint16_t>();

    if (!Pak_IsVersionSupported(version))
        Error("Pak file \"%s\" has version %hu which is unsupported!\n", pakPath, version);

    const std::streamoff headerSize = (std::streamoff)Pak_GetHeaderSize(version);

    if (size < headerSize)
        Error("Pak file \"%s\" appears truncated! ( %zd < %zd ).\n", pakPath, size, headerSize);

    return version;
}

static void RePak_HandleCompressPak(const char* const pakPath, const int compressLevel, const int workerCount)
{
    BinaryIO bio;
    const uint16_t version = RePak_OpenPakAndValidateHeader(bio, pakPath);

    // Largest header is 128 bytes (v8).
    char tempHdrBuf[128];
    bio.Seek(0);

    const size_t headerSize = Pak_GetHeaderSize(version);
    bio.Read(tempHdrBuf, headerSize);

    PakHdr_t* const hdr = (PakHdr_t*)tempHdrBuf;

    if (hdr->flags & (PAK_HEADER_FLAGS_RTECH_ENCODED | PAK_HEADER_FLAGS_OODLE_ENCODED | PAK_HEADER_FLAGS_ZSTD_ENCODED))
        Error("Pak file \"%s\" is already encoded using %s!\n", pakPath, Pak_EncodeAlgorithmToString(hdr->flags));

    const size_t newSize = Pak_EncodeStreamAndSwap(bio, compressLevel, workerCount, version, pakPath);

    if (!newSize)
        return; // Failure, don't mutate the file.

    // Update the header to accommodate for the compression method
    // and the new size so the runtime is aware of it.
    hdr->flags |= PAK_HEADER_FLAGS_ZSTD_ENCODED;
    hdr->compressedSize = newSize;

    bio.Seek(0); // Write the new header out.
    bio.Write(tempHdrBuf, headerSize);
}

static void RePak_HandleDecompressPak(const char* const pakPath)
{
    BinaryIO bio;
    const uint16_t version = RePak_OpenPakAndValidateHeader(bio, pakPath);

    // Largest header is 128 bytes (v8).
    char tempHdrBuf[128];
    bio.Seek(0);

    const size_t headerSize = Pak_GetHeaderSize(version);
    bio.Read(tempHdrBuf, headerSize);

    PakHdr_t* const hdr = (PakHdr_t*)tempHdrBuf;

    // TODO: support these are well.
    if (hdr->flags & (PAK_HEADER_FLAGS_RTECH_ENCODED | PAK_HEADER_FLAGS_OODLE_ENCODED))
        Error("Pak file \"%s\" is encoded using %s which is unsupported!\n", pakPath, Pak_EncodeAlgorithmToString(hdr->flags));

    if (!(hdr->flags & PAK_HEADER_FLAGS_ZSTD_ENCODED))
        Error("Pak file \"%s\" is already decoded!\n", pakPath);

    const size_t newSize = Pak_DecodeStreamAndSwap(bio, version, pakPath);

    if (!newSize)
        return; // Failure, don't mutate the file.

    // Update the header to accommodate for the compression method
    // and the new size so the runtime is aware of it.
    hdr->flags &= ~PAK_HEADER_FLAGS_ZSTD_ENCODED;
    hdr->compressedSize = newSize;

    // Should never happen, but in case it does inform the user and
    // equal decompressedSize to compressedSize as otherwise the
    // runtime will crash. There is no guarantee this will fix the
    // file and avoid undesired behavior in the runtime because the
    // file might just be corrupt as it is!
    if (hdr->compressedSize != hdr->decompressedSize)
    {
        Warning("Size mismatch after decoding \"%s\" ( pakHdr->compressedSize( %zu ) != pakHdr->decompressedSize( %zu ) ) -- correcting header... pak file may be corrupt!\n",
            pakPath, hdr->compressedSize, hdr->decompressedSize);

        hdr->decompressedSize = hdr->compressedSize;
    }

    bio.Seek(0); // Write the new header out.
    bio.Write(tempHdrBuf, headerSize);
}

static void RePak_HandleCommandLine(const int argc, char** argv)
{
    const std::vector<const char*> args = RePak_ParseGlobalOptions(argc, argv);
    const int argCount = static_cast<int>(args.size());

    if (argCount < 2)
    {
        RePak_ExplainUsage();
        return;
    }

    if (g_showDebugLogs)
        Debug("Verbose output enabled.\n");

    if (RePak_CheckCommandLine(args[1], REPAK_STR_TO_GUID_COMMAND, argCount, 3))
    {
        const PakGuid_t guid = RTech::StringToGuid(args[2]);
        Log("0x%llX\n", guid);

        return;
    }

    if (RePak_CheckCommandLine(args[1], REPAK_STR_TO_UIMG_HASH_COMMAND, argCount, 3))
    {
        const uint32_t hash = RTech::StringToUIMGHash(args[2]);
        Log("0x%lX\n", hash);

        return;
    }

    if (RePak_CheckCommandLine(args[1], REPAK_COMPRESS_PAK_COMMAND, argCount, 3))
    {
        int compressLevel = REPAK_DEFAULT_COMPRESS_LEVEL;

        if ((argCount > 3) && (!JSON_StringToNumber(args[3], strlen(args[3]), compressLevel)))
            Error("%s: failed to parse compressLevel for argument \"%s\".\n", __FUNCTION__, args[1]);

        int workerCount = REPAK_DEFAULT_COMPRESS_WORKERS;

        if ((argCount > 4) && (!JSON_StringToNumber(args[4], strlen(args[4]), workerCount)))
            Error("%s: failed to parse workerCount for argument \"%s\".\n", __FUNCTION__, args[1]);

        RePak_HandleCompressPak(args[2], compressLevel, workerCount);
        return;
    }

    if (RePak_CheckCommandLine(args[1], REPAK_DECOMPRESS_PAK_COMMAND, argCount, 3))
    {
        RePak_HandleDecompressPak(args[2]);
        return;
    }

    if (RePak_CheckCommandLine(args[1], REPAK_PCF_TO_EFCT_COMMAND, argCount, 5))
    {
        RePak_HandlePcfToEfct(
            args[2],
            args[3],
            args[4],
            argCount > 5 ? args[5] : nullptr);
        return;
    }

    RePak_HandleBuildFromPath(args[1]);
}

#ifndef FIRESTAR_REPAK_EMBEDDED
int main(int argc, char** argv)
{
    extern bool Console_ColorInit();
    Console_ColorInit();

    g_jsonErrorCallback = Error;

    RePak_HandleCommandLine(argc, argv);
    return EXIT_SUCCESS;
}
#else
namespace firestar::repak
{
    namespace
    {
        std::mutex g_sinkMutex;
        LogSink g_sink;
    }

    namespace detail
    {
        void SetLogSink(LogSink sink)
        {
            std::lock_guard<std::mutex> lock(g_sinkMutex);
            g_sink = std::move(sink);
        }

        void ClearLogSink()
        {
            std::lock_guard<std::mutex> lock(g_sinkMutex);
            g_sink = {};
        }

        void EmitLog(const LogLevel level, const std::string_view message)
        {
            LogSink sink;
            {
                std::lock_guard<std::mutex> lock(g_sinkMutex);
                sink = g_sink;
            }
            if (sink)
                sink(level, message);
        }
    }

    BuildResult Build(const BuildRequest& request) noexcept
    {
        BuildResult result;
        if (request.manifestPath.empty())
        {
            result.message = "No RePak manifest was provided.";
            return result;
        }

        detail::SetLogSink(request.log);
        g_showDebugLogs = request.verbose;
        g_jsonErrorCallback = Error;

        try
        {
            const std::string manifest = request.manifestPath.string();
            const std::string manifestBase = request.manifestBaseDirectory.string();
            if (!request.manifestJson.empty())
            {
                js::Document document;
                document.Parse(request.manifestJson.data(), request.manifestJson.size());
                if (document.HasParseError())
                    Error("Firestar's embedded RePak JSON is invalid at byte %zu.\n",
                        static_cast<size_t>(document.GetErrorOffset()));
                fs::path effectiveMapPath = request.manifestPath;
                if (!request.manifestBaseDirectory.empty())
                    effectiveMapPath = request.manifestBaseDirectory / request.manifestPath.filename();
                const std::string effectiveMapPathText = effectiveMapPath.string();
                RePak_BuildSingle(document, effectiveMapPathText.c_str());
            }
            else
            {
                RePak_HandleBuildFromPath(manifest.c_str(), manifestBase.empty() ? nullptr : manifestBase.c_str());
            }
            result.succeeded = true;
            result.message = "RePak build completed.";
        }
        catch (const std::exception& exception)
        {
            result.message = exception.what();
        }
        catch (...)
        {
            result.message = "RePak failed with an unknown error.";
        }

        BinaryIOCache_End();
        Logger_EndPakBuild();
        g_currentAsset = nullptr;
        detail::ClearLogSink();
        return result;
    }
}
#endif
