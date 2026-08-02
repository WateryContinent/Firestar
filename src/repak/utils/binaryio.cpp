#include "pch.h"
#include "binaryio.h"
#include <sys/stat.h>

namespace
{
	enum class CacheEntryState_e
	{
		Queued,
		Loading,
		Ready,
		Unavailable
	};

	struct BinaryIOCacheEntry_s
	{
		CacheEntryState_e state = CacheEntryState_e::Queued;
		std::shared_ptr<std::vector<uint8_t>> data;
	};

	struct BinaryIOCacheState_s
	{
		std::mutex mutex;
		std::condition_variable condition;
		std::vector<std::thread> workers;
		std::deque<std::string> queue;
		std::unordered_map<std::string, std::shared_ptr<BinaryIOCacheEntry_s>> entries;
		BinaryIOCacheStats_s stats;
		size_t maxBytes = 0;
		size_t reservedBytes = 0;
		bool stopping = false;
		bool active = false;
	};

	// Intentionally process-lifetime storage. Error() terminates the process
	// immediately, so avoiding a static object with joinable thread destructors
	// keeps fatal asset errors from invoking std::terminate during shutdown.
	BinaryIOCacheState_s& GetBinaryIOCacheState()
	{
		static BinaryIOCacheState_s* const state = new BinaryIOCacheState_s;
		return *state;
	}

