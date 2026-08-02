#include "pch.h"
#include "assets.h"
#include "public/ui.h"
#include "public/rui_package.h"

static bool UI_HasDefaultStringOffset(const std::vector<uint16_t>& offsets, const uint16_t offset)
{
    for (const uint16_t candidate : offsets)
    {
        if (candidate == offset)
            return true;
    }

    return false;
}

static void UI_RepairImplicitV40TextPointers(
    const RuiPackage& rui,
    std::vector<char>& defaultData,
    std::vector<char>& defaultStrings,
    std::vector<uint16_t>& defaultStringOffsets,
    const char* const assetPath)
{
    // S9/v40 type-0 jobs store the default text pointer's data-struct offset
    // at byte 10.  The RPAK exporter used for the source depot omitted some
    // of those pointers from its relocation list, leaving an old process
    // address in the resulting .ruip.  RePak must never carry that address
    // into a new RPAK: the target text-size helper dereferences it after the
    // HUD starts rendering.  Registered entries retain their exported text;
    // omitted entries are rebuilt as valid empty strings until the exporter
    // can preserve their original literals.
    if (rui.GetSourceRuiVersion() != 40)
        return;

    static constexpr size_t sourceJobSizes[] = { 24, 46, 28, 28, 46, 12 };
    static constexpr size_t type0DataPointerOffset = 10;
    size_t sourceOffset = 0;

    for (uint16_t jobIndex = 0; jobIndex < rui.hdr.renderJobCount; ++jobIndex)
    {
        if (sourceOffset + sizeof(uint16_t) > rui.renderJobs.size())
            Error("Truncated v40 RUI render-job table at job %hu in asset \"%s\".\n", jobIndex, assetPath);

        uint16_t type = 0;
        memcpy(&type, rui.renderJobs.data() + sourceOffset, sizeof(type));
        if (type >= _countof(sourceJobSizes))
            Error("Unsupported v40 RUI render-job type %hu at job %hu in asset \"%s\".\n", type, jobIndex, assetPath);

        const size_t sourceJobSize = sourceJobSizes[type];
        if (sourceOffset + sourceJobSize > rui.renderJobs.size())
            Error("Out-of-range v40 RUI render job %hu in asset \"%s\".\n", jobIndex, assetPath);

        if (type == 0)
        {
            uint16_t dataOffset = 0;
            memcpy(&dataOffset,
                rui.renderJobs.data() + sourceOffset + type0DataPointerOffset,
                sizeof(dataOffset));

            // Not every type-0 record is a text read. Some use this field as
            // an end/sentinel offset (including exactly data.size()), so only
            // treat an in-range eight-byte field as a candidate pointer.
            const bool hasInlineDataPointer =
                static_cast<size_t>(dataOffset) + sizeof(uint64_t) <= defaultData.size();
            if (hasInlineDataPointer && !UI_HasDefaultStringOffset(defaultStringOffsets, dataOffset))
            {
                const uint64_t fallbackStringOffset = static_cast<uint64_t>(defaultStrings.size());
                memcpy(defaultData.data() + dataOffset, &fallbackStringOffset, sizeof(fallbackStringOffset));
                defaultStrings.push_back('\0');
                defaultStringOffsets.push_back(dataOffset);
            }
        }

        sourceOffset += sourceJobSize;
    }

    if (sourceOffset != rui.renderJobs.size())
        Error("The v40 RUI render-job table for asset \"%s\" has %zu trailing bytes.\n",
            assetPath, rui.renderJobs.size() - sourceOffset);
}

