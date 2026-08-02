#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace firestar::repak
{
    enum class LogLevel
    {
        Debug,
        Info,
        Warning,
        Error,
        Progress
    };

    using LogSink = std::function<void(LogLevel level, std::string_view message)>;

    struct BuildRequest
    {
        std::filesystem::path manifestPath;
        // Some existing build lists are stored in a json/ subfolder but are
        // staged beside their sdk_depot/map_depot folders by their batch file.
        // This preserves that path base without creating a temporary manifest.
        std::filesystem::path manifestBaseDirectory;
        // When supplied, Firestar builds this in-memory JSON document instead
        // of rereading manifestPath. manifestPath remains the virtual path used
        // to resolve assetsDir, outputDir, and streaming paths.
        std::string manifestJson;
        bool verbose = false;
        LogSink log;
    };

    struct BuildResult
    {
        bool succeeded = false;
        std::string message;
    };

    // Builds a RePak map in the current process. Fatal RePak errors are caught
    // and returned to the caller; this function never intentionally terminates
    // the Firestar process.
    [[nodiscard]] BuildResult Build(const BuildRequest& request) noexcept;

    namespace detail
    {
        void SetLogSink(LogSink sink);
        void ClearLogSink();
        void EmitLog(LogLevel level, std::string_view message);
    }
}
