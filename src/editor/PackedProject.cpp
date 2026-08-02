#include "PackedProject.h"

#include <oodle/oodle2.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <span>

namespace fs = std::filesystem;

namespace
{
#pragma pack(push, 1)
    struct PackedProjectHeader
    {
        char magic[8]{'F', 'I', 'R', 'E', 'S', 'P', 'A', '1'};
        std::uint32_t version{1};
        std::uint32_t compressor{static_cast<std::uint32_t>(OodleLZ_Compressor_Kraken)};
        std::uint32_t entryCount{};
        std::uint32_t payloadCrc32{};
        std::uint64_t uncompressedBytes{};
        std::uint64_t compressedBytes{};
    };

    struct PackedProjectEntryHeader
    {
        std::uint32_t pathBytes{};
        std::uint32_t reserved{};
        std::uint64_t fileBytes{};
    };
#pragma pack(pop)

    constexpr std::uint64_t MaximumArchiveBytes = 8ull * 1024ull * 1024ull * 1024ull;
    constexpr std::uint32_t MaximumArchiveEntries = 100000;
    constexpr std::uint32_t MaximumArchivePathBytes = 32768;

    std::string PathToUtf8(const fs::path& path)
    {
        const std::u8string value = path.generic_u8string();
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    }

    fs::path PathFromUtf8(const std::string& path)
    {
        const std::u8string value(reinterpret_cast<const char8_t*>(path.data()), path.size());
        return fs::path(value);
    }

