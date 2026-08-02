#include "RmdlPreview.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>

namespace fs = std::filesystem;

namespace
{
    template <typename T>
    bool ReadAt(const std::vector<std::uint8_t>& bytes, const size_t offset, T& value)
    {
        if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
            return false;
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return true;
    }

    bool ReadFile(const fs::path& path, std::vector<std::uint8_t>& bytes)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) return false;
        const std::streamoff size = input.tellg();
        if (size <= 0 || size > 2ll * 1024ll * 1024ll * 1024ll) return false;
        bytes.resize(static_cast<size_t>(size));
        input.seekg(0);
        return static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()), size));
    }

    firestar::editor::PreviewVertex DecodeVertex(const std::uint8_t* bytes,
        const std::uint32_t stride, const std::uint64_t flags)
    {
        firestar::editor::PreviewVertex result;
        size_t offset{};
        switch (flags & 3u)
        {
        case 1:
            std::memcpy(&result.x, bytes, sizeof(float) * 3);
            offset = 12;
            break;
        case 2:
        {
            std::uint64_t packed{};
            std::memcpy(&packed, bytes, sizeof(packed));
            constexpr std::uint64_t Mask21 = (1ull << 21) - 1;
            constexpr std::uint64_t Mask22 = (1ull << 22) - 1;
            result.x = static_cast<float>(packed & Mask21) * 0.0009765625f - 1024.0f;
            result.y = static_cast<float>((packed >> 21) & Mask21) * 0.0009765625f - 1024.0f;
            result.z = static_cast<float>((packed >> 42) & Mask22) * 0.0009765625f - 2048.0f;
            offset = 8;
            break;
        }
        default:
            return result;
        }

        // Matches RSX Vertex_t::ParseVertexFromVG: a packed blend block,
        // packed normal, optional vertex colour, then the first float2 UV.
        if ((flags & 0x1000ull) != 0 || (flags & 0x4000ull) != 0)
            offset += 8;
        offset += 4;
        if ((flags & 0x10ull) != 0)
            offset += 4;
        const std::uint64_t texcoord0Format = (flags >> 24) & 0xFull;
        if (texcoord0Format == 2 && offset + sizeof(float) * 2 <= stride)
        {
            std::memcpy(&result.u, bytes + offset, sizeof(float));
            std::memcpy(&result.v, bytes + offset + sizeof(float), sizeof(float));
        }
        return result;
    }

    bool Finite(const firestar::editor::PreviewVertex& vertex)
    {
        return std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z) &&
            std::abs(vertex.x) < 1000000.0f && std::abs(vertex.y) < 1000000.0f &&
            std::abs(vertex.z) < 1000000.0f;
    }

    struct RmdlMeshAssignment
    {
        std::uint32_t materialIndex{};
        int bodyPartIndex{-1};
        int modelIndex{-1};
        bool valid{};
    };

    struct RmdlLayout
    {
        size_t materialCountOffset{};
        size_t materialTableOffset{};
        size_t bodyPartCountOffset{};
        size_t bodyPartTableOffset{};
        size_t modelStride{};
        size_t modelMeshCountOffset{};
        size_t modelMeshOffsetOffset{};
        size_t meshStride{};
        size_t meshIdOffset{};
    };

    std::optional<std::string> ReadRelativeString(const std::vector<std::uint8_t>& bytes,
        const size_t base, const size_t relativeOffset, const size_t maximumLength = 128)
    {
        if (relativeOffset > bytes.size() - (std::min)(base, bytes.size()))
            return std::nullopt;
        const size_t start = base + relativeOffset;
        if (start >= bytes.size()) return std::nullopt;
        const size_t available = (std::min)(maximumLength, bytes.size() - start);
        size_t length{};
        while (length < available && bytes[start + length] != 0)
        {
            const unsigned char value = bytes[start + length];
            if (value < 0x20 || value > 0x7E) return std::nullopt;
            ++length;
        }
        if (length == available) return std::nullopt;
        return std::string(reinterpret_cast<const char*>(bytes.data() + start), length);
    }

    std::vector<std::uint64_t> ReadMaterialGuids(const std::vector<std::uint8_t>& rmdl,
        const RmdlLayout& layout)
    {
        std::vector<std::uint64_t> guids;
        std::uint32_t count{};
        std::uint32_t offset{};
        if (!ReadAt(rmdl, layout.materialCountOffset, count) ||
            !ReadAt(rmdl, layout.materialTableOffset, offset) || count > 4096)
            return guids;
        guids.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            std::uint64_t guid{};
            if (!ReadAt(rmdl, static_cast<size_t>(offset) + index * 12 + 4, guid))
                break;
            guids.push_back(guid);
        }
        return guids;
    }

    bool ParseBodyPartsWithLayout(const std::vector<std::uint8_t>& rmdl,
        const RmdlLayout& layout, const size_t materialCount,
        std::vector<firestar::editor::PreviewBodyPart>& bodyParts,
        std::vector<RmdlMeshAssignment>& assignments, size_t& score)
    {
        std::uint32_t bodyPartCount{};
        std::uint32_t bodyPartOffset{};
        if (!ReadAt(rmdl, layout.bodyPartCountOffset, bodyPartCount) ||
            !ReadAt(rmdl, layout.bodyPartTableOffset, bodyPartOffset) ||
            bodyPartCount > 256 || bodyPartOffset > rmdl.size() ||
            static_cast<std::uint64_t>(bodyPartCount) * 16 > rmdl.size() - bodyPartOffset)
            return false;

        bodyParts.clear();
        assignments.clear();
        score = 0;
        bodyParts.reserve(bodyPartCount);
        for (std::uint32_t bodyPartIndex = 0; bodyPartIndex < bodyPartCount; ++bodyPartIndex)
        {
            const size_t bodyAddress = static_cast<size_t>(bodyPartOffset) + bodyPartIndex * 16;
            std::uint32_t nameOffset{};
            std::int32_t modelCount{};
            std::int32_t modelOffset{};
            if (!ReadAt(rmdl, bodyAddress, nameOffset) ||
                !ReadAt(rmdl, bodyAddress + 4, modelCount) ||
                !ReadAt(rmdl, bodyAddress + 12, modelOffset) ||
                modelCount < 0 || modelCount > 256 || modelOffset < 0)
                return false;

            firestar::editor::PreviewBodyPart bodyPart;
            bodyPart.name = ReadRelativeString(rmdl, bodyAddress, nameOffset)
                .value_or("Body group " + std::to_string(bodyPartIndex + 1));
            const size_t modelBase = bodyAddress + static_cast<size_t>(modelOffset);
            if (modelBase > rmdl.size() ||
                static_cast<std::uint64_t>(modelCount) * layout.modelStride > rmdl.size() - modelBase)
                return false;

            for (int modelIndex = 0; modelIndex < modelCount; ++modelIndex)
            {
                const size_t modelAddress = modelBase + static_cast<size_t>(modelIndex) * layout.modelStride;
                std::int32_t meshCount{};
                std::int32_t meshOffset{};
                if (!ReadAt(rmdl, modelAddress + layout.modelMeshCountOffset, meshCount) ||
                    !ReadAt(rmdl, modelAddress + layout.modelMeshOffsetOffset, meshOffset) ||
                    meshCount < 0 || meshCount > 65536 || meshOffset < 0)
                    return false;
                const size_t meshBase = modelAddress + static_cast<size_t>(meshOffset);
                if (meshBase > rmdl.size() ||
                    static_cast<std::uint64_t>(meshCount) * layout.meshStride > rmdl.size() - meshBase)
                    return false;

                firestar::editor::PreviewBodyModel model;
                const auto modelName = ReadRelativeString(rmdl, modelAddress, 0, 64);
                if (modelName && !modelName->empty())
                    model.name = *modelName;
                else if (modelIndex == 0)
                    model.name = "Default";
                else
                    model.name = bodyPart.name + " " + std::to_string(modelIndex + 1);

                for (int meshIndex = 0; meshIndex < meshCount; ++meshIndex)
                {
                    const size_t meshAddress = meshBase + static_cast<size_t>(meshIndex) * layout.meshStride;
                    std::uint16_t materialIndex{};
                    std::int32_t meshId{};
                    if (!ReadAt(rmdl, meshAddress, materialIndex) ||
                        !ReadAt(rmdl, meshAddress + layout.meshIdOffset, meshId) ||
                        meshId < 0 || meshId > 65535 ||
                        (materialCount != 0 && materialIndex >= materialCount))
                        return false;
                    if (assignments.size() <= static_cast<size_t>(meshId))
                        assignments.resize(static_cast<size_t>(meshId) + 1);
                    assignments[meshId] = {
                        materialIndex,
                        static_cast<int>(bodyPartIndex),
                        modelIndex,
                        true};
                    ++score;
                }
                bodyPart.models.push_back(std::move(model));
            }
            bodyParts.push_back(std::move(bodyPart));
        }
        return true;
    }

    void ReadRmdlMetadata(const std::vector<std::uint8_t>& rmdl, const bool revisionOne,
        std::vector<std::uint64_t>& materials,
        std::vector<firestar::editor::PreviewBodyPart>& bodyParts,
        std::vector<RmdlMeshAssignment>& assignments)
    {
        std::vector<RmdlLayout> candidates;
        if (revisionOne)
            candidates.push_back({208, 212, 236, 240, 136, 76, 80, 92, 32});
        else
        {
            candidates.push_back({208, 212, 236, 240, 112, 76, 80, 76, 16});
            candidates.push_back({208, 212, 236, 240, 116, 76, 80, 76, 16});
            candidates.push_back({216, 220, 244, 248, 124, 76, 88, 76, 16});
        }

        size_t bestScore{};
        bool found{};
        for (const RmdlLayout& candidate : candidates)
        {
            std::vector<std::uint64_t> parsedMaterials = ReadMaterialGuids(rmdl, candidate);
            std::vector<firestar::editor::PreviewBodyPart> parsedBodyParts;
            std::vector<RmdlMeshAssignment> parsedAssignments;
            size_t meshScore{};
            if (!ParseBodyPartsWithLayout(rmdl, candidate, parsedMaterials.size(),
                parsedBodyParts, parsedAssignments, meshScore))
                continue;
            const size_t score = meshScore * 8192 + parsedMaterials.size();
            if (!found || score > bestScore)
            {
                materials = std::move(parsedMaterials);
                bodyParts = std::move(parsedBodyParts);
                assignments = std::move(parsedAssignments);
                bestScore = score;
                found = true;
            }
        }
    }

    const RmdlMeshAssignment* AssignmentFor(const std::vector<RmdlMeshAssignment>& assignments,
        const std::uint32_t meshIndex)
    {
        return meshIndex < assignments.size() && assignments[meshIndex].valid
            ? &assignments[meshIndex] : nullptr;
    }

    bool AppendMesh(const std::vector<std::uint8_t>& vg, const size_t vertexOffset,
        const std::uint32_t vertexCount, const std::uint32_t vertexStride,
        const std::uint64_t flags, const size_t indexOffset, const std::uint32_t indexCount,
        const std::uint32_t sourceMeshIndex, const std::vector<std::uint64_t>& materialGuids,
        const std::vector<RmdlMeshAssignment>& assignments,
        firestar::editor::RmdlPreviewData& preview)
    {
        if (vertexCount == 0 || indexCount < 3 || vertexCount > 5000000 || indexCount > 15000000 ||
            vertexStride < 6 || vertexStride > 1024 || (flags & 3u) == 0 || (flags & 3u) == 3 ||
            vertexOffset > vg.size() || static_cast<std::uint64_t>(vertexCount) * vertexStride > vg.size() - vertexOffset ||
            indexOffset > vg.size() || static_cast<std::uint64_t>(indexCount) * 2 > vg.size() - indexOffset)
            return false;

        const std::uint32_t baseVertex = static_cast<std::uint32_t>(preview.vertices.size());
        for (std::uint32_t index = 0; index < vertexCount; ++index)
        {
            const auto vertex = DecodeVertex(vg.data() + vertexOffset + static_cast<size_t>(index) * vertexStride,
                vertexStride, flags);
            if (!Finite(vertex)) return false;
            preview.vertices.push_back(vertex);
            preview.minimum.x = (std::min)(preview.minimum.x, vertex.x);
            preview.minimum.y = (std::min)(preview.minimum.y, vertex.y);
            preview.minimum.z = (std::min)(preview.minimum.z, vertex.z);
            preview.maximum.x = (std::max)(preview.maximum.x, vertex.x);
            preview.maximum.y = (std::max)(preview.maximum.y, vertex.y);
            preview.maximum.z = (std::max)(preview.maximum.z, vertex.z);
        }
        firestar::editor::PreviewMesh mesh;
        mesh.firstIndex = static_cast<std::uint32_t>(preview.indices.size());
        mesh.sourceMeshIndex = sourceMeshIndex;
        const RmdlMeshAssignment* assignment = AssignmentFor(assignments, sourceMeshIndex);
        const std::uint32_t materialIndex = assignment
            ? assignment->materialIndex
            : (materialGuids.empty() ? 0u : sourceMeshIndex % static_cast<std::uint32_t>(materialGuids.size()));
        mesh.materialGuid = materialIndex < materialGuids.size() ? materialGuids[materialIndex] : 0;
        if (assignment)
        {
            mesh.bodyPartIndex = assignment->bodyPartIndex;
            mesh.modelIndex = assignment->modelIndex;
        }
        for (std::uint32_t index = 0; index < indexCount; ++index)
        {
            std::uint16_t local{};
            std::memcpy(&local, vg.data() + indexOffset + static_cast<size_t>(index) * 2, sizeof(local));
            if (local >= vertexCount)
                continue;
            preview.indices.push_back(baseVertex + local);
        }
        mesh.indexCount = static_cast<std::uint32_t>(preview.indices.size()) - mesh.firstIndex;
        if (mesh.indexCount < 3)
        {
            preview.vertices.resize(baseVertex);
            preview.indices.resize(mesh.firstIndex);
            return false;
        }
        preview.meshes.push_back(mesh);
        return true;
    }

    bool ParseRevisionOne(const std::vector<std::uint8_t>& vg,
        const std::vector<std::uint64_t>& materialGuids,
        const std::vector<RmdlMeshAssignment>& assignments, const size_t lodSelection,
        firestar::editor::RmdlPreviewData& preview)
    {
        std::int64_t meshOffset{}, meshCount{}, indexBase{}, vertexBase{}, lodOffset{}, lodCount{};
        if (!ReadAt(vg, 32, meshOffset) || !ReadAt(vg, 40, meshCount) ||
            !ReadAt(vg, 48, indexBase) || !ReadAt(vg, 64, vertexBase) ||
            !ReadAt(vg, 112, lodOffset) || !ReadAt(vg, 120, lodCount) ||
            meshOffset < 0 || meshCount <= 0 || meshCount > 100000 ||
            lodOffset < 0 || lodCount <= 0)
            return false;
        preview.lodLevels.reserve(static_cast<size_t>(lodCount));
        for (std::int64_t index = 0; index < lodCount; ++index)
            preview.lodLevels.push_back(static_cast<int>(index));
        preview.selectedLod = (std::min)(lodSelection, preview.lodLevels.size() - 1);
        const size_t lodAddress = static_cast<size_t>(lodOffset) + preview.selectedLod * 8;
        std::uint16_t firstMesh{};
        std::uint16_t lodMeshCount{};
        if (!ReadAt(vg, lodAddress, firstMesh) ||
            !ReadAt(vg, lodAddress + 2, lodMeshCount))
            return false;
        lodMeshCount = static_cast<std::uint16_t>((std::min<std::int64_t>)(lodMeshCount, meshCount - firstMesh));
        for (std::uint32_t localMesh = 0; localMesh < lodMeshCount; ++localMesh)
        {
            const std::uint32_t meshIndex = firstMesh + localMesh;
            const size_t mesh = static_cast<size_t>(meshOffset) + static_cast<size_t>(meshIndex) * 72;
            std::int64_t flags{};
            std::uint32_t relativeVertices{}, stride{}, vertexCount{}, relativeIndices{}, indexCount{};
            if (!ReadAt(vg, mesh, flags) || !ReadAt(vg, mesh + 8, relativeVertices) ||
                !ReadAt(vg, mesh + 12, stride) || !ReadAt(vg, mesh + 16, vertexCount) ||
                !ReadAt(vg, mesh + 32, relativeIndices) || !ReadAt(vg, mesh + 36, indexCount))
                continue;
            (void)AppendMesh(vg, static_cast<size_t>(vertexBase) + relativeVertices, vertexCount, stride,
                static_cast<std::uint64_t>(flags), static_cast<size_t>(indexBase) + relativeIndices * 2ull,
                indexCount, localMesh, materialGuids, assignments, preview);
        }
        preview.format = "RSX VG revision 1 / LOD " + std::to_string(preview.lodLevels[preview.selectedLod]);
        return !preview.Empty();
    }

    bool ParseRevisionTwoOrThreeWithStride(const std::vector<std::uint8_t>& vg,
        const std::vector<std::uint64_t>& materialGuids,
        const std::vector<RmdlMeshAssignment>& assignments, const size_t meshStride,
        const size_t lodSelection,
        firestar::editor::RmdlPreviewData& preview)
    {
        std::int32_t lodCount{};
        std::int64_t relativeLod{};
        if (!ReadAt(vg, 12, lodCount) || !ReadAt(vg, 24, relativeLod) ||
            lodCount <= 0 || lodCount > 64)
            return false;
        const std::int64_t lodBase = 24 + relativeLod;
        const size_t selectedLod = (std::min)(lodSelection, static_cast<size_t>(lodCount - 1));
        const std::int64_t lodAddress = lodBase + static_cast<std::int64_t>(selectedLod) * 24;
        if (lodAddress < 0) return false;
        std::uint8_t meshCount{};
        std::int64_t relativeMesh{};
        preview.lodLevels.reserve(static_cast<size_t>(lodCount));
        for (std::int32_t index = 0; index < lodCount; ++index)
        {
            std::uint8_t level{};
            if (!ReadAt(vg, static_cast<size_t>(lodBase) + static_cast<size_t>(index) * 24 + 10, level))
                level = static_cast<std::uint8_t>(index);
            preview.lodLevels.push_back(level);
        }
        preview.selectedLod = selectedLod;
        if (!ReadAt(vg, static_cast<size_t>(lodAddress) + 8, meshCount) ||
            !ReadAt(vg, static_cast<size_t>(lodAddress) + 16, relativeMesh) || meshCount == 0)
            return false;
        const std::int64_t meshBase = lodAddress + 16 + relativeMesh;
        if (meshBase < 0) return false;
        for (std::uint32_t meshIndex = 0; meshIndex < meshCount; ++meshIndex)
        {
            // Revision 2 and 3 share the mesh prefix used here. The revision 3
            // blend-shape tail is deliberately ignored for a static preview.
            const size_t mesh = static_cast<size_t>(meshBase) + static_cast<size_t>(meshIndex) * meshStride;
            std::uint64_t flags{};
            std::uint32_t stride{}, vertexCount{};
            std::int64_t relativeIndices{}, packedIndexCount{}, relativeVertices{}, vertexBufferSize{};
            if (!ReadAt(vg, mesh, flags) || !ReadAt(vg, mesh + 8, stride) ||
                !ReadAt(vg, mesh + 12, vertexCount) || !ReadAt(vg, mesh + 16, relativeIndices) ||
                !ReadAt(vg, mesh + 24, packedIndexCount) || !ReadAt(vg, mesh + 32, relativeVertices) ||
                !ReadAt(vg, mesh + 40, vertexBufferSize))
                continue;
            const std::uint64_t count56 = static_cast<std::uint64_t>(packedIndexCount) & 0x00FFFFFFFFFFFFFFull;
            if (count56 > (std::numeric_limits<std::uint32_t>::max)()) continue;
            const std::int64_t indexAddress = static_cast<std::int64_t>(mesh + 16) + relativeIndices;
            const std::int64_t vertexAddress = static_cast<std::int64_t>(mesh + 32) + relativeVertices;
            if (indexAddress < 0 || vertexAddress < 0 || vertexBufferSize < 0) continue;
            (void)AppendMesh(vg, static_cast<size_t>(vertexAddress), vertexCount, stride, flags,
                static_cast<size_t>(indexAddress), static_cast<std::uint32_t>(count56), meshIndex,
                materialGuids, assignments, preview);
        }
        preview.format = meshStride == 96
            ? "RSX VG revision 2 / LOD " + std::to_string(preview.lodLevels[preview.selectedLod])
            : "RSX VG revision 3 / LOD " + std::to_string(preview.lodLevels[preview.selectedLod]);
        return !preview.Empty();
    }

    bool ParseRevisionTwoOrThree(const std::vector<std::uint8_t>& vg,
        const std::vector<std::uint64_t>& materialGuids,
        const std::vector<RmdlMeshAssignment>& assignments, const size_t lodSelection,
        firestar::editor::RmdlPreviewData& preview)
    {
        firestar::editor::RmdlPreviewData revisionTwo;
        firestar::editor::RmdlPreviewData revisionThree;
        revisionTwo.Clear();
        revisionThree.Clear();
        const bool loadedTwo = ParseRevisionTwoOrThreeWithStride(vg, materialGuids, assignments,
            96, lodSelection, revisionTwo);
        const bool loadedThree = ParseRevisionTwoOrThreeWithStride(vg, materialGuids, assignments,
            112, lodSelection, revisionThree);
        if (!loadedTwo && !loadedThree)
            return false;
        const size_t twoScore = revisionTwo.indices.size() + revisionTwo.meshes.size() * 3;
        const size_t threeScore = revisionThree.indices.size() + revisionThree.meshes.size() * 3;
        preview = loadedTwo && (!loadedThree || twoScore >= threeScore)
            ? std::move(revisionTwo)
            : std::move(revisionThree);
        return true;
    }
}

