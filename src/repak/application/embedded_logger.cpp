#include "pch.h"
#include "application/embedded.h"
#include "utils/logger.h"

#include <cstdarg>
#include <cstdio>
#include <stdexcept>

const char* g_currentAsset = nullptr;
bool g_showDebugLogs = false;

namespace
{
    std::mutex g_logMutex;

    std::string FormatMessage(const char* const format, va_list arguments)
    {
        va_list countArguments;
        va_copy(countArguments, arguments);
        const int length = _vscprintf(format, countArguments);
        va_end(countArguments);

        if (length <= 0)
            return {};

        std::string result(static_cast<size_t>(length), '\0');
        va_list writeArguments;
        va_copy(writeArguments, arguments);
        vsnprintf_s(result.data(), result.size() + 1, _TRUNCATE, format, writeArguments);
        va_end(writeArguments);
        return result;
    }

    std::string PrefixAsset(std::string message)
    {
        if (!g_currentAsset)
            return message;
        return std::string(g_currentAsset) + ": " + message;
    }

    void EmitFormatted(const firestar::repak::LogLevel level, const char* const format, va_list arguments)
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        firestar::repak::detail::EmitLog(level, PrefixAsset(FormatMessage(format, arguments)));
    }
}

void Logger_colorInit()
{
}

void Logger_BeginPakBuild(const char* const pakPath, const size_t totalAssets)
{
    firestar::repak::detail::EmitLog(firestar::repak::LogLevel::Info,
        Utils::VFormat("Building %s (%zu assets)", pakPath, totalAssets));
}

void Logger_UpdateAssetStatus(const char* const assetType, const char* const assetPath,
    const size_t assetIndex, const size_t totalAssets)
{
    if (!g_showDebugLogs)
        return;
    firestar::repak::detail::EmitLog(firestar::repak::LogLevel::Progress,
        Utils::VFormat("Adding [%zu/%zu] %.4s %s", assetIndex + 1, totalAssets, assetType, assetPath));
}

void Logger_UpdateBuildProgress(const size_t completedAssets, const size_t totalAssets)
{
    if (g_showDebugLogs)
        return;
    firestar::repak::detail::EmitLog(firestar::repak::LogLevel::Progress,
        Utils::VFormat("Packed %zu/%zu assets", completedAssets, totalAssets));
}

void Logger_SetBuildStatus(const char* const status)
{
    firestar::repak::detail::EmitLog(firestar::repak::LogLevel::Progress, status ? status : "");
}

void Logger_EndPakBuild()
{
}

void Warning(const char* const format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    EmitFormatted(firestar::repak::LogLevel::Warning, format, arguments);
    va_end(arguments);
}

void Error(const char* const format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const std::string message = PrefixAsset(FormatMessage(format, arguments));
    va_end(arguments);

    firestar::repak::detail::EmitLog(firestar::repak::LogLevel::Error, message);
    throw std::runtime_error(message.empty() ? "RePak build failed." : message);
}

void Log(const char* const format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    EmitFormatted(firestar::repak::LogLevel::Info, format, arguments);
    va_end(arguments);
}

void Debug(const char* const format, ...)
{
    if (!g_showDebugLogs)
        return;
    va_list arguments;
    va_start(arguments, format);
    EmitFormatted(firestar::repak::LogLevel::Debug, format, arguments);
    va_end(arguments);
}
