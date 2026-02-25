#include "SceneLevelBuilder.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
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
        (static_cast<float>((bounds.max_x - bounds.min_x) + 1) + (2.0f * kBaseFloorMarginCells)) *
        kCellSize;
    const float base_span_z =
        (static_cast<float>((bounds.max_y - bounds.min_y) + 1) + (2.0f * kBaseFloorMarginCells)) *
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

void WriteRaytraceObj(const std::filesystem::path& path, const std::vector<CubeSpec>& cubes)
{
    std::vector<CubeSpec> final_cubes = cubes;
    if (final_cubes.empty())
    {
        final_cubes.push_back(CubeSpec{100000.0f, -100000.0f, 100000.0f, 0.01f, 0.01f, 0.01f});
    }

    std::ostringstream generated;
    generated << "# generated by grpcMUD client\n";
    generated << "o mesh\n";
    generated << std::fixed << std::setprecision(5);

    int next_index = 1;
    const auto emit_quad = [&](const glm::vec3& a,
                               const glm::vec3& b,
                               const glm::vec3& c,
                               const glm::vec3& d,
                               const glm::vec3& normal) {
        const int base = next_index;
        const std::array<glm::vec3, 4> points{a, b, c, d};
        const std::array<glm::vec2, 4> uv{{{0.0f, 0.0f},
                                           {1.0f, 0.0f},
                                           {1.0f, 1.0f},
                                           {0.0f, 1.0f}}};
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            generated << "v " << points[i].x << ' ' << points[i].y << ' ' << points[i].z << '\n';
            generated << "vt " << uv[i].x << ' ' << uv[i].y << '\n';
            generated << "vn " << normal.x << ' ' << normal.y << ' ' << normal.z << '\n';
        }
        generated << "f " << base << '/' << base << '/' << base << ' ' << (base + 1) << '/'
                  << (base + 1) << '/' << (base + 1) << ' ' << (base + 2) << '/'
                  << (base + 2) << '/' << (base + 2) << '\n';
        generated << "f " << base << '/' << base << '/' << base << ' ' << (base + 2) << '/'
                  << (base + 2) << '/' << (base + 2) << ' ' << (base + 3) << '/'
                  << (base + 3) << '/' << (base + 3) << '\n';
        next_index += 4;
    };

    for (const CubeSpec& cube : final_cubes)
    {
        const float min_x = cube.tx - (cube.sx * 0.5f);
        const float max_x = cube.tx + (cube.sx * 0.5f);
        const float min_y = cube.ty - (cube.sy * 0.5f);
        const float max_y = cube.ty + (cube.sy * 0.5f);
        const float min_z = cube.tz - (cube.sz * 0.5f);
        const float max_z = cube.tz + (cube.sz * 0.5f);

        emit_quad(
            glm::vec3(min_x, min_y, min_z),
            glm::vec3(max_x, min_y, min_z),
            glm::vec3(max_x, max_y, min_z),
            glm::vec3(min_x, max_y, min_z),
            glm::vec3(0.0f, 0.0f, -1.0f));
        emit_quad(
            glm::vec3(max_x, min_y, max_z),
            glm::vec3(min_x, min_y, max_z),
            glm::vec3(min_x, max_y, max_z),
            glm::vec3(max_x, max_y, max_z),
            glm::vec3(0.0f, 0.0f, 1.0f));
        emit_quad(
            glm::vec3(min_x, min_y, max_z),
            glm::vec3(min_x, min_y, min_z),
            glm::vec3(min_x, max_y, min_z),
            glm::vec3(min_x, max_y, max_z),
            glm::vec3(-1.0f, 0.0f, 0.0f));
        emit_quad(
            glm::vec3(max_x, min_y, min_z),
            glm::vec3(max_x, min_y, max_z),
            glm::vec3(max_x, max_y, max_z),
            glm::vec3(max_x, max_y, min_z),
            glm::vec3(1.0f, 0.0f, 0.0f));
        emit_quad(
            glm::vec3(min_x, min_y, max_z),
            glm::vec3(max_x, min_y, max_z),
            glm::vec3(max_x, min_y, min_z),
            glm::vec3(min_x, min_y, min_z),
            glm::vec3(0.0f, -1.0f, 0.0f));
        emit_quad(
            glm::vec3(min_x, max_y, min_z),
            glm::vec3(max_x, max_y, min_z),
            glm::vec3(max_x, max_y, max_z),
            glm::vec3(min_x, max_y, max_z),
            glm::vec3(0.0f, 1.0f, 0.0f));
    }

    const std::string generated_obj = generated.str();
    {
        std::ifstream current(path, std::ios::binary);
        if (current.is_open())
        {
            std::ostringstream existing;
            existing << current.rdbuf();
            if (existing.str() == generated_obj)
            {
                return;
            }
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        throw std::runtime_error("Failed to write OBJ file: " + path.string());
    }
    out << generated_obj;
    out.flush();
    if (!out.good())
    {
        throw std::runtime_error("Failed to flush OBJ file: " + path.string());
    }
}

