#include "pch.h"
#include "assets/assets.h"
#include "public/audio.h"

#include <algorithm>
#include <cctype>
#include <limits>

#define STB_VORBIS_HEADER_ONLY
#pragma warning(push, 2)
#include "thirdparty/stb/stb_vorbis.c"
#pragma warning(pop)
#undef STB_VORBIS_HEADER_ONLY

namespace
{
	constexpr uint32_t RIFF_MAGIC = MAKE_FOURCC('R', 'I', 'F', 'F');
	constexpr uint32_t WAVE_MAGIC = MAKE_FOURCC('W', 'A', 'V', 'E');
	constexpr uint32_t FMT_MAGIC = MAKE_FOURCC('f', 'm', 't', ' ');
	constexpr uint32_t DATA_MAGIC = MAKE_FOURCC('d', 'a', 't', 'a');
	constexpr uint16_t WAV_FORMAT_PCM = 0x0001;
	constexpr uint16_t WAV_FORMAT_IEEE_FLOAT = 0x0003;
	constexpr uint16_t OGG_DECODE_BITS_PER_SAMPLE = 16;

	struct ParsedWavInfo_s
	{
		uint16_t formatTag = 0;
		uint16_t channels = 0;
		uint32_t sampleRate = 0;
		uint32_t averageBytesPerSecond = 0;
		uint16_t blockAlign = 0;
		uint16_t bitsPerSample = 0;
		uint32_t sampleCount = 0;
	};

	template <typename T>
	T ReadLE(const std::vector<uint8_t>& bytes, const size_t offset)
	{
		T out{};
		memcpy(&out, bytes.data() + offset, sizeof(T));
		return out;
	}

	std::vector<uint8_t> ReadWholeFile(const std::string& path, const char* const debugName)
	{
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (!input.is_open())
			Error("Failed to open WAV file \"%s\" for audio source \"%s\".\n", path.c_str(), debugName);

		const std::streamoff fileSize = input.tellg();
		if (fileSize <= 0)
			Error("WAV file \"%s\" for audio source \"%s\" is empty.\n", path.c_str(), debugName);

		std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
		input.seekg(0, std::ios::beg);
		input.read(reinterpret_cast<char*>(bytes.data()), fileSize);

		if (!input)
			Error("Failed while reading WAV file \"%s\" for audio source \"%s\".\n", path.c_str(), debugName);

		return bytes;
	}

	std::string GetLowerExtension(const std::string& path)
	{
		std::string extension = std::filesystem::path(path).extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(),
			[](const unsigned char c) { return static_cast<char>(std::tolower(c)); });

