#pragma once

#include "public/rpak.h"

#define AUDIO_SOURCE_VERSION 1
#define AUDIO_EVENT_VERSION 4
#define AUDIO_EVENT_STREAM_PATH_MAX 260

#define AUDIO_SOURCE_MAGIC MAKE_FOURCC('R', 'S', 'A', 'S')
#define AUDIO_EVENT_MAGIC  MAKE_FOURCC('R', 'S', 'A', 'E')

#define AUDIO_SOURCE_FLAG_STORED_WAV (1u << 0)

#define AUDIO_EVENT_MODE_PLAY         0u
#define AUDIO_EVENT_MODE_STOP_EVENTS  1u
#define AUDIO_EVENT_MODE_STOP_MUSIC   2u
#define AUDIO_EVENT_MODE_STOP_ALL     3u
#define AUDIO_EVENT_MODE_STOP_MANAGED 4u

#define AUDIO_EVENT_FLAG_MANAGED            (1u << 0)
#define AUDIO_EVENT_FLAG_MUSIC              (1u << 1)
#define AUDIO_EVENT_FLAG_REPLACE_SAME_EVENT (1u << 2)

#pragma pack(push, 1)
struct AudioSourceAssetHeader_v1_t
{
	uint32_t magic = AUDIO_SOURCE_MAGIC;
	uint32_t version = AUDIO_SOURCE_VERSION;

	PakGuid_t sourceGuid = 0;
	PagePtr_t name;
	PagePtr_t virtualName;

	int64_t streamOffset : 52 = -1;
	int64_t streamIndex : 12 = -1;
	uint64_t streamSize = 0;

	uint32_t sampleRate = 0;
	uint32_t sampleCount = 0;
	uint16_t channels = 0;
	uint16_t bitsPerSample = 0;
	uint16_t formatTag = 0;
	uint16_t blockAlign = 0;
	uint32_t averageBytesPerSecond = 0;
	uint32_t flags = AUDIO_SOURCE_FLAG_STORED_WAV;
};
static_assert(sizeof(AudioSourceAssetHeader_v1_t) == 72);

struct AudioEventAssetHeader_v2_t
{
	uint32_t magic = AUDIO_EVENT_MAGIC;
	uint32_t version = AUDIO_EVENT_VERSION;

	PakGuid_t eventGuid = 0;
	PagePtr_t eventName;
	PagePtr_t sourceGuids;

	uint32_t sourceCount = 0;
	float volume = 1.0f;
	float pitch = 1.0f;
	uint32_t mode = 0;
	uint32_t flags = 0;

	PakGuid_t firstSourceGuid = 0;
	int64_t firstSourceStreamOffset : 52 = -1;
	int64_t firstSourceStreamIndex : 12 = -1;
	uint64_t firstSourceStreamSize = 0;
	uint32_t firstSourceSampleRate = 0;
	uint32_t firstSourceSampleCount = 0;
	uint16_t firstSourceChannels = 0;
	uint16_t firstSourceBitsPerSample = 0;
	uint16_t firstSourceFormatTag = 0;
	uint16_t firstSourceBlockAlign = 0;
	uint32_t firstSourceAverageBytesPerSecond = 0;
	uint32_t firstSourceFlags = 0;
	char firstSourceStreamPath[AUDIO_EVENT_STREAM_PATH_MAX];
};
static_assert(sizeof(AudioEventAssetHeader_v2_t) == 360);
#pragma pack(pop)
