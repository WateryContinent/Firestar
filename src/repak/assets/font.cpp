#include "pch.h"
#include "assets.h"

#pragma pack(push, 1)
struct FontAssetHeader_v7_t
{
	uint8_t opaquePrefix[0x10];
	PagePtr_t faceData;
	PagePtr_t hashData;
	PakGuid_t atlasGuid;
};
static_assert(sizeof(FontAssetHeader_v7_t) == 0x28);
#pragma pack(pop)

static uint8_t Font_ParseHexNibble(const char value)
{
	if (value >= '0' && value <= '9')
		return static_cast<uint8_t>(value - '0');
	if (value >= 'A' && value <= 'F')
		return static_cast<uint8_t>(value - 'A' + 10);
	if (value >= 'a' && value <= 'f')
		return static_cast<uint8_t>(value - 'a' + 10);

	Error("Font headerPrefixHex contains a non-hexadecimal character.\n");
}

void Assets::AddFontAsset_v7(CPakFileBuilder* const pak, const PakGuid_t assetGuid, const char* const assetPath, const rapidjson::Value& mapEntry)
{
	const char* const prefixHex = JSON_GetValueRequired<const char*>(mapEntry, "headerPrefixHex");
	if (strlen(prefixHex) != 0x20)
		Error("Font headerPrefixHex must contain exactly 32 hex characters.\n");

	const uint32_t faceOffset = JSON_GetNumberRequired<uint32_t>(mapEntry, "faceOffset");
	const uint32_t hashOffset = JSON_GetNumberRequired<uint32_t>(mapEntry, "hashOffset");
	const char* const atlasPath = JSON_GetValueRequired<const char*>(mapEntry, "atlas");
	const PakGuid_t atlasGuid = RTech::GetAssetGUIDFromString(atlasPath, false);
	rapidjson::Value::ConstMemberIterator internalPointersIt;
	JSON_GetRequired(mapEntry, "internalPointers", JSONFieldType_e::kArray, internalPointersIt);
	const rapidjson::Value::ConstArray internalPointers = internalPointersIt->value.GetArray();

	const std::string sourcePath = Utils::ChangeExtension(pak->GetAssetPath() + assetPath, ".bin");
	BinaryIO sourceFile;
	if (!sourceFile.Open(sourcePath, BinaryIO::Mode_e::Read))
		Error("Failed to open font data asset \"%s\".\n", sourcePath.c_str());

	const size_t sourceSize = sourceFile.GetSize();
	if (!sourceSize || faceOffset >= sourceSize || hashOffset >= sourceSize)
		Error("Font data offsets are outside \"%s\".\n", sourcePath.c_str());

	PakAsset_t& asset = pak->BeginAsset(assetGuid, assetPath);
	PakPageLump_s headerLump = pak->CreatePageLump(sizeof(FontAssetHeader_v7_t), SF_HEAD | SF_CLIENT, 8);
	PakPageLump_s dataLump = pak->CreatePageLump(sourceSize, SF_CPU | SF_CLIENT, 8);
	sourceFile.Read(dataLump.data, sourceSize);

	FontAssetHeader_v7_t* const header = reinterpret_cast<FontAssetHeader_v7_t*>(headerLump.data);
	for (size_t index = 0; index < ARRAYSIZE(header->opaquePrefix); ++index)
	{
		const uint8_t high = Font_ParseHexNibble(prefixHex[index * 2]);
		const uint8_t low = Font_ParseHexNibble(prefixHex[(index * 2) + 1]);
		header->opaquePrefix[index] = static_cast<uint8_t>((high << 4) | low);
	}
	header->atlasGuid = atlasGuid;
	pak->AddPointer(headerLump, offsetof(FontAssetHeader_v7_t, faceData), dataLump, faceOffset);
	pak->AddPointer(headerLump, offsetof(FontAssetHeader_v7_t, hashData), dataLump, hashOffset);
	for (const rapidjson::Value& value : internalPointers)
	{
		if (!value.IsArray() || value.Size() != 2 || !value[0].IsUint() || !value[1].IsUint())
			Error("Font internalPointers must contain [sourceOffset, targetOffset] unsigned-integer pairs.\n");

		const uint32_t sourceOffset = value[0].GetUint();
		const uint32_t targetOffset = value[1].GetUint();
		if (sourceOffset > sourceSize - sizeof(PagePtr_t) || targetOffset >= sourceSize)
			Error("Font internal pointer is outside \"%s\".\n", sourcePath.c_str());
		pak->AddPointer(dataLump, sourceOffset, dataLump, targetOffset);
	}
	Pak_RegisterGuidRefAtOffset(atlasGuid, offsetof(FontAssetHeader_v7_t, atlasGuid), headerLump, asset);

	asset.InitAsset(headerLump.GetPointer(), sizeof(FontAssetHeader_v7_t), PagePtr_t::NullPtr(), FONT_VERSION, AssetType::FONT);
	asset.SetHeaderPointer(headerLump.data);
	pak->FinishAsset();
}