		return extension;
	}

	template <typename T>
	void AppendLE(std::vector<uint8_t>& bytes, const T value)
	{
		const size_t offset = bytes.size();
		bytes.resize(offset + sizeof(T));
		memcpy(bytes.data() + offset, &value, sizeof(T));
	}

	void AppendBytes(std::vector<uint8_t>& bytes, const void* const data, const size_t size)
	{
		const size_t offset = bytes.size();
		bytes.resize(offset + size);
		memcpy(bytes.data() + offset, data, size);
	}

	std::vector<uint8_t> BuildPcm16WavBytes(const std::vector<int16_t>& pcmSamples, const uint32_t sampleRate,
		const uint32_t channels, const char* const path)
	{
		if (sampleRate == 0 || channels == 0)
			Error("Decoded OGG file \"%s\" has invalid audio format values.\n", path);

		if (channels > (std::numeric_limits<uint16_t>::max)())
			Error("Decoded OGG file \"%s\" has too many channels (%u).\n", path, channels);

		const uint64_t blockAlign64 = static_cast<uint64_t>(channels) * (OGG_DECODE_BITS_PER_SAMPLE / 8u);
		if (blockAlign64 > (std::numeric_limits<uint16_t>::max)())
			Error("Decoded OGG file \"%s\" has an unsupported block alignment.\n", path);

		const uint64_t byteRate64 = static_cast<uint64_t>(sampleRate) * blockAlign64;
		if (byteRate64 > (std::numeric_limits<uint32_t>::max)())
			Error("Decoded OGG file \"%s\" has an unsupported byte rate.\n", path);

		const uint64_t dataSize64 = static_cast<uint64_t>(pcmSamples.size()) * sizeof(int16_t);
		if (dataSize64 > (std::numeric_limits<uint32_t>::max)())
			Error("Decoded OGG file \"%s\" is too large to wrap as RIFF/WAVE.\n", path);

		const uint64_t riffSize64 = 36ull + dataSize64;
		if (riffSize64 > (std::numeric_limits<uint32_t>::max)())
			Error("Decoded OGG file \"%s\" is too large to wrap as RIFF/WAVE.\n", path);

		const uint32_t dataSize = static_cast<uint32_t>(dataSize64);
		const uint32_t riffSize = static_cast<uint32_t>(riffSize64);
		const uint16_t channels16 = static_cast<uint16_t>(channels);
		const uint16_t blockAlign = static_cast<uint16_t>(blockAlign64);
		const uint32_t byteRate = static_cast<uint32_t>(byteRate64);

		std::vector<uint8_t> wavBytes;
		wavBytes.reserve(44ull + dataSize);

		AppendLE<uint32_t>(wavBytes, RIFF_MAGIC);
		AppendLE<uint32_t>(wavBytes, riffSize);
		AppendLE<uint32_t>(wavBytes, WAVE_MAGIC);
		AppendLE<uint32_t>(wavBytes, FMT_MAGIC);
		AppendLE<uint32_t>(wavBytes, 16u);
		AppendLE<uint16_t>(wavBytes, WAV_FORMAT_PCM);
		AppendLE<uint16_t>(wavBytes, channels16);
		AppendLE<uint32_t>(wavBytes, sampleRate);
		AppendLE<uint32_t>(wavBytes, byteRate);
		AppendLE<uint16_t>(wavBytes, blockAlign);
		AppendLE<uint16_t>(wavBytes, OGG_DECODE_BITS_PER_SAMPLE);
		AppendLE<uint32_t>(wavBytes, DATA_MAGIC);
		AppendLE<uint32_t>(wavBytes, dataSize);

		if (dataSize != 0)
			AppendBytes(wavBytes, pcmSamples.data(), static_cast<size_t>(dataSize));

		return wavBytes;
	}

	std::vector<uint8_t> DecodeOggFileToWav(const std::string& path, const char* const debugName)
	{
		int vorbisError = 0;
		stb_vorbis* const vorbis = stb_vorbis_open_filename(path.c_str(), &vorbisError, nullptr);
		if (!vorbis)
			Error("Failed to decode OGG/Vorbis file \"%s\" for audio source \"%s\" (stb_vorbis error %d).\n",
				path.c_str(), debugName, vorbisError);

		const stb_vorbis_info info = stb_vorbis_get_info(vorbis);
		if (info.channels <= 0 || info.sample_rate <= 0)
		{
			stb_vorbis_close(vorbis);
			Error("Decoded OGG/Vorbis file \"%s\" for audio source \"%s\" has invalid output format.\n",
				path.c_str(), debugName);
		}

		constexpr int chunkFrames = 4096;
		if (info.channels > (std::numeric_limits<uint16_t>::max)() ||
			info.channels > ((std::numeric_limits<int>::max)() / chunkFrames))
		{
			stb_vorbis_close(vorbis);
			Error("Decoded OGG/Vorbis file \"%s\" for audio source \"%s\" has too many channels (%d).\n",
				path.c_str(), debugName, info.channels);
		}

		const uint32_t channels = static_cast<uint32_t>(info.channels);
		const uint32_t sampleRate = static_cast<uint32_t>(info.sample_rate);
		std::vector<int16_t> pcmSamples;
		std::vector<int16_t> chunk(static_cast<size_t>(chunkFrames) * channels);

		for (;;)
		{
			const int framesRead = stb_vorbis_get_samples_short_interleaved(vorbis, info.channels, chunk.data(),
				static_cast<int>(chunk.size()));

			if (framesRead == 0)
				break;

			if (framesRead < 0)
			{
				stb_vorbis_close(vorbis);
				Error("Failed while decoding OGG/Vorbis file \"%s\" for audio source \"%s\".\n",
					path.c_str(), debugName);
			}

			const uint64_t sampleCount64 = static_cast<uint64_t>(framesRead) * channels;
			if (sampleCount64 > (std::numeric_limits<size_t>::max)())
			{
				stb_vorbis_close(vorbis);
				Error("Decoded OGG/Vorbis file \"%s\" for audio source \"%s\" is too large.\n", path.c_str(), debugName);
			}

			pcmSamples.insert(pcmSamples.end(), chunk.begin(), chunk.begin() + static_cast<size_t>(sampleCount64));
		}

		stb_vorbis_close(vorbis);

		if (pcmSamples.empty())
			Error("Decoded OGG/Vorbis file \"%s\" for audio source \"%s\" produced no samples.\n", path.c_str(), debugName);

		return BuildPcm16WavBytes(pcmSamples, sampleRate, channels, path.c_str());
	}

	void ValidateAndParseWav(const std::vector<uint8_t>& bytes, const char* const path, ParsedWavInfo_s& outInfo)
	{
		if (bytes.size() < 12)
			Error("WAV file \"%s\" is too small to contain a RIFF/WAVE header.\n", path);

		if (ReadLE<uint32_t>(bytes, 0) != RIFF_MAGIC || ReadLE<uint32_t>(bytes, 8) != WAVE_MAGIC)
			Error("Audio source \"%s\" must be a RIFF/WAVE .wav file.\n", path);

		bool foundFmt = false;
		bool foundData = false;
		uint32_t dataSize = 0;

		size_t cursor = 12;
		while (cursor + 8 <= bytes.size())
		{
			const uint32_t chunkId = ReadLE<uint32_t>(bytes, cursor);
			const uint32_t chunkSize = ReadLE<uint32_t>(bytes, cursor + 4);
			const size_t chunkData = cursor + 8;
			const size_t chunkEnd = chunkData + chunkSize;

			if (chunkEnd > bytes.size())
				Error("WAV file \"%s\" has a truncated RIFF chunk.\n", path);

			if (chunkId == FMT_MAGIC)
			{
				if (chunkSize < 16)
					Error("WAV file \"%s\" has an invalid fmt chunk.\n", path);

				outInfo.formatTag = ReadLE<uint16_t>(bytes, chunkData);
				outInfo.channels = ReadLE<uint16_t>(bytes, chunkData + 2);
				outInfo.sampleRate = ReadLE<uint32_t>(bytes, chunkData + 4);
				outInfo.averageBytesPerSecond = ReadLE<uint32_t>(bytes, chunkData + 8);
				outInfo.blockAlign = ReadLE<uint16_t>(bytes, chunkData + 12);
				outInfo.bitsPerSample = ReadLE<uint16_t>(bytes, chunkData + 14);
				foundFmt = true;
			}
			else if (chunkId == DATA_MAGIC)
			{
				dataSize = chunkSize;
				foundData = true;
			}

			cursor = chunkEnd + (chunkSize & 1);
		}

		if (!foundFmt || !foundData)
			Error("WAV file \"%s\" must contain both fmt and data chunks.\n", path);

		if (outInfo.formatTag != WAV_FORMAT_PCM && outInfo.formatTag != WAV_FORMAT_IEEE_FLOAT)
			Error("WAV file \"%s\" uses unsupported format tag 0x%04X; use PCM or IEEE float WAV.\n", path, outInfo.formatTag);

		if (outInfo.channels == 0 || outInfo.sampleRate == 0 || outInfo.bitsPerSample == 0 ||
			(outInfo.bitsPerSample % 8) != 0)
			Error("WAV file \"%s\" has invalid audio format values.\n", path);

		if (outInfo.formatTag == WAV_FORMAT_IEEE_FLOAT && outInfo.bitsPerSample != 32)
			Error("WAV file \"%s\" uses unsupported %u-bit IEEE float samples; use 32-bit float WAV.\n",
				path, outInfo.bitsPerSample);
		if (outInfo.formatTag == WAV_FORMAT_PCM && outInfo.bitsPerSample != 8 &&
			outInfo.bitsPerSample != 16 && outInfo.bitsPerSample != 24 &&
			outInfo.bitsPerSample != 32)
		{
			Error("WAV file \"%s\" uses unsupported %u-bit PCM samples.\n", path, outInfo.bitsPerSample);
		}

		const uint64_t bytesPerSample = outInfo.bitsPerSample / 8u;
		const uint64_t expectedBlockAlign = static_cast<uint64_t>(outInfo.channels) * bytesPerSample;
		const uint64_t expectedAverageBytes =
			static_cast<uint64_t>(outInfo.sampleRate) * expectedBlockAlign;
		if (expectedBlockAlign == 0 || expectedBlockAlign > (std::numeric_limits<uint16_t>::max)() ||
			expectedAverageBytes > (std::numeric_limits<uint32_t>::max)())
		{
			Error("WAV file \"%s\" has audio format values that exceed packed-source limits.\n", path);
		}

		if (outInfo.blockAlign != expectedBlockAlign ||
			outInfo.averageBytesPerSecond != expectedAverageBytes)
		{
			Warning("WAV file \"%s\" has inconsistent frame metadata; normalizing block align %u->%llu and byte rate %u->%llu.\n",
				path,
				outInfo.blockAlign,
				static_cast<unsigned long long>(expectedBlockAlign),
				outInfo.averageBytesPerSecond,
				static_cast<unsigned long long>(expectedAverageBytes));
		}

		outInfo.blockAlign = static_cast<uint16_t>(expectedBlockAlign);
		outInfo.averageBytesPerSecond = static_cast<uint32_t>(expectedAverageBytes);
		outInfo.sampleCount = dataSize / outInfo.blockAlign;
	}

	std::vector<uint8_t> LoadAudioSourceAsWav(CPakFileBuilder* const pak, const char* const assetPath,
		const rapidjson::Value& mapEntry, std::string& outSourcePath, ParsedWavInfo_s& outInfo)
	{
		const char* inputOverride = JSON_GetValueOrDefault(mapEntry, "wav", static_cast<const char*>(nullptr));
		if (!inputOverride)
			inputOverride = JSON_GetValueOrDefault(mapEntry, "ogg", static_cast<const char*>(nullptr));
		if (!inputOverride)
			inputOverride = JSON_GetValueOrDefault(mapEntry, "file", static_cast<const char*>(nullptr));

		outSourcePath = inputOverride
			? pak->GetAssetPath() + inputOverride
			: Utils::ChangeExtension(pak->GetAssetPath() + assetPath, ".wav");

		std::vector<uint8_t> wavBytes;
		const std::string extension = GetLowerExtension(outSourcePath);
		if (extension == ".ogg" || extension == ".oga")
			wavBytes = DecodeOggFileToWav(outSourcePath, assetPath);
		else
			wavBytes = ReadWholeFile(outSourcePath, assetPath);

		ValidateAndParseWav(wavBytes, outSourcePath.c_str(), outInfo);
		return wavBytes;
	}

	PakGuid_t ParseAudioSourceGuid(const rapidjson::Value& value, const char* const debugName)
	{
		bool success = false;
		const PakGuid_t guid = Pak_ParseGuid(value, &success);
		if (!success || guid == 0)
			Error("Audio event source entry for \"%s\" must be a non-zero GUID, asset path, or source name.\n", debugName);

		return guid;
	}

	uint32_t ParseAudioEventMode(const rapidjson::Value& mapEntry, const char* const debugName)
	{
		rapidjson::Document::ConstMemberIterator modeIt;
		if (!JSON_GetIterator(mapEntry, "mode", modeIt))
			return AUDIO_EVENT_MODE_PLAY;

		if (modeIt->value.IsUint())
			return modeIt->value.GetUint();

		if (!modeIt->value.IsString())
			Error("Audio event \"%s\" field \"mode\" must be a number or string.\n", debugName);

		const std::string mode(modeIt->value.GetString(), modeIt->value.GetStringLength());
		if (mode == "play")
			return AUDIO_EVENT_MODE_PLAY;
		if (mode == "stop" || mode == "stop_events")
			return AUDIO_EVENT_MODE_STOP_EVENTS;
		if (mode == "stop_music")
			return AUDIO_EVENT_MODE_STOP_MUSIC;
		if (mode == "stop_all")
			return AUDIO_EVENT_MODE_STOP_ALL;
		if (mode == "stop_managed")
			return AUDIO_EVENT_MODE_STOP_MANAGED;

		Error("Audio event \"%s\" has unknown mode \"%s\".\n", debugName, mode.c_str());
		return AUDIO_EVENT_MODE_PLAY;
	}

	bool IsAudioEventControlMode(const uint32_t mode)
	{
		return mode == AUDIO_EVENT_MODE_STOP_EVENTS ||
			mode == AUDIO_EVENT_MODE_STOP_MUSIC ||
			mode == AUDIO_EVENT_MODE_STOP_ALL ||
			mode == AUDIO_EVENT_MODE_STOP_MANAGED;
	}

}

