#include "SceneLevelBuilder.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "ViewMath.hpp"

namespace grpcmud::client::scene
{
namespace
{
constexpr float kFloorThickness = 0.10f;
constexpr float kFloorTileSize = kCellSize + 0.02f;
constexpr float kFloorCenterY = -0.62f;
constexpr float kGroundY = kFloorCenterY + (kFloorThickness * 0.5f);
constexpr float kBaseFloorThickness = 0.08f;
constexpr float kBaseFloorCenterY = -0.72f;
constexpr float kBaseFloorMarginCells = 2.0f;
constexpr float kWallHeight = 2.3f;
constexpr float kWallCenterY = kGroundY + (kWallHeight * 0.5f) - 0.04f;
constexpr float kWallCubeSize = kCellSize + 0.02f;
constexpr float kPlayerHeight = 1.75f;
constexpr float kPlayerWidth = 0.55f;
constexpr float kNpcHeight = 1.55f;
constexpr float kNpcWidth = 0.65f;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr char kSkyboxPath[] = "asset/cubemap/hamarikyu.hdr";
constexpr char kSkyboxEnvPath[] = "asset/cubemap/hamarikyu_env.hdr";

struct CubeSpec
{
    float tx = 0.0f;
    float ty = 0.0f;
    float tz = 0.0f;
    float sx = 1.0f;
    float sy = 1.0f;
    float sz = 1.0f;
};

struct GeometryData
{
    std::vector<CubeSpec> floor_cubes;
    std::vector<CubeSpec> wall_cubes;
    std::vector<CubeSpec> player_cubes;
    std::vector<CubeSpec> npc_cubes;
};

struct CoordBounds
{
    int min_x = 0;
    int max_x = 0;
    int min_y = 0;
    int max_y = 0;
};

struct RelativeSquareSig
{
    int rel_x = 0;
    int rel_y = 0;
    std::uint8_t kind = 0;
    std::uint8_t open_mask = 0;
};

struct RelativeActorSig
{
    int rel_x = 0;
    int rel_y = 0;
    std::uint8_t kind = 0;
    std::uint8_t facing = 0;
};

struct MaterialSpec
{
    std::string name;
    glm::vec4 base_color{1.0f};
    float roughness = 1.0f;
    float metallic = 0.0f;
};

struct MeshBuffers
{
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<std::uint32_t> indices;
};

struct BufferViewInfo
{
    std::size_t offset = 0;
    std::size_t length = 0;
    int target = 0;
};

std::int64_t CoordKey(int x, int y)
{
    return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(y);
}

int CoordXFromKey(std::int64_t key)
{
    return static_cast<std::int32_t>(key >> 32);
}

int CoordYFromKey(std::int64_t key)
{
    return static_cast<std::int32_t>(key & 0xFFFFFFFF);
}

bool CoordLess(std::int64_t lhs, std::int64_t rhs)
{
    const int lx = CoordXFromKey(lhs);
    const int rx = CoordXFromKey(rhs);
    if (lx != rx)
    {
        return lx < rx;
    }
    return CoordYFromKey(lhs) < CoordYFromKey(rhs);
}

void AddCubeSpec(
    std::vector<CubeSpec>& cubes,
    float tx,
    float ty,
    float tz,
    float sx,
    float sy,
    float sz)
{
    cubes.push_back(CubeSpec{tx, ty, tz, sx, sy, sz});
}

std::uint64_t Fnv1aAppend(std::uint64_t hash, std::uint64_t value)
{
    for (int i = 0; i < 8; ++i)
    {
        const std::uint8_t byte = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu);
        hash ^= byte;
        hash *= kFnvPrime;
    }
    return hash;
}

std::uint32_t FloatBits(float value)
{
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::uint64_t HashCubeSpecs(const std::vector<CubeSpec>& cubes)
{
    std::uint64_t hash = kFnvOffset;
    hash = Fnv1aAppend(hash, static_cast<std::uint64_t>(cubes.size()));
    for (const CubeSpec& cube : cubes)
    {
        hash = Fnv1aAppend(hash, FloatBits(cube.tx));
        hash = Fnv1aAppend(hash, FloatBits(cube.ty));
        hash = Fnv1aAppend(hash, FloatBits(cube.tz));
        hash = Fnv1aAppend(hash, FloatBits(cube.sx));
        hash = Fnv1aAppend(hash, FloatBits(cube.sy));
        hash = Fnv1aAppend(hash, FloatBits(cube.sz));
    }
    return hash;
}

std::uint64_t HashGeometry(const GeometryData& geometry)
{
    return Fnv1aAppend(
        Fnv1aAppend(
            Fnv1aAppend(
                HashCubeSpecs(geometry.floor_cubes),
                HashCubeSpecs(geometry.wall_cubes)),
            HashCubeSpecs(geometry.player_cubes)),
        HashCubeSpecs(geometry.npc_cubes));
}

std::vector<std::int64_t> SortedCoords(const std::unordered_set<std::int64_t>& coords)
{
    std::vector<std::int64_t> sorted(coords.begin(), coords.end());
    std::sort(sorted.begin(), sorted.end(), CoordLess);
    return sorted;
}

CoordBounds ComputeBounds(
    const std::unordered_set<std::int64_t>& coords,
    int center_x,
    int center_y)
{
    CoordBounds bounds{};
    bounds.min_x = center_x;
    bounds.max_x = center_x;
    bounds.min_y = center_y;
    bounds.max_y = center_y;
    for (const std::int64_t key : coords)
    {
        const int x = CoordXFromKey(key);
        const int y = CoordYFromKey(key);
        bounds.min_x = std::min(bounds.min_x, x);
        bounds.max_x = std::max(bounds.max_x, x);
        bounds.min_y = std::min(bounds.min_y, y);
        bounds.max_y = std::max(bounds.max_y, y);
    }
    return bounds;
}

void ExpandGroundToBounds(std::unordered_set<std::int64_t>& ground, const CoordBounds& bounds)
{
    for (int y = bounds.min_y; y <= bounds.max_y; ++y)
    {
        for (int x = bounds.min_x; x <= bounds.max_x; ++x)
        {
            ground.insert(CoordKey(x, y));
        }
    }
}

void BuildGroundAndWallCoords(
    const mud::v1::LocalViewUpdate& view,
    std::unordered_set<std::int64_t>& ground,
    std::unordered_set<std::int64_t>& walls)
{
    std::unordered_map<std::int64_t, const mud::v1::VisibleSquare*> squares_by_coord;
    squares_by_coord.reserve(static_cast<std::size_t>(view.squares_size()));
    for (const auto& square : view.squares())
    {
        squares_by_coord[CoordKey(square.x(), square.y())] = &square;
    }

    const auto has_square = [&](int x, int y) -> bool {
        return squares_by_coord.find(CoordKey(x, y)) != squares_by_coord.end();
    };

    ground.reserve(static_cast<std::size_t>(view.squares_size()) * 3u);
    walls.reserve(static_cast<std::size_t>(view.squares_size()) * 2u);

    for (const auto& square : view.squares())
    {
        const std::int64_t key = CoordKey(square.x(), square.y());
        ground.insert(key);

        if (square.kind() == mud::v1::SQUARE_KIND_WALL)
        {
            walls.insert(key);
            continue;
        }

        if (!square.open_north() && !has_square(square.x(), square.y() - 1))
        {
            walls.insert(CoordKey(square.x(), square.y() - 1));
        }
        if (!square.open_west() && !has_square(square.x() - 1, square.y()))
        {
            walls.insert(CoordKey(square.x() - 1, square.y()));
        }
        if (!square.open_south() && !has_square(square.x(), square.y() + 1))
        {
            walls.insert(CoordKey(square.x(), square.y() + 1));
        }
        if (!square.open_east() && !has_square(square.x() + 1, square.y()))
        {
            walls.insert(CoordKey(square.x() + 1, square.y()));
        }
    }

    if (ground.empty())
    {
        ground.insert(CoordKey(view.center_x(), view.center_y()));
    }
    for (const std::int64_t key : walls)
    {
        ground.insert(key);
    }
}

std::vector<const mud::v1::VisibleActor*> SortedActors(const mud::v1::LocalViewUpdate& view)
{
    std::vector<const mud::v1::VisibleActor*> actors;
    actors.reserve(static_cast<std::size_t>(view.actors_size()));
    for (const auto& actor : view.actors())
    {
        if (actor.kind() == mud::v1::VisibleActor::KIND_SELF)
        {
            continue;
        }
        actors.push_back(&actor);
    }
    std::sort(
        actors.begin(),
        actors.end(),
        [](const mud::v1::VisibleActor* lhs, const mud::v1::VisibleActor* rhs) {
            if (lhs->kind() != rhs->kind())
            {
                return lhs->kind() < rhs->kind();
            }
            if (lhs->x() != rhs->x())
            {
                return lhs->x() < rhs->x();
            }
            if (lhs->y() != rhs->y())
            {
                return lhs->y() < rhs->y();
            }
            if (lhs->name() != rhs->name())
            {
                return lhs->name() < rhs->name();
            }
            return lhs->actor_id() < rhs->actor_id();
        });
    return actors;
}

GeometryData BuildGeometry(const mud::v1::LocalViewUpdate& view)
{
    std::unordered_set<std::int64_t> ground_coords;
    std::unordered_set<std::int64_t> wall_coords;
    BuildGroundAndWallCoords(view, ground_coords, wall_coords);

    const CoordBounds bounds =
        ComputeBounds(ground_coords, view.center_x(), view.center_y());
    ExpandGroundToBounds(ground_coords, bounds);

    GeometryData geometry;
    const float base_center_x =
        (static_cast<float>(bounds.min_x + bounds.max_x) * 0.5f) * kCellSize;
    const float base_center_z =
        (static_cast<float>(bounds.min_y + bounds.max_y) * 0.5f) * kCellSize;
    const float base_span_x =
        (static_cast<float>((bounds.max_x - bounds.min_x) + 1) +
         (2.0f * kBaseFloorMarginCells)) *
        kCellSize;
    const float base_span_z =
        (static_cast<float>((bounds.max_y - bounds.min_y) + 1) +
         (2.0f * kBaseFloorMarginCells)) *
        kCellSize;
    AddCubeSpec(
        geometry.floor_cubes,
        base_center_x,
        kBaseFloorCenterY,
        base_center_z,
        base_span_x,
        kBaseFloorThickness,
        base_span_z);

    for (const std::int64_t key : SortedCoords(ground_coords))
    {
        const float x = static_cast<float>(CoordXFromKey(key)) * kCellSize;
        const float z = static_cast<float>(CoordYFromKey(key)) * kCellSize;
        AddCubeSpec(
            geometry.floor_cubes,
            x,
            kFloorCenterY,
            z,
            kFloorTileSize,
            kFloorThickness,
            kFloorTileSize);
    }

    for (const std::int64_t key : SortedCoords(wall_coords))
    {
        const float x = static_cast<float>(CoordXFromKey(key)) * kCellSize;
        const float z = static_cast<float>(CoordYFromKey(key)) * kCellSize;
        AddCubeSpec(
            geometry.wall_cubes,
            x,
            kWallCenterY,
            z,
            kWallCubeSize,
            kWallHeight,
            kWallCubeSize);
    }

    for (const mud::v1::VisibleActor* actor : SortedActors(view))
    {
        const float x = static_cast<float>(actor->x()) * kCellSize;
        const float z = static_cast<float>(actor->y()) * kCellSize;
        if (actor->kind() == mud::v1::VisibleActor::KIND_NPC)
        {
            AddCubeSpec(
                geometry.npc_cubes,
                x,
                kGroundY + (kNpcHeight * 0.5f),
                z,
                kNpcWidth,
                kNpcHeight,
                kNpcWidth);
        }
        else
        {
            AddCubeSpec(
                geometry.player_cubes,
                x,
                kGroundY + (kPlayerHeight * 0.5f),
                z,
                kPlayerWidth,
                kPlayerHeight,
                kPlayerWidth);
        }
    }

    return geometry;
}

std::vector<CubeSpec> FinalizeCubeSpecs(std::vector<CubeSpec> cubes)
{
    if (!cubes.empty())
    {
        return cubes;
    }
    cubes.push_back(CubeSpec{
        100000.0f,
        -100000.0f,
        100000.0f,
        0.01f,
        0.01f,
        0.01f});
    return cubes;
}

MeshBuffers BuildMeshBuffers(const std::vector<CubeSpec>& cubes)
{
    MeshBuffers mesh;
    const std::vector<CubeSpec> final_cubes = FinalizeCubeSpecs(cubes);

    const auto emit_face = [&](const glm::vec3& a,
                               const glm::vec3& b,
                               const glm::vec3& c,
                               const glm::vec3& d,
                               const glm::vec3& normal) {
        const std::uint32_t base =
            static_cast<std::uint32_t>(mesh.positions.size() / 3u);
        const std::array<glm::vec3, 4> points{a, b, c, d};
        const std::array<glm::vec2, 4> uvs{{{0.0f, 0.0f},
                                            {1.0f, 0.0f},
                                            {1.0f, 1.0f},
                                            {0.0f, 1.0f}}};
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            mesh.positions.push_back(points[i].x);
            mesh.positions.push_back(points[i].y);
            mesh.positions.push_back(points[i].z);
            mesh.normals.push_back(normal.x);
            mesh.normals.push_back(normal.y);
            mesh.normals.push_back(normal.z);
            mesh.uvs.push_back(uvs[i].x);
            mesh.uvs.push_back(uvs[i].y);
        }
        mesh.indices.push_back(base + 0u);
        mesh.indices.push_back(base + 1u);
        mesh.indices.push_back(base + 2u);
        mesh.indices.push_back(base + 0u);
        mesh.indices.push_back(base + 2u);
        mesh.indices.push_back(base + 3u);
    };

    for (const CubeSpec& cube : final_cubes)
    {
        const float min_x = cube.tx - (cube.sx * 0.5f);
        const float max_x = cube.tx + (cube.sx * 0.5f);
        const float min_y = cube.ty - (cube.sy * 0.5f);
        const float max_y = cube.ty + (cube.sy * 0.5f);
        const float min_z = cube.tz - (cube.sz * 0.5f);
        const float max_z = cube.tz + (cube.sz * 0.5f);

        emit_face(
            glm::vec3(min_x, min_y, min_z),
            glm::vec3(max_x, min_y, min_z),
            glm::vec3(max_x, max_y, min_z),
            glm::vec3(min_x, max_y, min_z),
            glm::vec3(0.0f, 0.0f, -1.0f));
        emit_face(
            glm::vec3(max_x, min_y, max_z),
            glm::vec3(min_x, min_y, max_z),
            glm::vec3(min_x, max_y, max_z),
            glm::vec3(max_x, max_y, max_z),
            glm::vec3(0.0f, 0.0f, 1.0f));
        emit_face(
            glm::vec3(min_x, min_y, max_z),
            glm::vec3(min_x, min_y, min_z),
            glm::vec3(min_x, max_y, min_z),
            glm::vec3(min_x, max_y, max_z),
            glm::vec3(-1.0f, 0.0f, 0.0f));
        emit_face(
            glm::vec3(max_x, min_y, min_z),
            glm::vec3(max_x, min_y, max_z),
            glm::vec3(max_x, max_y, max_z),
            glm::vec3(max_x, max_y, min_z),
            glm::vec3(1.0f, 0.0f, 0.0f));
        emit_face(
            glm::vec3(min_x, min_y, max_z),
            glm::vec3(max_x, min_y, max_z),
            glm::vec3(max_x, min_y, min_z),
            glm::vec3(min_x, min_y, min_z),
            glm::vec3(0.0f, -1.0f, 0.0f));
        emit_face(
            glm::vec3(min_x, max_y, min_z),
            glm::vec3(max_x, max_y, min_z),
            glm::vec3(max_x, max_y, max_z),
            glm::vec3(min_x, max_y, max_z),
            glm::vec3(0.0f, 1.0f, 0.0f));
    }

    return mesh;
}

std::vector<std::uint8_t> ReadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string content = buffer.str();
    return std::vector<std::uint8_t>(content.begin(), content.end());
}