void WriteGeometryObjs(const GeometryData& geometry)
{
    const std::filesystem::path model_root = std::filesystem::path("asset") / "model";
    std::filesystem::create_directories(model_root);
    WriteRaytraceObj(model_root / "grpcmud_floor.obj", geometry.floor_cubes);
    WriteRaytraceObj(model_root / "grpcmud_wall.obj", geometry.wall_cubes);
    WriteRaytraceObj(model_root / "grpcmud_player.obj", geometry.player_cubes);
    WriteRaytraceObj(model_root / "grpcmud_npc.obj", geometry.npc_cubes);
}

frame::proto::Texture MakeTexture(std::string name, std::optional<std::string> file_name = std::nullopt)
{
    frame::proto::Texture texture;
    texture.set_name(std::move(name));
    texture.set_cubemap(false);
    texture.mutable_pixel_element_size()->set_value(frame::proto::PixelElementSize::BYTE);
    texture.mutable_pixel_structure()->set_value(frame::proto::PixelStructure::RGB);
    if (file_name.has_value())
    {
        texture.set_file_name(*file_name);
    }
    else
    {
        texture.mutable_size()->set_x(-1);
        texture.mutable_size()->set_y(-1);
    }
    return texture;
}

void AddBinding(
    frame::proto::Program* program,
    const std::string& name,
    std::uint32_t binding,
    frame::proto::ProgramBinding::BindingType binding_type)
{
    auto* program_binding = program->add_bindings();
    program_binding->set_name(name);
    program_binding->set_binding(binding);
    program_binding->set_binding_type(binding_type);
    program_binding->add_stages(frame::proto::FRAGMENT);
}

void AddUniform(
    frame::proto::Program* program,
    const std::string& name,
    frame::proto::Uniform::UniformEnum uniform_enum)
{
    auto* uniform = program->add_uniforms();
    uniform->set_name(name);
    uniform->set_uniform_enum(uniform_enum);
}

frame::proto::Program MakeRayTraceProgram()
{
    frame::proto::Program program;
    program.set_name("RayTraceProgram");
    program.add_output_texture_names("billboard");
    program.add_input_texture_names("ground_texture");
    program.add_input_texture_names("wall_texture");
    program.mutable_input_scene_type()->set_value(frame::proto::SceneType::QUAD);
    program.set_shader_vertex("grpcmud_raytrace.vert");
    program.set_shader_fragment("grpcmud_raytrace.frag");

    AddBinding(
        &program,
        "ground_texture",
        0,
        frame::proto::ProgramBinding::COMBINED_IMAGE_SAMPLER);
    AddBinding(
        &program,
        "wall_texture",
        1,
        frame::proto::ProgramBinding::COMBINED_IMAGE_SAMPLER);
    AddBinding(
        &program,
        "FloorTriangles",
        2,
        frame::proto::ProgramBinding::STORAGE_BUFFER);
    AddBinding(
        &program,
        "WallTriangles",
        3,
        frame::proto::ProgramBinding::STORAGE_BUFFER);
    AddBinding(
        &program,
        "PlayerTriangles",
        4,
        frame::proto::ProgramBinding::STORAGE_BUFFER);
    AddBinding(
        &program,
        "NpcTriangles",
        5,
        frame::proto::ProgramBinding::STORAGE_BUFFER);
    AddBinding(
        &program,
        "UniformBlock",
        6,
        frame::proto::ProgramBinding::UNIFORM_BUFFER);

    AddUniform(&program, "projection_inv", frame::proto::Uniform::PROJECTION_INV_MAT4);
    AddUniform(&program, "view_inv", frame::proto::Uniform::VIEW_INV_MAT4);
    AddUniform(&program, "camera_position", frame::proto::Uniform::CAMERA_POSITION_VEC3);
    AddUniform(&program, "model_inv", frame::proto::Uniform::MODEL_INV_MAT4);
    return program;
}

frame::proto::Program MakeRayTracePreprocessProgram()
{
    frame::proto::Program program;
    program.set_name("RayTracePreprocessProgram");
    program.mutable_input_scene_type()->set_value(frame::proto::SceneType::SCENE);
    program.set_shader_vertex("grpcmud_preprocess.vert");
    program.set_shader_fragment("grpcmud_preprocess.frag");

    AddUniform(&program, "projection", frame::proto::Uniform::PROJECTION_MAT4);
    AddUniform(&program, "view", frame::proto::Uniform::VIEW_MAT4);
    AddUniform(&program, "model", frame::proto::Uniform::MODEL_MAT4);
    return program;
}