void Assets::AddAudioSourceAsset_v1(CPakFileBuilder* const pak, const PakGuid_t assetGuid, const char* const assetPath, const rapidjson::Value& mapEntry)
{
	ParsedWavInfo_s wavInfo;
	std::string sourcePath;
	std::vector<uint8_t> wavBytes = LoadAudioSourceAsWav(pak, assetPath, mapEntry, sourcePath, wavInfo);

	const size_t wavSize = wavBytes.size();
	const size_t alignedSize = IALIGN(wavBytes.size(), STARPAK_DATABLOCK_ALIGNMENT);
	wavBytes.resize(alignedSize, 0);

	PakStreamSetEntry_s streamData = pak->AddStreamingDataEntry(static_cast<int64_t>(wavBytes.size()), wavBytes.data(), STREAMING_SET_MANDATORY);

	PakAsset_t& asset = pak->BeginAsset(assetGuid, assetPath);
	PakPageLump_s hdrLump = pak->CreatePageLump(sizeof(AudioSourceAssetHeader_v1_t), SF_HEAD, 8);
	AudioSourceAssetHeader_v1_t* const hdr = reinterpret_cast<AudioSourceAssetHeader_v1_t*>(hdrLump.data);
	hdr->magic = AUDIO_SOURCE_MAGIC;
	hdr->version = AUDIO_SOURCE_VERSION;

	const char* const virtualNameOverride = JSON_GetValueOrDefault(mapEntry, "virtualName", static_cast<const char*>(nullptr));
	const std::string virtualName = virtualNameOverride ? virtualNameOverride : Utils::VFormat("rpakwav/%016llx.wav", assetGuid);

	const size_t assetNameLen = strlen(assetPath) + 1;
	const size_t virtualNameLen = virtualName.length() + 1;
	PakPageLump_s dataLump = pak->CreatePageLump(assetNameLen + virtualNameLen, SF_CPU | SF_CLIENT, 8);

	memcpy(dataLump.data, assetPath, assetNameLen);
	memcpy(dataLump.data + assetNameLen, virtualName.c_str(), virtualNameLen);

	hdr->sourceGuid = assetGuid;
	hdr->streamOffset = streamData.streamOffset;
	hdr->streamIndex = streamData.streamIndex;
	hdr->streamSize = wavSize;
	hdr->sampleRate = wavInfo.sampleRate;
	hdr->sampleCount = wavInfo.sampleCount;
	hdr->channels = wavInfo.channels;
	hdr->bitsPerSample = wavInfo.bitsPerSample;
	hdr->formatTag = wavInfo.formatTag;
	hdr->blockAlign = wavInfo.blockAlign;
	hdr->averageBytesPerSecond = wavInfo.averageBytesPerSecond;
	hdr->flags = AUDIO_SOURCE_FLAG_STORED_WAV;

	pak->AddPointer(hdrLump, offsetof(AudioSourceAssetHeader_v1_t, name), dataLump, 0);
	pak->AddPointer(hdrLump, offsetof(AudioSourceAssetHeader_v1_t, virtualName), dataLump, assetNameLen);

	asset.InitAsset(hdrLump.GetPointer(), sizeof(AudioSourceAssetHeader_v1_t), dataLump.GetPointer(), AWSR_VERSION, AssetType::AWSR,
		streamData.streamOffset, streamData.streamIndex);
	asset.SetHeaderPointer(hdrLump.data);

	pak->FinishAsset();
}