bool WriteBinaryFileIfChanged(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& content)
{
    const std::vector<std::uint8_t> existing = ReadBinaryFile(path);
    if (existing == content)
    {
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        throw std::runtime_error("Failed to write file: " + path.string());
    }
    out.write(
        reinterpret_cast<const char*>(content.data()),
        static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out.good())
    {
        throw std::runtime_error("Failed to flush file: " + path.string());
    }
    return true;
}

bool WriteTextFileIfChanged(
    const std::filesystem::path& path,
    const std::string& content)
{
    std::ifstream in(path, std::ios::binary);
    if (in.is_open())
    {
        std::ostringstream existing;
        existing << in.rdbuf();
        if (existing.str() == content)
        {
            return false;
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        throw std::runtime_error("Failed to write file: " + path.string());
    }
    out << content;
    out.flush();
    if (!out.good())
    {
        throw std::runtime_error("Failed to flush file: " + path.string());
    }
    return true;
}

void CopyFileIfChanged(
    const std::filesystem::path& source,
    const std::filesystem::path& destination)
{
    const std::vector<std::uint8_t> content = ReadBinaryFile(source);
    if (content.empty() && std::filesystem::file_size(source) != 0u)
    {
        throw std::runtime_error("Failed to read file: " + source.string());
    }

    std::filesystem::create_directories(destination.parent_path());
    WriteBinaryFileIfChanged(destination, content);
}

void CopyDirectoryContents(
    const std::filesystem::path& source_root,
    const std::filesystem::path& destination_root)
{
    if (!std::filesystem::exists(source_root) ||
        !std::filesystem::is_directory(source_root))
    {
        throw std::runtime_error(
            "Missing Frame asset directory: " + source_root.string());
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(source_root))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        const std::filesystem::path relative =
            std::filesystem::relative(entry.path(), source_root);
        CopyFileIfChanged(entry.path(), destination_root / relative);
    }
}

