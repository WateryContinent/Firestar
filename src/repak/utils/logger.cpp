#include "pch.h"
#include "logger.h"

const char* g_currentAsset = nullptr;
bool g_showDebugLogs = false;

static std::string s_debugColorCode;
static std::string s_warningColorCode;
static std::string s_errorColorCode;
static std::string s_resetColorCode;
static std::mutex s_logMutex;
static bool s_statusActive = false;
static bool s_statusVisible = false;
static std::string s_statusText;
static std::chrono::steady_clock::time_point s_lastStatusDraw{};
static constexpr std::chrono::milliseconds STATUS_DRAW_INTERVAL(100);

static short Logger_GetConsoleWidth()
{
	CONSOLE_SCREEN_BUFFER_INFO info{};
	if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
		return static_cast<short>(info.srWindow.Right - info.srWindow.Left + 1);

	return 120;
}

static std::string Logger_FitStatusLine(std::string text)
{
	const short width = Logger_GetConsoleWidth();
	const size_t maxLen = width > 4 ? static_cast<size_t>(width - 1) : 80u;

	if (text.length() <= maxLen)
		return text;

	if (maxLen <= 3)
		return text.substr(0, maxLen);

	text.resize(maxLen - 3);
	text += "...";

	return text;
}

static void Logger_ClearStatusLine_NoLock()
{
	if (!s_statusVisible)
		return;

	printf("\r\x1B[2K");
	fflush(stdout);
	s_statusVisible = false;
}

static void Logger_DrawStatusLine_NoLock(const bool force = false)
{
	if (!s_statusActive || s_statusText.empty())
		return;

	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	if (!force && s_lastStatusDraw.time_since_epoch().count() != 0 && now - s_lastStatusDraw < STATUS_DRAW_INTERVAL)
		return;

	printf("\r\x1B[2K%s", Logger_FitStatusLine(s_statusText).c_str());
	fflush(stdout);
	s_statusVisible = true;
	s_lastStatusDraw = now;
}

static std::string Logger_MakeProgressBar(const size_t completedAssets, const size_t totalAssets)
{
	constexpr size_t barWidth = 32;
	const size_t clampedCompleted = totalAssets > 0 ? (completedAssets < totalAssets ? completedAssets : totalAssets) : 0;
	const size_t filled = totalAssets > 0 ? ((clampedCompleted * barWidth) / totalAssets) : barWidth;
	const int percent = totalAssets > 0 ? static_cast<int>((clampedCompleted * 100) / totalAssets) : 100;

	std::string bar;
	bar.reserve(barWidth);

	for (size_t i = 0; i < barWidth; i++)
		bar += i < filled ? '#' : '-';

	return Utils::VFormat("Progress [%s] %3i%%  %zu/%zu assets", bar.c_str(), percent, clampedCompleted, totalAssets);
}

void Logger_colorInit()
{
	s_debugColorCode = "\x1B[94m";
	s_warningColorCode = "\x1B[93m";
	s_errorColorCode = "\x1B[91m";
	s_resetColorCode = "\033[0m";
}

void Logger_BeginPakBuild(const char* const pakPath, const size_t totalAssets)
{
	std::lock_guard<std::mutex> lock(s_logMutex);

	Logger_ClearStatusLine_NoLock();
	printf("RePak (rexx, built on " __DATE__ " at " __TIME__ ")\n");
	printf("Building: %s\n", pakPath);

	s_statusActive = true;
	s_statusText = g_showDebugLogs
		? "Adding: waiting for assets..."
		: Logger_MakeProgressBar(0, totalAssets);

	Logger_DrawStatusLine_NoLock(true);
}

void Logger_UpdateAssetStatus(const char* const assetType, const char* const assetPath, const size_t assetIndex, const size_t totalAssets)
{
	std::lock_guard<std::mutex> lock(s_logMutex);

	if (!s_statusActive)
		return;

	// Normal mode updates once after the asset completes. Only verbose mode
	// needs the current type/path before the asset starts.
	if (!g_showDebugLogs)
		return;

	s_statusText = Utils::VFormat("Adding [%zu/%zu] %.4s %s", assetIndex + 1, totalAssets, assetType, assetPath);
	Logger_DrawStatusLine_NoLock();
}

void Logger_UpdateBuildProgress(const size_t completedAssets, const size_t totalAssets)
{
	std::lock_guard<std::mutex> lock(s_logMutex);

	if (!s_statusActive)
		return;

	// Verbose mode already updates the line before each asset. Drawing the same
	// text again here doubled the number of synchronous console flushes.
	if (g_showDebugLogs)
		return;

	s_statusText = Logger_MakeProgressBar(completedAssets, totalAssets);
	Logger_DrawStatusLine_NoLock();
}

void Logger_SetBuildStatus(const char* const status)
{
	std::lock_guard<std::mutex> lock(s_logMutex);

	if (!s_statusActive)
		return;

	s_statusText = status;
	Logger_DrawStatusLine_NoLock(true);
}

void Logger_EndPakBuild()
{
	std::lock_guard<std::mutex> lock(s_logMutex);

	Logger_ClearStatusLine_NoLock();
	s_statusActive = false;
	s_statusText.clear();
}

void Warning(_Printf_format_string_ const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	std::string msg;

	if (g_currentAsset)
		msg = Utils::VFormat("WARNING( %s ): %s%s%s", g_currentAsset, s_warningColorCode.c_str(), fmt, s_resetColorCode.c_str());
	else
		msg = "WARNING: " + s_warningColorCode + fmt + s_resetColorCode;

	{
		std::lock_guard<std::mutex> lock(s_logMutex);
		Logger_ClearStatusLine_NoLock();
		vprintf(msg.c_str(), args);
		Logger_DrawStatusLine_NoLock();
	}
	va_end(args);
}

void Error(_Printf_format_string_ const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	std::string msg;

	if (g_currentAsset)
		msg = Utils::VFormat("ERROR( %s ): %s%s%s", g_currentAsset, s_errorColorCode.c_str(), fmt, s_resetColorCode.c_str());
	else
		msg = "ERROR: " + s_errorColorCode + fmt + s_resetColorCode;

	{
		std::lock_guard<std::mutex> lock(s_logMutex);
		Logger_ClearStatusLine_NoLock();
		vprintf(msg.c_str(), args);
	}
	va_end(args);

	exit(EXIT_FAILURE);
}

void Log(_Printf_format_string_ const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	{
		std::lock_guard<std::mutex> lock(s_logMutex);
		Logger_ClearStatusLine_NoLock();
		vprintf(fmt, args);
		Logger_DrawStatusLine_NoLock();
	}
	va_end(args);
}

void Debug(_Printf_format_string_ const char* fmt, ...)
{
	if (!g_showDebugLogs)
		return;

	va_list args;
	va_start(args, fmt);

	std::string msg = "[D] " + s_debugColorCode + fmt + s_resetColorCode;

	{
		std::lock_guard<std::mutex> lock(s_logMutex);
		Logger_ClearStatusLine_NoLock();
		vprintf(msg.c_str(), args);
		Logger_DrawStatusLine_NoLock();
	}
	va_end(args);
}
