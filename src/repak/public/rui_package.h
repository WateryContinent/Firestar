#pragma once
#include "public/ui.h"

#define RUI_PACKAGE_MAGIC ('R' | ('U' << 8) | ('I' << 16) | ('P' << 24))
#define RUI_PACKAGE_VERSION_1 1
#define RUI_PACKAGE_VERSION_2 2

#pragma pack(push, 1)
struct RuiPackageHeader_v1_t {
	uint32_t magic;
	uint16_t packageVersion;
	uint16_t ruiVersion;
	uint64_t nameOffset;
	float elementWidth;
	float elementHeight;
	float elementWidthRcp;
	float elementHeightRcp;
	uint16_t defaultValuesSize;
	uint16_t dataStructSize;
	uint16_t styleDescriptorCount;
	uint16_t unk_A4;//unused in r2
	uint16_t renderJobCount;
	uint16_t argClusterCount;
	uint16_t argCount;
	uint16_t keyframingCount;
	uint16_t transformDataSize;
	uint16_t nameSize;
	uint16_t rpakPointersInDefaltDataCount;
	uint8_t pad[2];
	uint32_t argNamesSize;
	uint32_t renderJobSize;
	uint32_t keyframingSize;
	uint32_t defaultStringsSize;

	uint64_t argNamesOffset;//debug only
	uint64_t argClusterOffset;
	uint64_t argumentsOffset;
	uint64_t styleDescriptorOffset;
	uint64_t renderJobOffset;
	uint64_t keyframingOffset;
	uint64_t transformDataOffset;
	uint64_t defaultValuesOffset;
	uint64_t defaultStringDataOffset;
	uint64_t rpakPointersInDefaultDataOffset;
	uint64_t defaultStringsDataSize;
};

struct RuiPackageHeader_v2_t {
	uint32_t magic;
	uint16_t packageVersion;
	uint16_t ruiVersion;
	uint64_t nameOffset;
	float elementWidth;
	float elementHeight;
	float elementWidthRcp;
	float elementHeightRcp;
	uint16_t defaultValuesSize;
	uint16_t dataStructSize;
	uint16_t styleDescriptorCount;
	uint16_t unk_A4;
	uint16_t renderJobCount;
	uint16_t argClusterCount;
	uint16_t argCount;
	uint16_t keyframingCount;
	uint16_t transformDataSize;
	uint16_t nameSize;
	uint16_t rpakPointersInDefaltDataCount;
	uint8_t pad[2];
	uint32_t argNamesSize;
	uint32_t renderJobSize;
	uint32_t keyframingSize;
	uint32_t defaultStringsSize;

	uint64_t argNamesOffset;
	uint64_t argClusterOffset;
	uint64_t argumentsOffset;
	uint64_t styleDescriptorOffset;
	uint64_t renderJobOffset;
	uint64_t keyframingOffset;
	uint64_t transformDataOffset;
	uint64_t defaultValuesOffset;
	uint64_t defaultStringDataOffset;
	uint64_t rpakPointersInDefaultDataOffset;
	uint64_t defaultStringsDataSize;

	uint32_t pointerFixupCount;
	uint64_t pointerFixupOffset;
};

struct RuiPackagePointerFixup_t {
	uint32_t srcSection;
	uint32_t srcOffset;
	uint32_t dstSection;
	uint32_t dstOffset;
};
#pragma pack(pop)

static_assert(sizeof(RuiPackageHeader_v1_t) == 160);
static_assert(sizeof(RuiPackageHeader_v2_t) == 172);
static_assert(sizeof(RuiPackagePointerFixup_t) == 16);

enum RuiPackageSection_t : uint32_t {
	RUI_PACKAGE_SECTION_DEFAULT_VALUES = 1,
	RUI_PACKAGE_SECTION_KEYFRAMING = 2,
};

struct RuiPackage {
	