void EnsureFrameAssetsAvailable()
{
    static bool synced = false;
    if (synced)
    {
        return;
    }

    const std::filesystem::path frame_asset_root =
        std::filesystem::path("external") / "frame" / "asset";
    const std::filesystem::path asset_root = "asset";

    std::filesystem::create_directories(asset_root);
    CopyDirectoryContents(frame_asset_root / "shader", asset_root / "shader");
    CopyDirectoryContents(frame_asset_root / "cubemap", asset_root / "cubemap");
    CopyDirectoryContents(
        frame_asset_root / "material" / "plastic",
        asset_root / "material" / "plastic");

    synced = true;
}

void AlignBuffer(std::vector<std::uint8_t>& buffer)
{
    while ((buffer.size() % 4u) != 0u)
    {
        buffer.push_back(0u);
    }
}

template <typename T>
BufferViewInfo AppendBinary(
    std::vector<std::uint8_t>& buffer,
    const std::vector<T>& values,
    int target)
{
    AlignBuffer(buffer);
    BufferViewInfo info;
    info.offset = buffer.size();
    info.length = values.size() * sizeof(T);
    info.target = target;
    const auto* raw = reinterpret_cast<const std::uint8_t*>(values.data());
    buffer.insert(buffer.end(), raw, raw + info.length);
    return info;
}