	std::string NormalizeCachePath(const char* const filePath)
	{
		std::string path = std::filesystem::path(filePath).lexically_normal().generic_string();

#ifdef _WIN32
		std::transform(path.begin(), path.end(), path.begin(), [](const unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
#endif

		return path;
	}

	void BinaryIOCache_Worker()
	{
		BinaryIOCacheState_s& cache = GetBinaryIOCacheState();

		for (;;)
		{
			std::string path;
			std::shared_ptr<BinaryIOCacheEntry_s> entry;

			{
				std::unique_lock<std::mutex> lock(cache.mutex);
				cache.condition.wait(lock, [&cache]() { return cache.stopping || !cache.queue.empty(); });

				if (cache.stopping)
					return;

				path = std::move(cache.queue.front());
				cache.queue.pop_front();
				entry = cache.entries.at(NormalizeCachePath(path.c_str()));
				entry->state = CacheEntryState_e::Loading;
			}

			std::ifstream input(path, std::ios::binary | std::ios::ate);
			if (!input)
			{
				std::lock_guard<std::mutex> lock(cache.mutex);
				entry->state = CacheEntryState_e::Unavailable;
				cache.condition.notify_all();
				continue;
			}

			const std::streamoff fileSize = input.tellg();
			if (fileSize < 0 || static_cast<uint64_t>(fileSize) > static_cast<uint64_t>(SIZE_MAX))
			{
				std::lock_guard<std::mutex> lock(cache.mutex);
				entry->state = CacheEntryState_e::Unavailable;
				cache.condition.notify_all();
				continue;
			}

			const size_t size = static_cast<size_t>(fileSize);
			{
				std::lock_guard<std::mutex> lock(cache.mutex);
				if (size > cache.maxBytes - (std::min)(cache.reservedBytes, cache.maxBytes))
				{
					entry->state = CacheEntryState_e::Unavailable;
					cache.condition.notify_all();
					continue;
				}

				cache.reservedBytes += size;
			}

			auto data = std::make_shared<std::vector<uint8_t>>(size);
			input.seekg(0, std::ios::beg);

			if (size > 0)
				input.read(reinterpret_cast<char*>(data->data()), size);

			const bool readSucceeded = input.good() || (input.eof() && input.gcount() == static_cast<std::streamsize>(size));
			{
				std::lock_guard<std::mutex> lock(cache.mutex);
				if (!readSucceeded)
				{
					cache.reservedBytes -= size;
					entry->state = CacheEntryState_e::Unavailable;
				}
				else
				{
					entry->data = std::move(data);
					entry->state = CacheEntryState_e::Ready;
					cache.stats.loadedFiles++;
					cache.stats.loadedBytes += size;
				}

				cache.condition.notify_all();
			}
		}
	}
}

void BinaryIOCache_Begin(const size_t workerCount, const size_t maxBytes)
{
	BinaryIOCacheState_s& cache = GetBinaryIOCacheState();
	BinaryIOCache_End();

	if (workerCount == 0 || maxBytes == 0)
		return;

	{
		std::lock_guard<std::mutex> lock(cache.mutex);
		cache.active = true;
		cache.stopping = false;
		cache.maxBytes = maxBytes;
		cache.reservedBytes = 0;
		cache.stats = {};
	}

	cache.workers.reserve(workerCount);
	for (size_t i = 0; i < workerCount; ++i)
		cache.workers.emplace_back(BinaryIOCache_Worker);
}

void BinaryIOCache_Prefetch(const std::vector<std::string>& filePaths)
{
	BinaryIOCacheState_s& cache = GetBinaryIOCacheState();
	std::lock_guard<std::mutex> lock(cache.mutex);

	if (!cache.active)
		return;

	for (const std::string& path : filePaths)
	{
		const std::string key = NormalizeCachePath(path.c_str());
		if (cache.entries.contains(key))
			continue;

		cache.entries.emplace(key, std::make_shared<BinaryIOCacheEntry_s>());
		cache.queue.push_back(path);
		cache.stats.queuedFiles++;
	}

	cache.condition.notify_all();
}

std::shared_ptr<const std::vector<uint8_t>> BinaryIOCache_TryGet(const char* const filePath)
{
	BinaryIOCacheState_s& cache = GetBinaryIOCacheState();
	std::unique_lock<std::mutex> lock(cache.mutex);

	if (!cache.active)
		return nullptr;

	const auto it = cache.entries.find(NormalizeCachePath(filePath));
	if (it == cache.entries.end())
		return nullptr;

	const std::shared_ptr<BinaryIOCacheEntry_s>& entry = it->second;
	const bool waited = entry->state == CacheEntryState_e::Queued || entry->state == CacheEntryState_e::Loading;

	if (waited)
	{
		cache.condition.wait(lock, [&cache, &entry]() {
			return cache.stopping || entry->state == CacheEntryState_e::Ready || entry->state == CacheEntryState_e::Unavailable;
		});
	}

	if (entry->state != CacheEntryState_e::Ready)
		return nullptr;

	cache.stats.cacheHits++;
	if (waited)
		cache.stats.waitedHits++;

	return entry->data;
}

BinaryIOCacheStats_s BinaryIOCache_End()
{
	BinaryIOCacheState_s& cache = GetBinaryIOCacheState();
	{
		std::lock_guard<std::mutex> lock(cache.mutex);
		if (!cache.active && cache.workers.empty())
			return cache.stats;

		cache.stopping = true;
		cache.condition.notify_all();
	}

	for (std::thread& worker : cache.workers)
	{
		if (worker.joinable())
			worker.join();
	}

	std::lock_guard<std::mutex> lock(cache.mutex);
	const BinaryIOCacheStats_s stats = cache.stats;
	cache.workers.clear();
	cache.queue.clear();
	cache.entries.clear();
	cache.maxBytes = 0;
	cache.reservedBytes = 0;
	cache.stopping = false;
	cache.active = false;
	return stats;
}

//-----------------------------------------------------------------------------
// Purpose: CIOStream constructors
//-----------------------------------------------------------------------------
BinaryIO::BinaryIO()
{
	Reset();
}

//-----------------------------------------------------------------------------
// Purpose: CIOStream destructor
//-----------------------------------------------------------------------------
BinaryIO::~BinaryIO()
{
	Close();
}

//-----------------------------------------------------------------------------
// Purpose: get internal stream mode from selected mode
//-----------------------------------------------------------------------------
static std::ios_base::openmode GetInternalStreamMode(const BinaryIO::Mode_e mode)
{
	switch (mode)
	{
	case BinaryIO::Mode_e::Read:
		return (std::ios::in | std::ios::binary);
	case BinaryIO::Mode_e::Write:
		return (std::ios::out | std::ios::binary);
	case BinaryIO::Mode_e::ReadWrite:
		return (std::ios::in | std::ios::out | std::ios::binary);
	case BinaryIO::Mode_e::ReadWriteCreate:
		return (std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	}

	assert(0); // code bug, can never reach this.
	return 0;
}

//-----------------------------------------------------------------------------
// Purpose: opens the file in specified mode
// Input  : *filePath - 
//			mode - 
// Output : true if operation is successful
//-----------------------------------------------------------------------------
bool BinaryIO::Open(const char* const filePath, const Mode_e mode)
{
	m_flags = GetInternalStreamMode(mode);
	m_mode = mode;
	m_cachedReadData.reset();
	m_cachedReadOffset = 0;

	if (m_stream.is_open())
	{
		m_stream.close();
	}

	if (mode == Mode_e::Read)
	{
		m_cachedReadData = BinaryIOCache_TryGet(filePath);
		if (m_cachedReadData)
		{
			m_size = static_cast<std::streamoff>(m_cachedReadData->size());
			return true;
		}
	}

	m_stream.open(filePath, m_flags);

	if (!m_stream.is_open() || !m_stream.good())
	{
		return false;
	}

	if (IsReadMode())
	{
		struct _stat64 status;
		if (_stat64(filePath, &status) != NULL)
		{
			return false;
		}

		m_size = status.st_size;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: resets the state
//-----------------------------------------------------------------------------
void BinaryIO::Reset()
{
	m_cachedReadData.reset();
	m_cachedReadOffset = 0;
	m_size = 0;
	m_skip = 0;
	m_mode = Mode_e::None;
	m_flags = 0;
}

//-----------------------------------------------------------------------------
// Purpose: closes the stream
//-----------------------------------------------------------------------------
void BinaryIO::Close()
{
	if (m_stream.is_open())
		m_stream.close();
	Reset();
}

//-----------------------------------------------------------------------------
// Purpose: flushes the ofstream
//-----------------------------------------------------------------------------
void BinaryIO::Flush()
{
	if (IsWritable())
		m_stream.flush();
}

//-----------------------------------------------------------------------------
// Purpose: gets the position of the current character in the stream
// Output : std::streampos
//-----------------------------------------------------------------------------
std::streamoff BinaryIO::TellGet()
{
	assert(IsReadMode());
	if (m_cachedReadData)
		return m_cachedReadOffset;

	return m_stream.tellg();
}
std::streamoff BinaryIO::TellPut()
{
	assert(IsWriteMode());
	return m_stream.tellp();
}

//-----------------------------------------------------------------------------
// Purpose: sets the position of the current character in the stream
// Input  : offset - 
//			way - 
//-----------------------------------------------------------------------------
void BinaryIO::SeekGet(const std::streamoff offset, const std::ios_base::seekdir way)
{
	assert(IsReadMode());
	if (m_cachedReadData)
	{
		switch (way)
		{
		case std::ios::beg: m_cachedReadOffset = offset; break;
		case std::ios::cur: m_cachedReadOffset += offset; break;
		case std::ios::end: m_cachedReadOffset = m_size + offset; break;
		default: assert(false); break;
		}
		return;
	}

	m_stream.seekg(offset, way);
}
//-----------------------------------------------------------------------------
// NOTE: if you seek beyond the end of the file to try and pad it out, use the
// Pad() method instead as the behavior of seek is operating system dependent
//-----------------------------------------------------------------------------
void BinaryIO::SeekPut(const std::streamoff offset, const std::ios_base::seekdir way)
{
	assert(IsWriteMode());

	CalcSkipDelta(offset, way);
	m_stream.seekp(offset, way);
}
void BinaryIO::Seek(const std::streamoff offset, const std::ios_base::seekdir way)
{
	if (IsReadMode())
		SeekGet(offset, way);
	if (IsWriteMode())
		SeekPut(offset, way);
}

//-----------------------------------------------------------------------------
// Purpose: returns the data
// Output : std::filebuf*
//-----------------------------------------------------------------------------
const std::filebuf* BinaryIO::GetData() const
{
	if (m_cachedReadData)
		return nullptr;

	return m_stream.rdbuf();
}

//-----------------------------------------------------------------------------
// Purpose: returns the data size
// Output : std::streampos
//-----------------------------------------------------------------------------
const std::streamoff BinaryIO::GetSize() const
{
	return m_size;
}

bool BinaryIO::IsReadMode() const
{
	return (m_flags & std::ios::in);
}

bool BinaryIO::IsWriteMode() const
{
	return (m_flags & std::ios::out);
}

//-----------------------------------------------------------------------------
// Purpose: checks if we are able to read the file
// Output : true on success, false otherwise
//-----------------------------------------------------------------------------
bool BinaryIO::IsReadable() const
{
	if (m_cachedReadData)
		return IsReadMode() && m_cachedReadOffset >= 0 && m_cachedReadOffset < m_size;

	if (!IsReadMode() || !m_stream || m_stream.eof())
		return false;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: checks if we are able to write to file
// Output : true on success, false otherwise
//-----------------------------------------------------------------------------
bool BinaryIO::IsWritable() const
{
	if (!IsWriteMode() || !m_stream)
		return false;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: checks if we hit the end of file
// Output : true on success, false otherwise
//-----------------------------------------------------------------------------
bool BinaryIO::IsEof() const
{
	if (m_cachedReadData)
		return m_cachedReadOffset >= m_size;

	return m_stream.eof();
}

bool BinaryIO::ReadBytes(void* const value, const size_t size)
{
	if (size == 0)
		return true;

	if (!IsReadable())
		return false;

	if (!m_cachedReadData)
	{
		m_stream.read(reinterpret_cast<char*>(value), size);
		return m_stream.good() || (m_stream.eof() && m_stream.gcount() == static_cast<std::streamsize>(size));
	}

	const size_t offset = static_cast<size_t>(m_cachedReadOffset);
	const size_t available = m_cachedReadData->size() - offset;
	if (size > available)
	{
		if (available > 0)
			memcpy(value, m_cachedReadData->data() + offset, available);

		m_cachedReadOffset = m_size;
		return false;
	}

	memcpy(value, m_cachedReadData->data() + offset, size);
	m_cachedReadOffset += static_cast<std::streamoff>(size);
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: reads a string from the file
// Input  : &svOut - 
// Output : true on success, false otherwise
//-----------------------------------------------------------------------------
bool BinaryIO::ReadString(std::string& out)
{
	if (!IsReadable())
		return false;

	while (!IsEof())
	{
		const char c = Read<char>();

		if (c == '\0')
			break;

		out += c;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: reads a string from the file into a fixed size buffer
// Input  : *buf - 
//			len - 
// Output : true on success, false otherwise
//-----------------------------------------------------------------------------
bool BinaryIO::ReadString(char* const buf, const size_t len)
{
	if (!IsReadable())
		return false;

	size_t i = 0;

	while (i < len && !IsEof())
	{
		const char c = Read<char>();

		if (c == '\0')
			break;

		buf[i++] = c;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: writes a string to the file
// Input  : &input - 
// Output : true on success, false otherwise
//-----------------------------------------------------------------------------
bool BinaryIO::WriteString(const std::string& input, const bool nullterminate)
{
	if (!IsWritable())
		return false;

	const char* const text = input.c_str();
	const size_t len = input.length() + nullterminate;

	m_stream.write(text, len);
	CalcAddDelta(len);

	return true;
}

// limit number of io calls and allocations by just using this static buffer
// for padding out the stream.
static constexpr size_t PAD_BUF_SIZE = 4096;
const static char s_padBuf[PAD_BUF_SIZE];

//-----------------------------------------------------------------------------
// Purpose: pads the out stream up to count bytes
// Input  : count - 
//-----------------------------------------------------------------------------
void BinaryIO::Pad(const size_t count)
{
	assert(count > 0);
	size_t remainder = count;

	while (remainder)
	{
		const size_t writeCount = (std::min)(remainder, PAD_BUF_SIZE);
		Write(s_padBuf, writeCount);

		remainder -= writeCount;
	}
}

//-----------------------------------------------------------------------------
// Purpose: makes sure that the size gets incremented if we exceeded the end of
//          the stream with the delta amount
//-----------------------------------------------------------------------------
void BinaryIO::CalcAddDelta(const size_t count)
{
	if (m_skip > 0)
	{
		m_skip -= count;

		if (m_skip < 0)
		{
			m_size += -m_skip; // Add the overshoot to the file size.
			m_skip = 0;
		}
	}
	else
		m_size += count;
}

//-----------------------------------------------------------------------------
// Purpose: if we seek backwards, and then write new data, we should not add
//          this to the total output size of the stream as we modify and not
//          add. we have to keep by how much we shifted backwards and advanced
//          forward until we can start adding again.
//-----------------------------------------------------------------------------
void BinaryIO::CalcSkipDelta(const std::streamoff offset, const std::ios_base::seekdir way)
{
	switch (way)
	{
	case std::ios_base::beg:
	{
		if (offset < 0)
		{
			assert(false && "Negative offset in std::ios_base::beg is invalid.");
			return;
		}

		if (offset > m_size)
		{
			m_size = offset;
			m_skip = 0;
		}
		else
			m_skip = m_size - offset;
		break;
	}
	case std::ios_base::cur:
	{
		if (offset > 0)
			CalcAddDelta(offset);
		else
			m_skip += -offset;
		break;
	}
	case std::ios_base::end:
	{
		if (offset >= 0)
		{
			m_size += offset;
			m_skip = 0;
		}
		else
			m_skip += -offset;
		break;
	}
	default:
		assert(false && "Unsupported seek direction.");
		break;
	}

	// Ensure m_skip is non-negative, this can happen if you call this method
	// with cur or end, and a negative value who's absolute value is greater
	// than the total stream size. If you hit this, you have a bug somewhere.
	assert(m_skip >= 0);

	if (m_skip < 0)
		m_skip = 0;
}
