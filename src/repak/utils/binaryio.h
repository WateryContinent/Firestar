#pragma once

#include <memory>
#include <string>
#include <vector>

struct BinaryIOCacheStats_s
{
	size_t queuedFiles = 0;
	size_t loadedFiles = 0;
	size_t cacheHits = 0;
	size_t waitedHits = 0;
	size_t loadedBytes = 0;
};

// Starts a bounded, process-local read-ahead cache. File contents are loaded by
// worker threads, then consumed by BinaryIO and JSON parsers without reopening
// the source file. The cache is deliberately opt-in per pak build.
void BinaryIOCache_Begin(size_t workerCount, size_t maxBytes);
void BinaryIOCache_Prefetch(const std::vector<std::string>& filePaths);
std::shared_ptr<const std::vector<uint8_t>> BinaryIOCache_TryGet(const char* filePath);
BinaryIOCacheStats_s BinaryIOCache_End();

class BinaryIO
{
public:
	enum class Mode_e
	{
		None = 0,
		Read,
		Write,
		ReadWrite, // For existing files only.
		ReadWriteCreate
	};

	BinaryIO();
	~BinaryIO();

	bool Open(const char* const filePath, const Mode_e mode);
	inline bool Open(const std::string& filePath, const Mode_e mode) { return Open(filePath.c_str(), mode); };

	void Close();
	void Reset();
	void Flush();

	std::streamoff TellGet();
	std::streamoff TellPut();

	void SeekGet(const std::streamoff offset, const std::ios_base::seekdir way = std::ios::beg);
	void SeekPut(const std::streamoff offset, const std::ios_base::seekdir way = std::ios::beg);
	void Seek(const std::streamoff offset, const std::ios_base::seekdir way = std::ios::beg);

	const std::filebuf* GetData() const;
	const std::streamoff GetSize() const;

	bool IsReadMode() const;
	bool IsWriteMode() const;

	bool IsReadable() const;
	bool IsWritable() const;

	bool IsEof() const;

	//-----------------------------------------------------------------------------
	// Purpose: reads any value from the file
	//-----------------------------------------------------------------------------
	template<typename T>
	inline void Read(T& value)
	{
		ReadBytes(&value, sizeof(value));
	}

	//-----------------------------------------------------------------------------
	// Purpose: reads any value from the file with specified size
	//-----------------------------------------------------------------------------
	template<typename T>
	inline void Read(T* const value, const size_t size)
	{
		ReadBytes(value, size);
	}
	template<typename T>
	inline void Read(T& value, const size_t size)
	{
		ReadBytes(&value, size);
	}

	//-----------------------------------------------------------------------------
	// Purpose: reads any value from the file and returns it
	//-----------------------------------------------------------------------------
	template<typename T>
	inline T Read()
	{
		T value{};
		if (!IsReadable())
			return value;

		ReadBytes(&value, sizeof(value));
		return value;
	}
	bool ReadString(std::string& svOut);
	bool ReadString(char* const pBuf, const size_t nLen);

	//-----------------------------------------------------------------------------
	// Purpose: writes any value to the file
	//-----------------------------------------------------------------------------
	template<typename T>
	inline void Write(const T& value)
	{
		if (!IsWritable())
			return;

		const size_t count = sizeof(value);

		m_stream.write(reinterpret_cast<const char*>(&value), count);
		CalcAddDelta(count);
	}

	//-----------------------------------------------------------------------------
	// Purpose: writes any value to the file with specified size
	//-----------------------------------------------------------------------------
	template<typename T>
	inline void Write(const T* const value, const size_t size)
	{
		if (!IsWritable())
			return;

		m_stream.write(reinterpret_cast<const char*>(value), size);
		CalcAddDelta(size);
	}
	bool WriteString(const std::string& svInput, const bool nullterminate);
	void Pad(const size_t count);

protected:
	bool ReadBytes(void* const value, const size_t size);
	void CalcAddDelta(const size_t count);
	void CalcSkipDelta(const std::streamoff offset, const std::ios_base::seekdir way);

private:
	std::fstream            m_stream; // I/O stream.
	std::shared_ptr<const std::vector<uint8_t>> m_cachedReadData;
	std::streamoff          m_cachedReadOffset;
	std::streamoff          m_size;   // File size.
	std::streamoff          m_skip;   // Amount skipped back.
	std::ios_base::openmode m_flags;  // Stream flags.
	Mode_e                  m_mode;   // Stream mode.
};