std::pair<glm::vec3, glm::vec3> ComputePositionExtents(const std::vector<float>& positions)
{
    glm::vec3 min_value(
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    glm::vec3 max_value(
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest());
    for (std::size_t i = 0; i + 2 < positions.size(); i += 3)
    {
        min_value.x = std::min(min_value.x, positions[i + 0]);
        min_value.y = std::min(min_value.y, positions[i + 1]);
        min_value.z = std::min(min_value.z, positions[i + 2]);
        max_value.x = std::max(max_value.x, positions[i + 0]);
        max_value.y = std::max(max_value.y, positions[i + 1]);
        max_value.z = std::max(max_value.z, positions[i + 2]);
    }
    return {min_value, max_value};
}

std::string FormatFloat(float value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;
    return out.str();
}

std::string FormatVec3(const glm::vec3& value)
{
    return "[" + FormatFloat(value.x) + ", " + FormatFloat(value.y) + ", " +
           FormatFloat(value.z) + "]";
}

std::string FormatVec4(const glm::vec4& value)
{
    return "[" + FormatFloat(value.x) + ", " + FormatFloat(value.y) + ", " +
           FormatFloat(value.z) + ", " + FormatFloat(value.w) + "]";
}

void WriteGltfMesh(
    const std::filesystem::path& gltf_path,
    const std::vector<CubeSpec>& cubes,
    const MaterialSpec& material)
{
    const MeshBuffers mesh = BuildMeshBuffers(cubes);
    const std::uint32_t vertex_count =
        static_cast<std::uint32_t>(mesh.positions.size() / 3u);
    const std::uint32_t index_count =
        static_cast<std::uint32_t>(mesh.indices.size());
    const auto [min_position, max_position] =
        ComputePositionExtents(mesh.positions);

    std::vector<std::uint8_t> binary;
    const BufferViewInfo position_view =
        AppendBinary(binary, mesh.positions, 34962);
    const BufferViewInfo normal_view =
        AppendBinary(binary, mesh.normals, 34962);
    const BufferViewInfo uv_view =
        AppendBinary(binary, mesh.uvs, 34962);
    const BufferViewInfo index_view =
        AppendBinary(binary, mesh.indices, 34963);

    const std::filesystem::path bin_path = gltf_path.parent_path() /
                                           (gltf_path.stem().string() + ".bin");
    std::ostringstream json;
    json << "{\n";
    json << "  \"asset\": {\n";
    json << "    \"version\": \"2.0\",\n";
    json << "    \"generator\": \"grpcMUD\"\n";
    json << "  },\n";
    json << "  \"scene\": 0,\n";
    json << "  \"scenes\": [\n";
    json << "    {\n";
    json << "      \"nodes\": [0]\n";
    json << "    }\n";
    json << "  ],\n";
    json << "  \"nodes\": [\n";
    json << "    {\n";
    json << "      \"mesh\": 0,\n";
    json << "      \"name\": \"" << material.name << "Node\"\n";
    json << "    }\n";
    json << "  ],\n";
    json << "  \"materials\": [\n";
    json << "    {\n";
    json << "      \"name\": \"" << material.name << "\",\n";
    json << "      \"pbrMetallicRoughness\": {\n";
    json << "        \"baseColorFactor\": " << FormatVec4(material.base_color) << ",\n";
    json << "        \"metallicFactor\": " << FormatFloat(material.metallic) << ",\n";
    json << "        \"roughnessFactor\": " << FormatFloat(material.roughness) << "\n";
    json << "      }\n";
    json << "    }\n";
    json << "  ],\n";
    json << "  \"meshes\": [\n";
    json << "    {\n";
    json << "      \"name\": \"" << material.name << "Mesh\",\n";
    json << "      \"primitives\": [\n";
    json << "        {\n";
    json << "          \"attributes\": {\n";
    json << "            \"POSITION\": 0,\n";
    json << "            \"NORMAL\": 1,\n";
    json << "            \"TEXCOORD_0\": 2\n";
    json << "          },\n";
    json << "          \"indices\": 3,\n";
    json << "          \"material\": 0\n";
    json << "        }\n";
    json << "      ]\n";
    json << "    }\n";
    json << "  ],\n";
    json << "  \"buffers\": [\n";
    json << "    {\n";
    json << "      \"uri\": \"" << bin_path.filename().generic_string() << "\",\n";
    json << "      \"byteLength\": " << binary.size() << "\n";
    json << "    }\n";
    json << "  ],\n";
    json << "  \"bufferViews\": [\n";
    json << "    {\n";
    json << "      \"buffer\": 0,\n";
    json << "      \"byteOffset\": " << position_view.offset << ",\n";
    json << "      \"byteLength\": " << position_view.length << ",\n";
    json << "      \"target\": " << position_view.target << "\n";
    json << "    },\n";
    json << "    {\n";
    json << "      \"buffer\": 0,\n";
    json << "      \"byteOffset\": " << normal_view.offset << ",\n";
    json << "      \"byteLength\": " << normal_view.length << ",\n";
    json << "      \"target\": " << normal_view.target << "\n";
    json << "    },\n";
    json << "    {\n";
    json << "      \"buffer\": 0,\n";
    json << "      \"byteOffset\": " << uv_view.offset << ",\n";
    json << "      \"byteLength\": " << uv_view.length << ",\n";
    json << "      \"target\": " << uv_view.target << "\n";
    json << "    },\n";
    json << "    {\n";
    json << "      \"buffer\": 0,\n";
    json << "      \"byteOffset\": " << index_view.offset << ",\n";
    json << "      \"byteLength\": " << index_view.length << ",\n";
    json << "      \"target\": " << index_view.target << "\n";
    json << "    }\n";
    json << "  ],\n";
    json << "  \"accessors\": [\n";
    json << "    {\n";
    json << "      \"bufferView\": 0,\n";
    json << "      \"componentType\": 5126,\n";
    json << "      \"count\": " << vertex_count << ",\n";
    json << "      \"type\": \"VEC3\",\n";
    json << "      \"min\": " << FormatVec3(min_position) << ",\n";
    json << "      \"max\": " << FormatVec3(max_position) << "\n";
    json << "    },\n";
    json << "    {\n";
    json << "      \"bufferView\": 1,\n";
    json << "      \"componentType\": 5126,\n";
    json << "      \"count\": " << vertex_count << ",\n";
    json << "      \"type\": \"VEC3\"\n";
    json << "    },\n";
    json << "    {\n";
    json << "      \"bufferView\": 2,\n";
    json << "      \"componentType\": 5126,\n";
    json << "      \"count\": " << vertex_count << ",\n";
    json << "      \"type\": \"VEC2\"\n";
    json << "    },\n";
    json << "    {\n";
    json << "      \"bufferView\": 3,\n";
    json << "      \"componentType\": 5125,\n";
    json << "      \"count\": " << index_count << ",\n";
    json << "      \"type\": \"SCALAR\"\n";
    json << "    }\n";
    json << "  ]\n";
    json << "}\n";

    std::filesystem::create_directories(gltf_path.parent_path());
    WriteBinaryFileIfChanged(bin_path, binary);
    WriteTextFileIfChanged(gltf_path, json.str());
}

void WriteGeometryGltfs(const GeometryData& geometry)
{
    EnsureFrameAssetsAvailable();

    const std::filesystem::path model_root = std::filesystem::path("asset") / "model";
    std::filesystem::create_directories(model_root);

    WriteGltfMesh(
        model_root / "grpcmud_floor.gltf",
        geometry.floor_cubes,
        MaterialSpec{
            .name = "FloorMaterial",
            .base_color = glm::vec4(0.26f, 0.34f, 0.18f, 1.0f),
            .roughness = 0.96f,
            .metallic = 0.02f});
    WriteGltfMesh(
        model_root / "grpcmud_wall.gltf",
        geometry.wall_cubes,
        MaterialSpec{
            .name = "WallMaterial",
            .base_color = glm::vec4(0.43f, 0.39f, 0.35f, 1.0f),
            .roughness = 0.92f,
            .metallic = 0.05f});
    WriteGltfMesh(
        model_root / "grpcmud_player.gltf",
        geometry.player_cubes,
        MaterialSpec{
            .name = "PlayerMaterial",
            .base_color = glm::vec4(0.24f, 0.58f, 0.92f, 1.0f),
            .roughness = 0.42f,
            .metallic = 0.02f});
    WriteGltfMesh(
        model_root / "grpcmud_npc.gltf",
        geometry.npc_cubes,
        MaterialSpec{
            .name = "NpcMaterial",
            .base_color = glm::vec4(0.92f, 0.12f, 0.10f, 1.0f),
            .roughness = 0.58f,
            .metallic = 0.03f});
}

frame::proto::Texture MakeRenderTexture(const std::string& name)
{
    frame::proto::Texture texture;
    texture.set_name(name);
    texture.set_cubemap(false);
    texture.mutable_pixel_element_size()->set_value(frame::proto::PixelElementSize::BYTE);
    texture.mutable_pixel_structure()->set_value(frame::proto::PixelStructure::RGB);
    texture.mutable_size()->set_x(-1);
    texture.mutable_size()->set_y(-1);
    return texture;
}

frame::proto::Texture MakeEnvironmentTexture(
    const std::string& name,
    const std::string& file_name)
{
    frame::proto::Texture texture;
    texture.set_name(name);
    texture.set_cubemap(true);
    texture.mutable_pixel_element_size()->set_value(frame::proto::PixelElementSize::FLOAT);
    texture.mutable_pixel_structure()->set_value(frame::proto::PixelStructure::RGB);
    texture.set_file_name(file_name);
    return texture;
}

frame::proto::Texture MakeSolidTexture(
    const std::string& name,
    const std::array<float, 4>& color,
    frame::proto::PixelElementSize::Enum element_size)
{
    frame::proto::Texture texture;
    texture.set_name(name);
    texture.set_cubemap(false);
    texture.mutable_size()->set_x(1);
    texture.mutable_size()->set_y(1);
    texture.mutable_pixel_structure()->set_value(frame::proto::PixelStructure::RGB_ALPHA);
    texture.mutable_pixel_element_size()->set_value(element_size);

    if (element_size == frame::proto::PixelElementSize::FLOAT)
    {
        texture.set_pixels(
            reinterpret_cast<const char*>(color.data()),
            static_cast<int>(color.size() * sizeof(float)));
        return texture;
    }

    const std::array<std::uint8_t, 4> pixels = {
        static_cast<std::uint8_t>(std::clamp(color[0], 0.0f, 1.0f) * 255.0f),
        static_cast<std::uint8_t>(std::clamp(color[1], 0.0f, 1.0f) * 255.0f),
        static_cast<std::uint8_t>(std::clamp(color[2], 0.0f, 1.0f) * 255.0f),
        static_cast<std::uint8_t>(std::clamp(color[3], 0.0f, 1.0f) * 255.0f)};
    texture.set_pixels(
        reinterpret_cast<const char*>(pixels.data()),
        static_cast<int>(pixels.size()));
    return texture;
}

void AddDefaultRaytraceTextures(frame::proto::Level* level_proto)
{
    const auto add_byte_texture =
        [&](const std::string& name, const std::array<float, 4>& color) {
            *level_proto->add_textures() = MakeSolidTexture(
                name,
                color,
                frame::proto::PixelElementSize::BYTE);
        };
    const auto add_float_texture =
        [&](const std::string& name, const std::array<float, 4>& color) {
            *level_proto->add_textures() = MakeSolidTexture(
                name,
                color,
                frame::proto::PixelElementSize::FLOAT);
        };

    const std::array<float, 4> white = {1.0f, 1.0f, 1.0f, 1.0f};
    const std::array<float, 4> normal = {0.5f, 0.5f, 1.0f, 1.0f};
    const std::array<float, 4> black = {0.0f, 0.0f, 0.0f, 1.0f};
    const std::array<float, 4> ior = {1.5f, 1.5f, 1.5f, 1.0f};
    const std::array<float, 4> far_attenuation = {1000000.0f, 1000000.0f, 1000000.0f, 1.0f};

    add_byte_texture("albedo_texture", white);
    add_byte_texture("Color", white);
    add_byte_texture("normal_texture", normal);
    add_byte_texture("roughness_texture", white);
    add_byte_texture("metallic_texture", black);
    add_byte_texture("ao_texture", white);
    add_byte_texture("specular_factor_texture", white);
    add_byte_texture("specular_color_texture", white);
    add_byte_texture("transmission_texture", black);
    add_float_texture("ior_texture", ior);
    add_float_texture("thickness_texture", black);
    add_byte_texture("attenuation_color_texture", white);
    add_float_texture("attenuation_distance_texture", far_attenuation);

    add_byte_texture("opaque_albedo_texture", white);
    add_byte_texture("opaque_normal_texture", normal);
    add_byte_texture("opaque_roughness_texture", white);
    add_byte_texture("opaque_metallic_texture", black);
    add_byte_texture("opaque_ao_texture", white);
    add_byte_texture("opaque_specular_factor_texture", white);
    add_byte_texture("opaque_specular_color_texture", white);

    add_byte_texture("transmissive_albedo_texture", white);
    add_byte_texture("transmissive_normal_texture", normal);
    add_byte_texture("transmissive_roughness_texture", white);
    add_byte_texture("transmissive_metallic_texture", black);
    add_byte_texture("transmissive_ao_texture", white);
    add_byte_texture("transmissive_transmission_texture", black);
    add_float_texture("transmissive_ior_texture", ior);
    add_float_texture("transmissive_thickness_texture", black);
    add_byte_texture("transmissive_attenuation_color_texture", white);
    add_float_texture("transmissive_attenuation_distance_texture", far_attenuation);
}

void AddSceneFileMeshNode(
    frame::proto::SceneTree* scene_tree,
    const std::string& name,
    const std::string& parent,
    const std::string& file_name,
    frame::proto::NodeMesh::RenderTimeEnum render_time)
{
    auto* node = scene_tree->add_node_meshes();
    node->set_name(name);
    node->set_parent(parent);
    node->set_file_name(file_name);
    node->set_render_time_enum(render_time);
}

void AddSceneEnumMeshNode(
    frame::proto::SceneTree* scene_tree,
    const std::string& name,
    const std::string& parent,
    frame::proto::NodeMesh::MeshEnum mesh_enum,
    frame::proto::NodeMesh::RenderTimeEnum render_time)
{
    auto* node = scene_tree->add_node_meshes();
    node->set_name(name);
    node->set_parent(parent);
    node->set_mesh_enum(mesh_enum);
    node->set_render_time_enum(render_time);
}

void AddIdentityMatrixNode(
    frame::proto::SceneTree* scene_tree,
    const std::string& name,
    const std::string& parent = std::string{})
{
    auto* matrix_node = scene_tree->add_node_matrices();
    matrix_node->set_name(name);
    matrix_node->set_matrix_type_enum(frame::proto::NodeMatrix::STATIC_MATRIX);
    if (!parent.empty())
    {
        matrix_node->set_parent(parent);
    }
    auto* matrix = matrix_node->mutable_matrix();
    matrix->set_m11(1.0f);
    matrix->set_m22(1.0f);
    matrix->set_m33(1.0f);
    matrix->set_m44(1.0f);
}

frame::proto::SceneTree MakeSceneTree(const mud::v1::LocalViewUpdate& view)
{
    const float cam_x = static_cast<float>(view.center_x()) * kCellSize;
    const float cam_z = static_cast<float>(view.center_y()) * kCellSize;
    const auto facing = viewmath::ToFacingVec(view.facing());
    const float target_x = cam_x + (facing.x * kCameraForwardDistance);
    const float target_z = cam_z + (facing.z * kCameraForwardDistance);

    frame::proto::SceneTree scene_tree;
    scene_tree.set_default_root_name("root");
    scene_tree.set_default_camera_name("camera");

    AddIdentityMatrixNode(&scene_tree, "root");
    AddIdentityMatrixNode(&scene_tree, "env_holder", "root");
    AddIdentityMatrixNode(&scene_tree, "mesh_holder", "root");

    auto* camera = scene_tree.add_node_cameras();
    camera->set_name("camera");
    camera->set_parent("root");
    camera->mutable_position()->set_x(cam_x);
    camera->mutable_position()->set_y(kCameraHeight);
    camera->mutable_position()->set_z(cam_z);
    camera->mutable_target()->set_x(target_x);
    camera->mutable_target()->set_y(kCameraHeight);
    camera->mutable_target()->set_z(target_z);
    camera->mutable_up()->set_x(0.0f);
    camera->mutable_up()->set_y(1.0f);
    camera->mutable_up()->set_z(0.0f);
    camera->set_fov_degrees(68.0f);
    camera->set_aspect_ratio(1.777777f);
    camera->set_near_clip(kCameraNearClip);
    camera->set_far_clip(200.0f);

    AddSceneEnumMeshNode(
        &scene_tree,
        "CubeMapMesh",
        "env_holder",
        frame::proto::NodeMesh::CUBE,
        frame::proto::NodeMesh::SKYBOX_RENDER_TIME);
    AddSceneEnumMeshNode(
        &scene_tree,
        "RayTracingRendering",
        "root",
        frame::proto::NodeMesh::QUAD,
        frame::proto::NodeMesh::SCENE_RENDER_TIME);
    AddSceneFileMeshNode(
        &scene_tree,
        "FloorMesh",
        "mesh_holder",
        "grpcmud_floor.gltf",
        frame::proto::NodeMesh::PRE_RENDER_TIME);
    AddSceneFileMeshNode(
        &scene_tree,
        "WallMesh",
        "mesh_holder",
        "grpcmud_wall.gltf",
        frame::proto::NodeMesh::PRE_RENDER_TIME);
    AddSceneFileMeshNode(
        &scene_tree,
        "PlayerMesh",
        "mesh_holder",
        "grpcmud_player.gltf",
        frame::proto::NodeMesh::PRE_RENDER_TIME);
    AddSceneFileMeshNode(
        &scene_tree,
        "NpcMesh",
        "mesh_holder",
        "grpcmud_npc.gltf",
        frame::proto::NodeMesh::PRE_RENDER_TIME);

    auto* sun = scene_tree.add_node_lights();
    sun->set_name("sun");
    sun->set_parent("root");
    sun->set_light_type(frame::proto::NodeLight::DIRECTIONAL_LIGHT);
    sun->set_shadow_type(frame::proto::NodeLight::HARD_SHADOW);
    sun->mutable_direction()->set_x(0.42f);
    sun->mutable_direction()->set_y(-1.00f);
    sun->mutable_direction()->set_z(0.28f);
    sun->mutable_color()->set_x(1.00f);
    sun->mutable_color()->set_y(0.95f);
    sun->mutable_color()->set_z(0.90f);

    auto* torch = scene_tree.add_node_lights();
    torch->set_name("torch");
    torch->set_parent("root");
    torch->set_light_type(frame::proto::NodeLight::POINT_LIGHT);
    torch->set_shadow_type(frame::proto::NodeLight::NO_SHADOW);
    torch->mutable_position()->set_x(cam_x);
    torch->mutable_position()->set_y(kCameraHeight + 0.25f);
    torch->mutable_position()->set_z(cam_z);
    torch->mutable_color()->set_x(1.00f);
    torch->mutable_color()->set_y(0.72f);
    torch->mutable_color()->set_z(0.46f);

    return scene_tree;
}

std::uint8_t MakeOpenMask(const mud::v1::VisibleSquare& square)
{
    std::uint8_t mask = 0;
    if (square.open_north())
    {
        mask |= 0x1u;
    }
    if (square.open_east())
    {
        mask |= 0x2u;
    }
    if (square.open_south())
    {
        mask |= 0x4u;
    }
    if (square.open_west())
    {
        mask |= 0x8u;
    }
    return mask;
}

} // namespace