    std::uint32_t Crc32(const std::span<const std::uint8_t> bytes)
    {
        std::uint32_t crc = 0xFFFFFFFFu;
        for (const std::uint8_t byte : bytes)
        {
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

    bool SafeRelativePath(const fs::path& path)
    {
        if (path.empty() || path.is_absolute() || path.has_root_directory() || path.has_root_name())
            return false;
        for (const fs::path& component : path)
        {
            if (component.empty() || component == L"." || component == L"..")
                return false;
        }
        return true;
    }

    std::string PathKey(std::string value)
    {
        std::replace(value.begin(), value.end(), '\\', '/');
        std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        return value;
    }

    template <typename T>
    void Append(std::vector<std::uint8_t>& bytes, const T& value)
    {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
        bytes.insert(bytes.end(), begin, begin + sizeof(value));
    }

    bool ReadBytes(const std::vector<std::uint8_t>& bytes, size_t& cursor, void* output, const size_t count)
    {
        if (count > bytes.size() - (std::min)(cursor, bytes.size()))
            return false;
        std::memcpy(output, bytes.data() + cursor, count);
        cursor += count;
        return true;
    }
}

namespace firestar::editor
{
    bool WritePackedProject(const fs::path& destination,
        const std::vector<PackedProjectEntry>& entries, std::string& error)
    {
        error.clear();
        if (entries.empty() || entries.size() > MaximumArchiveEntries)
        {
            error = "The packed project has an invalid number of files.";
            return false;
        }

        std::set<std::string> uniquePaths;
        std::vector<std::uint8_t> payload;
        for (const PackedProjectEntry& entry : entries)
        {
            const fs::path relative = entry.relativePath.lexically_normal();
            const std::string path = PathToUtf8(relative);
            if (!SafeRelativePath(relative) || path.empty() || path.size() > MaximumArchivePathBytes ||
                !uniquePaths.insert(PathKey(path)).second)
            {
                error = "The packed project contains an invalid or duplicate path: " + path;
                return false;
            }
            const std::uint64_t required = sizeof(PackedProjectEntryHeader) + path.size() + entry.bytes.size();
            if (required > MaximumArchiveBytes || payload.size() > MaximumArchiveBytes - required)
            {
                error = "The packed project is too large.";
                return false;
            }
            PackedProjectEntryHeader header;
            header.pathBytes = static_cast<std::uint32_t>(path.size());
            header.fileBytes = entry.bytes.size();
            Append(payload, header);
            payload.insert(payload.end(), path.begin(), path.end());
            payload.insert(payload.end(), entry.bytes.begin(), entry.bytes.end());
        }

        const OO_SINTa bound = OodleLZ_GetCompressedBufferSizeNeeded(
            OodleLZ_Compressor_Kraken, static_cast<OO_SINTa>(payload.size()));
        if (bound <= 0 || static_cast<std::uint64_t>(bound) > MaximumArchiveBytes)
        {
            error = "Oodle could not size the packed project buffer.";
            return false;
        }
        std::vector<std::uint8_t> compressed(static_cast<size_t>(bound));
        const OO_SINTa compressedSize = OodleLZ_Compress(OodleLZ_Compressor_Kraken,
            payload.data(), static_cast<OO_SINTa>(payload.size()), compressed.data(),
            OodleLZ_CompressionLevel_Optimal2);
        if (compressedSize <= 0 || compressedSize > bound)
        {
            error = "Oodle could not compress the Firestar project.";
            return false;
        }
        compressed.resize(static_cast<size_t>(compressedSize));

        PackedProjectHeader archiveHeader;
        archiveHeader.entryCount = static_cast<std::uint32_t>(entries.size());
        archiveHeader.payloadCrc32 = Crc32(payload);
        archiveHeader.uncompressedBytes = payload.size();
        archiveHeader.compressedBytes = compressed.size();

        std::error_code ioError;
        if (!destination.parent_path().empty())
            fs::create_directories(destination.parent_path(), ioError);
        if (ioError)
        {
            error = "Unable to create the packed project folder: " + ioError.message();
            return false;
        }
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            error = "Unable to write the packed Firestar project.";
            return false;
        }
        output.write(reinterpret_cast<const char*>(&archiveHeader), sizeof(archiveHeader));
        output.write(reinterpret_cast<const char*>(compressed.data()),
            static_cast<std::streamsize>(compressed.size()));
        if (!output.good())
        {
            error = "The packed Firestar project write was incomplete.";
            return false;
        }
        return true;
    }

    bool ReadPackedProject(const fs::path& source, std::vector<PackedProjectEntry>& entries,
        std::string& error)
    {
        entries.clear();
        error.clear();
        std::ifstream input(source, std::ios::binary | std::ios::ate);
        if (!input)
        {
            error = "Unable to open the packed Firestar project.";
            return false;
        }
        const std::streamoff totalSize = input.tellg();
        if (totalSize < static_cast<std::streamoff>(sizeof(PackedProjectHeader)))
        {
            error = "This is not a packed Firestar project.";
            return false;
        }
        input.seekg(0);
        PackedProjectHeader archiveHeader;
        if (!input.read(reinterpret_cast<char*>(&archiveHeader), sizeof(archiveHeader)) ||
            std::memcmp(archiveHeader.magic, "FIRESPA1", 8) != 0 || archiveHeader.version != 1 ||
            archiveHeader.compressor != OodleLZ_Compressor_Kraken ||
            archiveHeader.entryCount == 0 || archiveHeader.entryCount > MaximumArchiveEntries ||
            archiveHeader.uncompressedBytes > MaximumArchiveBytes ||
            archiveHeader.compressedBytes > MaximumArchiveBytes ||
            archiveHeader.compressedBytes != static_cast<std::uint64_t>(totalSize) - sizeof(archiveHeader))
        {
            error = "This packed Firestar project has an unsupported or damaged header.";
            return false;
        }
        std::vector<std::uint8_t> compressed(static_cast<size_t>(archiveHeader.compressedBytes));
        if (!input.read(reinterpret_cast<char*>(compressed.data()),
            static_cast<std::streamsize>(compressed.size())))
        {
            error = "The packed Firestar project ended unexpectedly.";
            return false;
        }
        std::vector<std::uint8_t> payload(static_cast<size_t>(archiveHeader.uncompressedBytes));
        const OO_SINTa decoded = OodleLZ_Decompress(compressed.data(),
            static_cast<OO_SINTa>(compressed.size()), payload.data(), static_cast<OO_SINTa>(payload.size()),
            OodleLZ_FuzzSafe_Yes, OodleLZ_CheckCRC_No, OodleLZ_Verbosity_None);
        if (decoded != static_cast<OO_SINTa>(payload.size()) || Crc32(payload) != archiveHeader.payloadCrc32)
        {
            error = "The packed Firestar project failed its Oodle or CRC validation.";
            return false;
        }

        size_t cursor = 0;
        std::set<std::string> uniquePaths;
        for (std::uint32_t index = 0; index < archiveHeader.entryCount; ++index)
        {
            PackedProjectEntryHeader entryHeader;
            if (!ReadBytes(payload, cursor, &entryHeader, sizeof(entryHeader)) ||
                entryHeader.pathBytes == 0 || entryHeader.pathBytes > MaximumArchivePathBytes ||
                entryHeader.fileBytes > MaximumArchiveBytes ||
                entryHeader.pathBytes > payload.size() - (std::min)(cursor, payload.size()))
            {
                error = "The packed Firestar project contains a damaged file entry.";
                return false;
            }
            std::string path(entryHeader.pathBytes, '\0');
            if (!ReadBytes(payload, cursor, path.data(), path.size()) || path.find('\0') != std::string::npos ||
                !uniquePaths.insert(PathKey(path)).second)
            {
                error = "The packed Firestar project contains an invalid file path.";
                return false;
            }
            PackedProjectEntry entry;
            entry.relativePath = PathFromUtf8(path).lexically_normal();
            if (!SafeRelativePath(entry.relativePath) ||
                entryHeader.fileBytes > payload.size() - (std::min)(cursor, payload.size()))
            {
                error = "The packed Firestar project contains an unsafe file path or size.";
                return false;
            }
            entry.bytes.resize(static_cast<size_t>(entryHeader.fileBytes));
            if (!ReadBytes(payload, cursor, entry.bytes.data(), entry.bytes.size()))
            {
                error = "The packed Firestar project contains a truncated file.";
                return false;
            }
            entries.push_back(std::move(entry));
        }
        if (cursor != payload.size())
        {
            error = "The packed Firestar project has unexpected trailing data.";
            return false;
        }
        return true;
    }

    bool ExtractPackedProject(const fs::path& source, const fs::path& destinationRoot,
        fs::path& extractedProject, std::string& error)
    {
        std::vector<PackedProjectEntry> entries;
        if (!ReadPackedProject(source, entries, error))
            return false;
        const size_t projectCount = static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
            [](const PackedProjectEntry& entry) { return entry.relativePath.extension() == L".fsp"; }));
        if (projectCount != 1)
        {
            error = projectCount == 0
                ? "The packed project does not contain a Firestar .fsp file."
                : "The packed project contains more than one .fsp file.";
            return false;
        }
        std::error_code ioError;
        if (fs::exists(destinationRoot, ioError) && !fs::is_empty(destinationRoot, ioError))
        {
            error = "The extraction folder already exists and is not empty.";
            return false;
        }
        fs::create_directories(destinationRoot, ioError);
        if (ioError)
        {
            error = "Unable to create the extraction folder: " + ioError.message();
            return false;
        }