void Assets::AddAudioEventAsset_v1(CPakFileBuilder* const pak, const PakGuid_t assetGuid, const char* const assetPath, const rapidjson::Value& mapEntry)
{
	const uint32_t mode = ParseAudioEventMode(mapEntry, assetPath);
	const bool controlMode = IsAudioEventControlMode(mode);
	rapidjson::Document::ConstMemberIterator sourcesIt;
	bool hasSourcesArray = false;

	if (controlMode)
	{
		hasSourcesArray =
			JSON_GetIterator(mapEntry, "targets", JSONFieldType_e::kArray, sourcesIt) ||
			JSON_GetIterator(mapEntry, "sources", JSONFieldType_e::kArray, sourcesIt);
	}
	else
	{
		if (!JSON_GetRequired(mapEntry, "sources", JSONFieldType_e::kArray, sourcesIt))
			Error("Audio event \"%s\" requires a sources array.\n", assetPath);

		hasSourcesArray = true;
	}

	if (!controlMode && sourcesIt->value.GetArray().Empty())
		Error("Audio event \"%s\" requires at least one source.\n", assetPath);

	if (mode == AUDIO_EVENT_MODE_STOP_EVENTS && (!hasSourcesArray || sourcesIt->value.GetArray().Empty()))
		Error("Audio event \"%s\" in stop_events mode requires a non-empty targets array.\n", assetPath);

	std::vector<PakGuid_t> sourceGuids;
	if (hasSourcesArray)
		sourceGuids.reserve(sourcesIt->value.GetArray().Size());

	if (hasSourcesArray)
	{
		const rapidjson::Value::ConstArray sourcesArray = sourcesIt->value.GetArray();
		for (const rapidjson::Value& source : sourcesArray)
			sourceGuids.push_back(ParseAudioSourceGuid(source, assetPath));
	}

	PakAsset_t& asset = pak->BeginAsset(assetGuid, assetPath);
	PakPageLump_s hdrLump = pak->CreatePageLump(sizeof(AudioEventAssetHeader_v2_t), SF_HEAD, 8);
	AudioEventAssetHeader_v2_t* const hdr = reinterpret_cast<AudioEventAssetHeader_v2_t*>(hdrLump.data);
	hdr->magic = AUDIO_EVENT_MAGIC;
	hdr->version = AUDIO_EVENT_VERSION;

	const size_t nameLen = strlen(assetPath) + 1;
	const size_t sourceGuidDataSize = sourceGuids.size() * sizeof(PakGuid_t);

	hdr->eventGuid = assetGuid;
	hdr->sourceCount = static_cast<uint32_t>(sourceGuids.size());
	hdr->volume = JSON_GetNumberOrDefault(mapEntry, "volume", 1.0f);
	hdr->pitch = JSON_GetNumberOrDefault(mapEntry, "pitch", 1.0f);
	hdr->mode = mode;
	hdr->flags = JSON_GetNumberOrDefault(mapEntry, "flags", 0u);
	bool boolFlag = false;
	if (JSON_GetValue(mapEntry, "music", JSONFieldType_e::kBool, boolFlag) && boolFlag)
		hdr->flags |= AUDIO_EVENT_FLAG_MUSIC;
	if (JSON_GetValue(mapEntry, "replaceSameEvent", JSONFieldType_e::kBool, boolFlag) && boolFlag)
		hdr->flags |= AUDIO_EVENT_FLAG_REPLACE_SAME_EVENT;

	PakPageLump_s dataLump = pak->CreatePageLump(sourceGuidDataSize + nameLen, SF_CPU, 8);

	if (sourceGuidDataSize != 0)
		memcpy(dataLump.data, sourceGuids.data(), sourceGuidDataSize);
	memcpy(dataLump.data + sourceGuidDataSize, assetPath, nameLen);

	memset(hdr->firstSourceStreamPath, 0, sizeof(hdr->firstSourceStreamPath));

	if (!controlMode)
	{
		const PakGuid_t firstSourceGuid = sourceGuids[0];
		const PakAsset_t* const firstSourceAsset = pak->GetAssetByGuid(firstSourceGuid, nullptr, true);
		if (!firstSourceAsset || !firstSourceAsset->IsType(TYPE_AWSR) || !firstSourceAsset->header)
			Error("Audio event \"%s\" references source GUID 0x%llX before a matching awsr source asset was added.\n", assetPath, firstSourceGuid);

		const AudioSourceAssetHeader_v1_t* const firstSourceHdr = reinterpret_cast<const AudioSourceAssetHeader_v1_t*>(firstSourceAsset->header);
		if (firstSourceHdr->magic != AUDIO_SOURCE_MAGIC || firstSourceHdr->version != AUDIO_SOURCE_VERSION)
			Error("Audio event \"%s\" references source GUID 0x%llX, but its awsr header is invalid.\n", assetPath, firstSourceGuid);

		const std::string& streamPath = pak->GetStreamingFilePath(STREAMING_SET_MANDATORY, static_cast<size_t>(firstSourceHdr->streamIndex));
		if (streamPath.length() >= AUDIO_EVENT_STREAM_PATH_MAX)
			Error("Audio event \"%s\" stream path \"%s\" exceeds the %u byte packed WAV event limit.\n", assetPath, streamPath.c_str(), AUDIO_EVENT_STREAM_PATH_MAX);

		hdr->firstSourceGuid = firstSourceGuid;
		hdr->firstSourceStreamOffset = firstSourceHdr->streamOffset;
		hdr->firstSourceStreamIndex = firstSourceHdr->streamIndex;
		hdr->firstSourceStreamSize = firstSourceHdr->streamSize;
		hdr->firstSourceSampleRate = firstSourceHdr->sampleRate;
		hdr->firstSourceSampleCount = firstSourceHdr->sampleCount;
		hdr->firstSourceChannels = firstSourceHdr->channels;
		hdr->firstSourceBitsPerSample = firstSourceHdr->bitsPerSample;
		hdr->firstSourceFormatTag = firstSourceHdr->formatTag;
		hdr->firstSourceBlockAlign = firstSourceHdr->blockAlign;
		hdr->firstSourceAverageBytesPerSecond = firstSourceHdr->averageBytesPerSecond;
		hdr->firstSourceFlags = firstSourceHdr->flags;
		memcpy(hdr->firstSourceStreamPath, streamPath.c_str(), streamPath.length());
	}

	pak->AddPointer(hdrLump, offsetof(AudioEventAssetHeader_v2_t, sourceGuids), dataLump, 0);
	pak->AddPointer(hdrLump, offsetof(AudioEventAssetHeader_v2_t, eventName), dataLump, sourceGuidDataSize);

	asset.InitAsset(hdrLump.GetPointer(), sizeof(AudioEventAssetHeader_v2_t), dataLump.GetPointer(), AEVT_VERSION, AssetType::AEVT);
	asset.SetHeaderPointer(hdrLump.data);

	pak->FinishAsset();
}