std::uint64_t ComputeRelativeSceneSignature(const mud::v1::LocalViewUpdate& view)
{
    std::vector<RelativeSquareSig> square_sigs;
    square_sigs.reserve(static_cast<std::size_t>(view.squares_size()));
    for (const auto& square : view.squares())
    {
        square_sigs.push_back(RelativeSquareSig{
            square.x() - view.center_x(),
            square.y() - view.center_y(),
            static_cast<std::uint8_t>(square.kind()),
            MakeOpenMask(square)});
    }
    std::sort(
        square_sigs.begin(),
        square_sigs.end(),
        [](const RelativeSquareSig& lhs, const RelativeSquareSig& rhs) {
            if (lhs.rel_x != rhs.rel_x)
            {
                return lhs.rel_x < rhs.rel_x;
            }
            if (lhs.rel_y != rhs.rel_y)
            {
                return lhs.rel_y < rhs.rel_y;
            }
            if (lhs.kind != rhs.kind)
            {
                return lhs.kind < rhs.kind;
            }
            return lhs.open_mask < rhs.open_mask;
        });

    std::vector<RelativeActorSig> actor_sigs;
    actor_sigs.reserve(static_cast<std::size_t>(view.actors_size()));
    for (const auto& actor : view.actors())
    {
        if (actor.kind() == mud::v1::VisibleActor::KIND_SELF)
        {
            continue;
        }
        actor_sigs.push_back(RelativeActorSig{
            actor.x() - view.center_x(),
            actor.y() - view.center_y(),
            static_cast<std::uint8_t>(actor.kind()),
            static_cast<std::uint8_t>(actor.facing())});
    }
    std::sort(
        actor_sigs.begin(),
        actor_sigs.end(),
        [](const RelativeActorSig& lhs, const RelativeActorSig& rhs) {
            if (lhs.rel_x != rhs.rel_x)
            {
                return lhs.rel_x < rhs.rel_x;
            }
            if (lhs.rel_y != rhs.rel_y)
            {
                return lhs.rel_y < rhs.rel_y;
            }
            if (lhs.kind != rhs.kind)
            {
                return lhs.kind < rhs.kind;
            }
            return lhs.facing < rhs.facing;
        });

    std::uint64_t hash = kFnvOffset;
    hash = Fnv1aAppend(hash, static_cast<std::uint64_t>(square_sigs.size()));
    for (const RelativeSquareSig& sig : square_sigs)
    {
        hash = Fnv1aAppend(hash, static_cast<std::uint32_t>(sig.rel_x));
        hash = Fnv1aAppend(hash, static_cast<std::uint32_t>(sig.rel_y));
        hash = Fnv1aAppend(hash, sig.kind);
        hash = Fnv1aAppend(hash, sig.open_mask);
    }
    hash = Fnv1aAppend(hash, static_cast<std::uint64_t>(actor_sigs.size()));
    for (const RelativeActorSig& sig : actor_sigs)
    {
        hash = Fnv1aAppend(hash, static_cast<std::uint32_t>(sig.rel_x));
        hash = Fnv1aAppend(hash, static_cast<std::uint32_t>(sig.rel_y));
        hash = Fnv1aAppend(hash, sig.kind);
        hash = Fnv1aAppend(hash, sig.facing);
    }
    return hash;
}