        extractedProject.clear();
        for (const PackedProjectEntry& entry : entries)
        {
            const fs::path destination = (destinationRoot / entry.relativePath).lexically_normal();
            const fs::path relativeCheck = fs::relative(destination, destinationRoot, ioError);
            if (ioError || !SafeRelativePath(relativeCheck))
            {
                error = "A packed project file tried to leave the extraction folder.";
                return false;
            }
            fs::create_directories(destination.parent_path(), ioError);
            if (ioError)
            {
                error = "Unable to create an extracted file folder: " + ioError.message();
                return false;
            }
            std::ofstream output(destination, std::ios::binary | std::ios::trunc);
            if (!output || (!entry.bytes.empty() &&
                !output.write(reinterpret_cast<const char*>(entry.bytes.data()),
                    static_cast<std::streamsize>(entry.bytes.size()))))
            {
                error = "Unable to extract " + PathToUtf8(entry.relativePath);
                return false;
            }
            if (entry.relativePath.extension() == L".fsp")
                extractedProject = destination;
        }
        fs::create_directories(destinationRoot / L"build", ioError);
        if (ioError || extractedProject.empty())
        {
            error = extractedProject.empty()
                ? "The packed project does not contain a Firestar .fsp file."
                : "Unable to create the extracted build folder: " + ioError.message();
            return false;
        }
        return true;
    }
}