frame::proto::Material MakeRayTraceMaterial()
{
    frame::proto::Material material;
    material.set_name("RayTraceMaterial");
    material.set_program_name("RayTraceProgram");
    material.add_texture_names("ground_texture");
    material.add_texture_names("wall_texture");
    material.add_inner_names("ground_texture");
    material.add_inner_names("wall_texture");
    material.add_buffer_names("FloorMesh.0.triangle");
    material.add_buffer_names("WallMesh.0.triangle");
    material.add_buffer_names("PlayerMesh.0.triangle");
    material.add_buffer_names("NpcMesh.0.triangle");
    material.add_inner_buffer_names("FloorTriangles");
    material.add_inner_buffer_names("WallTriangles");
    material.add_inner_buffer_names("PlayerTriangles");
    material.add_inner_buffer_names("NpcTriangles");
    return material;
}

frame::proto::Material MakeRayTracePreprocessMaterial()
{
    frame::proto::Material material;
    material.set_name("RayTracePreprocessMaterial");
    material.set_program_name("RayTracePreprocessProgram");
    return material;
}

void AddStaticMeshNode(
    frame::proto::SceneTree* scene_tree,
    const std::string& name,
    const std::string& material_name,
    std::optional<std::string> file_name,
    std::optional<frame::proto::NodeStaticMesh::MeshEnum> mesh_enum,
    frame::proto::NodeStaticMesh::RenderTimeEnum render_time)
{
    auto* node = scene_tree->add_node_static_meshes();
    node->set_name(name);
    node->set_parent("root");
    node->set_material_name(material_name);
    node->set_render_time_enum(render_time);
    if (mesh_enum.has_value())
    {
        node->set_mesh_enum(*mesh_enum);
    }
    if (file_name.has_value())
    {
        node->set_file_name(*file_name);
    }
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

    auto* root_matrix = scene_tree.add_node_matrices();
    root_matrix->set_name("root");
    auto* matrix = root_matrix->mutable_matrix();
    matrix->set_m11(1.0f);
    matrix->set_m22(1.0f);
    matrix->set_m33(1.0f);
    matrix->set_m44(1.0f);

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

    AddStaticMeshNode(
        &scene_tree,
        "RayTracingRendering",
        "RayTraceMaterial",
        std::nullopt,
        frame::proto::NodeStaticMesh::QUAD,
        frame::proto::NodeStaticMesh::SCENE_RENDER_TIME);
    AddStaticMeshNode(
        &scene_tree,
        "FloorMesh",
        "RayTracePreprocessMaterial",
        "grpcmud_floor.obj",
        std::nullopt,
        frame::proto::NodeStaticMesh::PRE_RENDER_TIME);
    AddStaticMeshNode(
        &scene_tree,
        "WallMesh",
        "RayTracePreprocessMaterial",
        "grpcmud_wall.obj",
        std::nullopt,
        frame::proto::NodeStaticMesh::PRE_RENDER_TIME);
    AddStaticMeshNode(
        &scene_tree,
        "PlayerMesh",
        "RayTracePreprocessMaterial",
        "grpcmud_player.obj",
        std::nullopt,
        frame::proto::NodeStaticMesh::PRE_RENDER_TIME);
    AddStaticMeshNode(
        &scene_tree,
        "NpcMesh",
        "RayTracePreprocessMaterial",
        "grpcmud_npc.obj",
        std::nullopt,
        frame::proto::NodeStaticMesh::PRE_RENDER_TIME);

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
    WriteGeometryObjs(geometry);

    const std::uint64_t geometry_hash = HashGeometry(geometry);
    std::ostringstream revision;
    revision << std::hex << std::setw(16) << std::setfill('0') << geometry_hash;

    SceneLevelBuildResult result;
    result.geometry_hash = geometry_hash;
    result.level_proto.set_name("grpcMUDRaytrace-" + revision.str());
    result.level_proto.set_default_texture_name("billboard");

    *result.level_proto.add_textures() = MakeTexture("billboard");
    *result.level_proto.add_textures() =
        MakeTexture("ground_texture", "asset/texture/grpcmud_ground_grass.png");
    *result.level_proto.add_textures() =
        MakeTexture("wall_texture", "asset/texture/grpcmud_wall_rock.jpg");

    *result.level_proto.add_programs() = MakeRayTraceProgram();
    *result.level_proto.add_programs() = MakeRayTracePreprocessProgram();

    *result.level_proto.add_materials() = MakeRayTraceMaterial();
    *result.level_proto.add_materials() = MakeRayTracePreprocessMaterial();
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