SceneLevelBuildResult BuildLevelProtoFromView(const mud::v1::LocalViewUpdate& view)
{
    const GeometryData geometry = BuildGeometry(view);
    WriteGeometryGltfs(geometry);

    const std::uint64_t geometry_hash = HashGeometry(geometry);
    std::ostringstream revision;
    revision << std::hex << std::setw(16) << std::setfill('0') << geometry_hash;

    SceneLevelBuildResult result;
    result.geometry_hash = geometry_hash;
    result.level_proto.set_name("grpcMUDRaytrace-" + revision.str());
    result.level_proto.set_default_texture_name("albedo");
    *result.level_proto.add_textures() = MakeRenderTexture("albedo");
    *result.level_proto.add_textures() =
        MakeEnvironmentTexture("skybox", kSkyboxPath);
    *result.level_proto.add_textures() =
        MakeEnvironmentTexture("skybox_env", kSkyboxEnvPath);
    AddDefaultRaytraceTextures(&result.level_proto);
    *result.level_proto.mutable_scene_tree() = MakeSceneTree(view);
    return result;
}

SceneLevelBuildResult BuildBootstrapLevelProto()
{
    mud::v1::LocalViewUpdate bootstrap;
    bootstrap.set_center_x(0);
    bootstrap.set_center_y(0);
    bootstrap.set_facing(mud::v1::DIRECTION_NORTH);
    auto* square = bootstrap.add_squares();
    square->set_square_id("bootstrap");
    square->set_x(0);
    square->set_y(0);
    square->set_kind(mud::v1::SQUARE_KIND_FLOOR);
    square->set_open_north(true);
    square->set_open_east(true);
    square->set_open_south(true);
    square->set_open_west(true);
    return BuildLevelProtoFromView(bootstrap);
}

} // namespace grpcmud::client::scene