	RuiPackage(const fs::path& inputPath) {
		FILE* f = NULL;
		errno_t errorCode = fopen_s(&f, inputPath.string().c_str(), "rb");
		if (errorCode == 0) {
			fread(&hdr, sizeof(RuiPackageHeader_v1_t), 1, f);
			if(hdr.magic != RUI_PACKAGE_MAGIC)
				Error("Attempted to load an invalid RUIP file (expected magic %x, got %x).\n", RUI_PACKAGE_MAGIC, hdr.magic);
			if(hdr.packageVersion == RUI_PACKAGE_VERSION_2)
				fread(reinterpret_cast<char*>(&hdr) + sizeof(RuiPackageHeader_v1_t),
					sizeof(RuiPackageHeader_v2_t) - sizeof(RuiPackageHeader_v1_t), 1, f);
			else if(hdr.packageVersion != RUI_PACKAGE_VERSION_1)
				Error("Attempted to load an unsupported RUIP file (expected version %u or %u, got %u).\n",
					RUI_PACKAGE_VERSION_1, RUI_PACKAGE_VERSION_2, hdr.packageVersion);

			fseek(f,(long)hdr.nameOffset,0);
			name.resize(hdr.nameSize);
			fread(name.data(), 1, hdr.nameSize, f);

			fseek(f, (long)hdr.argNamesOffset, 0);
			argNames.resize(hdr.argNamesSize);
			fread(argNames.data(), 1, hdr.argNamesSize, f);

			fseek(f,(long)hdr.defaultValuesOffset,0);
			defaultData.resize(hdr.defaultValuesSize);
			fread(defaultData.data(),1,hdr.defaultValuesSize,f);

			fseek(f,(long)hdr.defaultStringDataOffset,0);
			defaultStrings.resize(hdr.defaultStringsDataSize);
			fread(defaultStrings.data(),1,hdr.defaultStringsDataSize,f);

			fseek(f,(long)hdr.rpakPointersInDefaultDataOffset,0);
			defaultStringOffsets.resize(hdr.rpakPointersInDefaltDataCount);
			fread(defaultStringOffsets.data(),sizeof(uint16_t),hdr.rpakPointersInDefaltDataCount,f);

			fseek(f,(long)hdr.styleDescriptorOffset,0);
			const size_t styleDescriptorSize = hdr.ruiVersion >= R5_UI_VERSION
				? sizeof(StyleDescriptor_v40_s)
				: sizeof(StyleDescriptor_v30_s);
			styleDescriptors.resize(hdr.styleDescriptorCount*styleDescriptorSize);
			fread(styleDescriptors.data(),styleDescriptorSize,hdr.styleDescriptorCount,f);

			fseek(f,(long)hdr.renderJobOffset,0);
			renderJobs.resize(hdr.renderJobSize);
			fread(renderJobs.data(),1,hdr.renderJobSize,f);

			fseek(f,(long)hdr.transformDataOffset,0);
			transformData.resize(hdr.transformDataSize);
			fread(transformData.data(),1,hdr.transformDataSize,f);

			fseek(f, (long)hdr.keyframingOffset, 0);
			keyframingData.resize(hdr.keyframingSize);
			fread(keyframingData.data(), 1, hdr.keyframingSize, f);

			if (hdr.packageVersion == RUI_PACKAGE_VERSION_2 && hdr.pointerFixupCount) {
				fseek(f, (long)hdr.pointerFixupOffset, 0);
				pointerFixups.resize(hdr.pointerFixupCount);
				fread(pointerFixups.data(), sizeof(RuiPackagePointerFixup_t), hdr.pointerFixupCount, f);
			}

			fseek(f,(long)hdr.argumentsOffset,0);
			arguments.resize(hdr.argCount);
			fread(arguments.data(),sizeof(Argument_s),hdr.argCount,f);

			fseek(f,(long)hdr.argClusterOffset,0);
			argCluster.resize(hdr.argClusterCount);
			fread(argCluster.data(),sizeof(ArgCluster_s),hdr.argClusterCount,f);

			fclose(f);
		}
		else {
			Error("Could not open ruip file %s with error %x",inputPath.string().c_str(),errorCode);
		}
	}

	RuiHeader_v30_s CreateRuiHeader_v30() {
		RuiHeader_v30_s ruiHdr;

		ruiHdr.elementWidth = hdr.elementWidth;
		ruiHdr.elementHeight = hdr.elementHeight;
		ruiHdr.elementWidthRcp = hdr.elementWidthRcp;
		ruiHdr.elementHeightRcp = hdr.elementHeightRcp;

		ruiHdr.argumentCount = hdr.argCount;
		ruiHdr.keyframingCount = hdr.keyframingCount;
		ruiHdr.dataStructSize = hdr.dataStructSize;
		ruiHdr.dataStructInitSize = hdr.defaultValuesSize;
		ruiHdr.styleDescriptorCount = hdr.styleDescriptorCount;
		ruiHdr.maxTransformIndex = hdr.unk_A4;
		ruiHdr.renderJobCount = hdr.renderJobCount;
		ruiHdr.argClusterCount = hdr.argClusterCount;

		return ruiHdr;
	}

	uint16_t GetRuiVersion() const {
		if (hdr.ruiVersion >= R5_UI_VERSION)
			return R5_UI_VERSION;

		return hdr.ruiVersion ? hdr.ruiVersion : R2_UI_VERSION;
	}

	uint16_t GetSourceRuiVersion() const {
		return hdr.ruiVersion ? hdr.ruiVersion : R2_UI_VERSION;
	}

	RuiPackageHeader_v2_t hdr{};

	std::vector<char> name;
	std::vector<char> argNames;
	std::vector<char> defaultData;
	std::vector<char> defaultStrings;
	std::vector<uint16_t> defaultStringOffsets;
	std::vector<char> transformData;
	std::vector<char> keyframingData;
	std::vector<char> renderJobs;
	std::vector<Argument_s> arguments;
	std::vector<ArgCluster_s> argCluster;
	std::vector<char> styleDescriptors;
	std::vector<RuiPackagePointerFixup_t> pointerFixups;
};
