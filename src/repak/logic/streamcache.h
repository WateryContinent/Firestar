#pragma once
#include <filesystem>
#include <utils/utils.h>

#define STREAM_CACHE_FILE_MAGIC ('S'+('R'<<8)+('M'<<16)+('p'<<24))
#define STREAM_CACHE_FILE_MAJOR_VERSION 2
#define STREAM_CACHE_FILE_MINOR_VERSION 4

struct StreamCacheFileHeader_s
{
	int magic;

	unsigned short majorVersion;
	unsigned short minorVersion;

	size_t streamingFileCount;

	size_t dataEntryCount;
	size_t dataEntriesOffset;
};

struct StreamCacheFileEntry_s
{
	bool isOptional;
	std::string streamFilePath;
};

struct StreamCacheDataEntry_s
{
	int64_t dataOffset : 52;
	int64_t pathIndex : 12;
	int64_t dataSize;
	__m128i hash;
};

struct StreamCacheFindParams_s
{
	__m128i hash;
	int64_t size;
	const char* streamFilePath;
};

struct StreamCacheFindResult_s
{
	const StreamCacheFileEntry_s* fileEntry;
	const StreamCacheDataEntry_s* dataEntry;
};

struct StreamCacheLookupKey_s
{
	uint64_t hashLow;
	uint64_t hashHigh;
	int64_t dataSize;
	bool optional;

	bool operator==(const StreamCacheLookupKey_s& other) const
	{
		return hashLow == other.hashLow && hashHigh == other.hashHigh &&
			dataSize == other.dataSize && optional == other.optional;
	}
};

struct StreamCacheLookupKeyHash_s
{
	size_t operator()(const StreamCacheLookupKey_s& key) const
	{
		size_t value = std::hash<uint64_t>{}(key.hashLow);
		value ^= std::hash<uint64_t>{}(key.hashHigh) + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
		value ^= std::hash<int64_t>{}(key.dataSize) + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
		value ^= std::hash<bool>{}(key.optional) + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
		return value;
	}
};

class CStreamCache
{
public:
	void BuildStarMapFromPaksDirectory(const char* const streamCacheFile);
	void ParseMap(const char* const streamCacheFile);

	int64_t AddStarPakPathToMapList(const std::string& path, const bool optional);
	StreamCacheFileHeader_s ConstructHeader() const;

	static StreamCacheFindParams_s CreateParams(const uint8_t* const data, const int64_t size, const char* const streamFilePath);

	bool Find(const StreamCacheFindParams_s& params, StreamCacheFindResult_s& result, const bool optional);
	void Add(const StreamCacheFindParams_s& params, const int64_t offset, const bool optional);

	void WriteCacheFileToIOStream(BinaryIO& io);

	void AddStreamFileToFilter(const std::string& streamFile);
	void AddStreamFileToFilter(const char* const streamFile, const size_t nameLen);

	bool IsStreamFileInFilter(const std::string& streamFile) const;

	inline bool HasStreamFileFilter() const { return !m_cacheFilter.empty(); }

private:
	static StreamCacheLookupKey_s MakeLookupKey(const __m128i& hash, const int64_t size, const bool optional);
	void RebuildLookupIndexes();

	std::vector<StreamCacheFileEntry_s> m_streamFiles;
	std::vector<StreamCacheDataEntry_s> m_dataEntries;
	std::unordered_multimap<StreamCacheLookupKey_s, size_t, StreamCacheLookupKeyHash_s> m_dataIndex;
	std::unordered_map<std::string, int64_t> m_streamFileIndex;
	std::unordered_set<std::string> m_cacheFilter;
};