namespace firestar::editor
{
    void RmdlPreviewData::Clear()
    {
        vertices.clear();
        indices.clear();
        meshes.clear();
        bodyParts.clear();
        lodLevels.clear();
        minimum = {
            (std::numeric_limits<float>::max)(),
            (std::numeric_limits<float>::max)(),
            (std::numeric_limits<float>::max)()};
        maximum = {
            (std::numeric_limits<float>::lowest)(),
            (std::numeric_limits<float>::lowest)(),
            (std::numeric_limits<float>::lowest)()};
        selectedLod = 0;
        format.clear();
    }

    bool LoadRmdlPreview(const fs::path& rmdlPath, const size_t lodSelection,
        RmdlPreviewData& preview, std::string& error)
    {
        preview.Clear();
        error.clear();
        std::vector<std::uint8_t> rmdl;
        if (!ReadFile(rmdlPath, rmdl) || rmdl.size() < 216 || std::memcmp(rmdl.data(), "IDST", 4) != 0)
        {
            error = "The model source is not a supported RMDL file.";
            return false;
        }
        fs::path vgPath = rmdlPath;
        vgPath.replace_extension(L".vg");
        std::vector<std::uint8_t> vg;
        if (!ReadFile(vgPath, vg) || vg.size() < 64)
        {
            error = "The model needs its matching .vg geometry file for preview.";
            return false;
        }
        std::uint32_t id{};
        std::uint32_t version{};
        if (!ReadAt(vg, 0, id) || !ReadAt(vg, 4, version) || id != 0x47567430u || version != 1)
        {
            error = "The model's VG geometry header is unsupported.";
            return false;
        }
        std::int32_t possibleRevisionOneSize{};
        (void)ReadAt(vg, 12, possibleRevisionOneSize);
        const bool looksRevisionOne = possibleRevisionOneSize > 128 &&
            static_cast<size_t>(possibleRevisionOneSize) <= vg.size() + 4096;
        std::vector<std::uint64_t> materials;
        std::vector<RmdlMeshAssignment> assignments;
        ReadRmdlMetadata(rmdl, looksRevisionOne, materials, preview.bodyParts, assignments);
        std::vector<PreviewBodyPart> bodyParts = std::move(preview.bodyParts);
        const bool loaded = looksRevisionOne
            ? ParseRevisionOne(vg, materials, assignments, lodSelection, preview)
            : ParseRevisionTwoOrThree(vg, materials, assignments, lodSelection, preview);
        if (!loaded)
        {
            preview.Clear();
            error = "The RMDL geometry layout could not be decoded for preview.";
            return false;
        }
        preview.bodyParts = std::move(bodyParts);
        return true;
    }
}