static std::vector<char> UI_ConvertTransformData_v42_to_v39(const RuiPackage& rui, const char* const assetPath)
{
    // RUI v42 added one trailing uint16 to each entry handled by transform
    // opcodes 2-6 and 8-11. The v39 interpreter advances by the older entry
    // sizes, so leaving those words in place eventually makes it dispatch a
    // data byte as an opcode. Opcode 0 is a single-byte instruction. Opcode
    // 7 is a conditional prefix that tail-calls the opcode 5 or 6 handler, so
    // it is followed by the same count byte and entry layout as those opcodes.
    static constexpr size_t sourceEntrySizes[] =
    {
        0, 2, 12, 12, 12, 12, 12, 12, 22, 22, 22, 32, 8, 6
    };
    static constexpr size_t targetEntrySizes[] =
    {
        0, 2, 10, 10, 10, 10, 10, 10, 20, 20, 20, 30, 8, 6
    };

    std::vector<char> converted;
    converted.reserve(rui.transformData.size());
    size_t sourceOffset = 0;

    while (sourceOffset < rui.transformData.size())
    {
        const uint8_t opcode = static_cast<uint8_t>(rui.transformData[sourceOffset++]);
        converted.push_back(static_cast<char>(opcode));

        if (opcode == 0)
            continue;

        if (opcode >= _countof(sourceEntrySizes) || sourceEntrySizes[opcode] == 0)
            Error("Unsupported v42 RUI transform opcode %hhu at byte %zu in asset \"%s\".\n",
                opcode, sourceOffset - 1, assetPath);
        if (sourceOffset >= rui.transformData.size())
            Error("Truncated v42 RUI transform count after opcode %hhu in asset \"%s\".\n",
                opcode, assetPath);

        const uint8_t count = static_cast<uint8_t>(rui.transformData[sourceOffset++]);
        converted.push_back(static_cast<char>(count));
        const size_t sourceEntrySize = sourceEntrySizes[opcode];
        const size_t targetEntrySize = targetEntrySizes[opcode];

        for (uint8_t entry = 0; entry < count; ++entry)
        {
            if (sourceOffset + sourceEntrySize > rui.transformData.size())
                Error("Out-of-range v42 RUI transform opcode %hhu entry %hhu in asset \"%s\".\n",
                    opcode, entry, assetPath);

            converted.insert(converted.end(),
                rui.transformData.data() + sourceOffset,
                rui.transformData.data() + sourceOffset + targetEntrySize);
            sourceOffset += sourceEntrySize;
        }
    }

    return converted;
}

static std::vector<char> UI_ConvertRenderJobs_v40_to_v39(const RuiPackage& rui, const char* const assetPath)
{
    // RUI v40 added one uint16 field at byte 14 of render-job type 0.
    // The target v39 engine walks jobs with a fixed type-size table, so the
    // extra word must be removed or every following job is read off by two.
    static constexpr size_t sourceJobSizes[] = { 24, 46, 28, 28, 46, 12 };
    static constexpr size_t targetJobSizes[] = { 22, 46, 28, 28, 46, 12 };
    static constexpr size_t type0RemovedFieldOffset = 14;

    std::vector<char> converted;
    converted.reserve(rui.renderJobs.size());
    size_t sourceOffset = 0;
    size_t expectedTargetSize = 0;

    for (uint16_t i = 0; i < rui.hdr.renderJobCount; i++)
    {
        if (sourceOffset + sizeof(uint16_t) > rui.renderJobs.size())
            Error("Truncated v40 RUI render-job table at job %hu in asset \"%s\".\n", i, assetPath);

        uint16_t type = 0;
        memcpy(&type, rui.renderJobs.data() + sourceOffset, sizeof(type));
        if (type >= _countof(sourceJobSizes))
            Error("Unsupported v40 RUI render-job type %hu at job %hu in asset \"%s\".\n", type, i, assetPath);

        const size_t sourceJobSize = sourceJobSizes[type];
        if (sourceOffset + sourceJobSize > rui.renderJobs.size())
            Error("Out-of-range v40 RUI render job %hu in asset \"%s\".\n", i, assetPath);

        const char* const sourceJob = rui.renderJobs.data() + sourceOffset;
        if (type == 0)
        {
            converted.insert(converted.end(), sourceJob, sourceJob + type0RemovedFieldOffset);
            converted.insert(converted.end(), sourceJob + type0RemovedFieldOffset + sizeof(uint16_t), sourceJob + sourceJobSize);
        }
        else
        {
            converted.insert(converted.end(), sourceJob, sourceJob + sourceJobSize);
        }

        sourceOffset += sourceJobSize;
        expectedTargetSize += targetJobSizes[type];
    }

    if (sourceOffset != rui.renderJobs.size())
        Error("The v40 RUI render-job table for asset \"%s\" has %zu trailing bytes.\n",
            assetPath, rui.renderJobs.size() - sourceOffset);

    if (converted.size() != expectedTargetSize)
        Error("Converted RUI render-job size mismatch for asset \"%s\".\n", assetPath);

    return converted;
}

