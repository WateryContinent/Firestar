#pragma once

extern const char* g_currentAsset;
extern bool g_showDebugLogs;

extern void Logger_colorInit();
extern void Logger_BeginPakBuild(const char* const pakPath, const size_t totalAssets);
extern void Logger_UpdateAssetStatus(const char* const assetType, const char* const assetPath, const size_t assetIndex, const size_t totalAssets);
extern void Logger_UpdateBuildProgress(const size_t completedAssets, const size_t totalAssets);
extern void Logger_SetBuildStatus(const char* const status);
extern void Logger_EndPakBuild();

// non-fatal errors/issues
void Warning(_Printf_format_string_ const char* fmt, ...);
// fatal errors
void Error(_Printf_format_string_ const char* fmt, ...);
// general prints for Release
void Log(_Printf_format_string_ const char* fmt, ...);
// any prints that shouldn't be used in Release
void Debug(_Printf_format_string_ const char* fmt, ...);