static std::vector<char> UI_ConvertRenderJobs_v42_to_v39(const RuiPackage& rui, const char* const assetPath)
{
    // RUI v42 prepends one uint16 field after the type in every render job.
    // Text and image jobs also gained one trailing argument-reference word.
    // Type 0 still contains the v40-only word at byte 16 in the v42 layout.
    // Removing these fields restores the target v39 fixed-size records.
    static constexpr size_t sourceJobSizes[] = { 28, 50, 30, 30, 48, 14 };
    static constexpr size_t targetJobSizes[] = { 22, 46, 28, 28, 46, 12 };
    static constexpr size_t removedOffsets[][3] = {
        { 2, 16, 24 },                         // type 0: universal, v40, v42 text field
        { 2, 44, static_cast<size_t>(-1) },   // type 1: universal, v42 image field
        { 2, static_cast<size_t>(-1), static_cast<size_t>(-1) },
        { 2, static_cast<size_t>(-1), static_cast<size_t>(-1) },
        { 2, static_cast<size_t>(-1), static_cast<size_t>(-1) },
        { 2, static_cast<size_t>(-1), static_cast<size_t>(-1) },
    };

    std::vector<char> converted;
    converted.reserve(rui.renderJobs.size());
    size_t sourceOffset = 0;
    size_t expectedTargetSize = 0;

    for (uint16_t i = 0; i < rui.hdr.renderJobCount; i++)
    {
        if (sourceOffset + sizeof(uint16_t) > rui.renderJobs.size())
            Error("Truncated v42 RUI render-job table at job %hu in asset \"%s\".\n", i, assetPath);

        uint16_t type = 0;
        memcpy(&type, rui.renderJobs.data() + sourceOffset, sizeof(type));
        if (type >= _countof(sourceJobSizes))
            Error("Unsupported v42 RUI render-job type %hu at job %hu in asset \"%s\".\n", type, i, assetPath);

        const size_t sourceJobSize = sourceJobSizes[type];
        if (sourceOffset + sourceJobSize > rui.renderJobs.size())
            Error("Out-of-range v42 RUI render job %hu in asset \"%s\".\n", i, assetPath);

        const char* const sourceJob = rui.renderJobs.data() + sourceOffset;
        for (size_t byteOffset = 0; byteOffset < sourceJobSize; byteOffset += sizeof(uint16_t))
        {
            bool removeWord = false;
            for (const size_t removedOffset : removedOffsets[type])
            {
                if (removedOffset == byteOffset)
                {
                    removeWord = true;
                    break;
                }
            }

            if (!removeWord)
                converted.insert(converted.end(), sourceJob + byteOffset, sourceJob + byteOffset + sizeof(uint16_t));
        }

        sourceOffset += sourceJobSize;
        expectedTargetSize += targetJobSizes[type];
    }

    if (sourceOffset != rui.renderJobs.size())
        Error("The v42 RUI render-job table for asset \"%s\" has %zu trailing bytes.\n",
            assetPath, rui.renderJobs.size() - sourceOffset);

    if (converted.size() != expectedTargetSize)
        Error("Converted v42 RUI render-job size mismatch for asset \"%s\".\n", assetPath);

    return converted;
}

void UI_loadFromPackage(CPakFileBuilder* const pak, const PakGuid_t assetGuid, const char* const assetPath, const rapidjson::Value& mapEntry) {
    
    UNUSED(assetGuid);
    const fs::path inputFilePath = pak->GetAssetPath() / fs::path(assetPath).replace_extension("ruip");
    RuiPackage rui{inputFilePath};
    const bool hasExplicitRuiVersion = mapEntry.HasMember("$ruiVersion");
    const uint32_t requestedRuiVersion = JSON_GetValueOrDefault(mapEntry, "$ruiVersion", static_cast<uint32_t>(rui.GetRuiVersion()));

    if (requestedRuiVersion > UINT16_MAX)
        Error("RUI asset version %u is out of range for asset \"%s\".\n", requestedRuiVersion, assetPath);

    const uint16_t ruiVersion = static_cast<uint16_t>(requestedRuiVersion);

    if (ruiVersion != R2_UI_VERSION && ruiVersion != R5_UI_VERSION && ruiVersion != 40)
        Error("Unsupported RUI asset version %hu for asset \"%s\".\n", ruiVersion, assetPath);

    std::vector<char> renderJobs = rui.renderJobs;
    std::vector<char> transformData = rui.transformData;
    std::vector<char> defaultData = rui.defaultData;
    std::vector<char> defaultStrings = rui.defaultStrings;
    std::vector<uint16_t> defaultStringOffsets = rui.defaultStringOffsets;
    UI_RepairImplicitV40TextPointers(rui, defaultData, defaultStrings, defaultStringOffsets, assetPath);
    if (hasExplicitRuiVersion && rui.GetSourceRuiVersion() == 40 && ruiVersion == R5_UI_VERSION)
        renderJobs = UI_ConvertRenderJobs_v40_to_v39(rui, assetPath);
    else if (hasExplicitRuiVersion && rui.GetSourceRuiVersion() == 42 && ruiVersion == R5_UI_VERSION)
    {
        renderJobs = UI_ConvertRenderJobs_v42_to_v39(rui, assetPath);
        transformData = UI_ConvertTransformData_v42_to_v39(rui, assetPath);
    }
    
    PakAsset_t& asset = pak->BeginAsset(assetGuid,assetPath);
    PakPageLump_s hdrChunk = pak->CreatePageLump(sizeof(RuiHeader_v30_s),SF_HEAD|SF_CLIENT,8);
    RuiHeader_v30_s* ruiHdr = reinterpret_cast<RuiHeader_v30_s*>(hdrChunk.data);
    *ruiHdr = rui.CreateRuiHeader_v30();
    
    PakPageLump_s nameChunk = pak->CreatePageLump(rui.name.size(),SF_CPU|SF_CLIENT,8);
    memcpy(nameChunk.data,rui.name.data(),rui.name.size());
    pak->AddPointer(hdrChunk,offsetof(RuiHeader_v30_s,name),nameChunk,0);

    PakPageLump_s defaultValuesChunk = pak->CreatePageLump(defaultData.size()+defaultStrings.size(),SF_CPU|SF_CLIENT,8);
    memcpy(defaultValuesChunk.data,defaultData.data(),defaultData.size());
    memcpy(&defaultValuesChunk.data[defaultData.size()],defaultStrings.data(),defaultStrings.size());
    for (uint16_t offset : defaultStringOffsets) {
        if (static_cast<size_t>(offset) + sizeof(uint64_t) > defaultData.size())
            Error("Out-of-range default string pointer offset %hu in asset \"%s\".\n", offset, assetPath);

        uint64_t sourceStringOffset = 0;
        memcpy(&sourceStringOffset, defaultData.data() + offset, sizeof(sourceStringOffset));
        if (sourceStringOffset >= defaultStrings.size())
            Error("Out-of-range default string offset %llu in asset \"%s\".\n",
                static_cast<unsigned long long>(sourceStringOffset), assetPath);

        const uint64_t stringOffset = sourceStringOffset + defaultData.size();
        pak->AddPointer(defaultValuesChunk,offset,defaultValuesChunk,stringOffset);
    }
    pak->AddPointer(hdrChunk,offsetof(RuiHeader_v30_s,dataStructInitData),defaultValuesChunk,0);
   
    PakPageLump_s transformDataChunk = pak->CreatePageLump(transformData.size(),SF_CPU|SF_CLIENT,8);
    memcpy(transformDataChunk.data,transformData.data(),transformData.size());
    pak->AddPointer(hdrChunk,offsetof(RuiHeader_v30_s,transformData),transformDataChunk,0);

    PakPageLump_s argClustersChunk = pak->CreatePageLump(rui.argCluster.size()*sizeof(ArgCluster_s),SF_CPU|SF_CLIENT,8);
    memcpy(argClustersChunk.data,rui.argCluster.data(),rui.argCluster.size()*sizeof(ArgCluster_s));
    pak->AddPointer(hdrChunk,offsetof(RuiHeader_v30_s,argClusters),argClustersChunk,0);

    PakPageLump_s argumentsChunk = pak->CreatePageLump(rui.arguments.size()*sizeof(Argument_s),SF_CPU|SF_CLIENT,8);
    memcpy(argumentsChunk.data,rui.arguments.data(),rui.arguments.size()*sizeof(Argument_s));
    pak->AddPointer(hdrChunk,offsetof(RuiHeader_v30_s,arguments),argumentsChunk,0);

    if (!rui.argNames.empty())
    {
        PakPageLump_s argNamesChunk = pak->CreatePageLump(rui.argNames.size(), SF_CPU | SF_CLIENT, 8);
        memcpy(argNamesChunk.data, rui.argNames.data(), rui.argNames.size());
        pak->AddPointer(hdrChunk, offsetof(RuiHeader_v30_s, argNames), argNamesChunk, 0);
    }
    else
    {
        ruiHdr->argNames = 0;
    }

    PakPageLump_s styleDescriptorChunk = pak->CreatePageLump(rui.styleDescriptors.size(),SF_CPU|SF_CLIENT,8);
    memcpy(styleDescriptorChunk.data,rui.styleDescriptors.data(),rui.styleDescriptors.size());
    pak->AddPointer(hdrChunk,offsetof(RuiHeader_v30_s,styleDescriptors),styleDescriptorChunk,0);
    
    PakPageLump_s renderJobChunk = pak->CreatePageLump(renderJobs.size(),SF_CPU|SF_CLIENT,8);
    memcpy(renderJobChunk.data,renderJobs.data(),renderJobs.size());
    pak->AddPointer(hdrChunk,offsetof(RuiHeader_v30_s,renderJobData),renderJobChunk,0);

    if (!rui.keyframingData.empty())
    {
        PakPageLump_s keyframingChunk = pak->CreatePageLump(rui.keyframingData.size(), SF_CPU | SF_CLIENT, 8);
        memcpy(keyframingChunk.data, rui.keyframingData.data(), rui.keyframingData.size());

        for (const RuiPackagePointerFixup_t& fixup : rui.pointerFixups)
        {
            // Default string pointers predate the general fixup table and are
            // still reconstructed above from defaultStringOffsets.
            if (fixup.srcSection == RUI_PACKAGE_SECTION_DEFAULT_VALUES
                && fixup.dstSection == RUI_PACKAGE_SECTION_DEFAULT_VALUES)
                continue;

            if (fixup.srcSection != RUI_PACKAGE_SECTION_KEYFRAMING
                || fixup.dstSection != RUI_PACKAGE_SECTION_KEYFRAMING)
                Error("Unsupported RUIP pointer fixup from section %u to %u in asset \"%s\".\n",
                    fixup.srcSection, fixup.dstSection, assetPath);

            if (static_cast<size_t>(fixup.srcOffset) + sizeof(uint64_t) > rui.keyframingData.size()
                || fixup.dstOffset >= rui.keyframingData.size())
                Error("Out-of-range RUIP keyframing pointer fixup (%u -> %u) in asset \"%s\".\n",
                    fixup.srcOffset, fixup.dstOffset, assetPath);

            pak->AddPointer(keyframingChunk, fixup.srcOffset, keyframingChunk, fixup.dstOffset);
        }

        pak->AddPointer(hdrChunk, offsetof(RuiHeader_v30_s, keyframings), keyframingChunk, 0);
    }

    asset.InitAsset(hdrChunk.GetPointer(),sizeof(RuiHeader_v30_s),
        PagePtr_t::NullPtr(),ruiVersion,AssetType::UI);
    asset.SetHeaderPointer(hdrChunk.data);

    pak->FinishAsset();

}


void Assets::AddRuiAsset_v30(CPakFileBuilder* const pak, const PakGuid_t assetGuid, const char* const assetPath, const rapidjson::Value& mapEntry)
{
    UI_loadFromPackage(pak,assetGuid,assetPath,mapEntry);
}
