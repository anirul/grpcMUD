
#include "ClientApp.hpp"

#include <SDL3/SDL_keycode.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "frame/api.h"
#include "frame/file/file_system.h"
#include "frame/gui/draw_gui_factory.h"
#include "frame/gui/gui_window_interface.h"
#include "frame/json/parse_level.h"
#include "frame/logger.h"
#include "frame/vulkan/device.h"
#include "frame/vulkan/window_factory.h"
#include "frame/window_factory.h"
#include "frame/window_interface.h"

ABSL_FLAG(std::string, server_address, "localhost:50051", "gRPC server address.");
ABSL_FLAG(std::string, player_name, "",
          "Player name to join as. If empty, the client prompts interactively.");
ABSL_FLAG(std::string, render_api, "opengl",
          "Rendering backend for the client (opengl|vulkan).");

namespace grpcmud::client
{
namespace
{
constexpr std::size_t kMaxLogLines = 128;
constexpr glm::uvec2 kDefaultWindowSize{1280u, 720u};
constexpr float kCellSize = 2.0f;
constexpr float kHalfCell = kCellSize * 0.5f;
constexpr float kFloorThickness = 0.10f;
constexpr float kFloorTileSize = kCellSize + 0.02f;
constexpr float kFloorCenterY = -0.62f;
constexpr float kGroundY = kFloorCenterY + (kFloorThickness * 0.5f);
constexpr float kBaseFloorThickness = 0.08f;
constexpr float kBaseFloorCenterY = -0.72f;
constexpr float kBaseFloorMarginCells = 2.0f;
constexpr float kWallThickness = 0.18f;
constexpr float kWallHeight = 2.3f;
constexpr float kWallCenterY = kGroundY + (kWallHeight * 0.5f) - 0.04f;
constexpr float kWallCubeSize = kCellSize + 0.02f;
constexpr float kPlayerHeight = 1.75f;
constexpr float kPlayerWidth = 0.55f;
constexpr float kNpcHeight = 1.55f;
constexpr float kNpcWidth = 0.65f;
constexpr float kCameraHeight = 0.92f;
constexpr float kCameraForwardDistance = 2.0f;
constexpr float kCameraNearClip = 0.12f;
constexpr float kPi = 3.14159265358979323846f;
constexpr std::uint64_t kMinTickIntervalEstimateMs = 100;
constexpr std::uint64_t kMaxTickIntervalEstimateMs = 1500;

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

struct FacingVec
{
    float x = 0.0f;
    float z = -1.0f;
};

FacingVec ToFacingVec(mud::v1::Direction direction)
{
    switch (direction)
    {
    case mud::v1::DIRECTION_NORTH:
        return {0.0f, -1.0f};
    case mud::v1::DIRECTION_EAST:
        return {1.0f, 0.0f};
    case mud::v1::DIRECTION_SOUTH:
        return {0.0f, 1.0f};
    case mud::v1::DIRECTION_WEST:
        return {-1.0f, 0.0f};
    case mud::v1::DIRECTION_UNSPECIFIED:
    default:
        return {0.0f, -1.0f};
    }
}

mud::v1::Direction TurnLeftDirection(mud::v1::Direction direction)
{
    switch (direction)
    {
    case mud::v1::DIRECTION_NORTH:
        return mud::v1::DIRECTION_WEST;
    case mud::v1::DIRECTION_WEST:
        return mud::v1::DIRECTION_SOUTH;
    case mud::v1::DIRECTION_SOUTH:
        return mud::v1::DIRECTION_EAST;
    case mud::v1::DIRECTION_EAST:
        return mud::v1::DIRECTION_NORTH;
    case mud::v1::DIRECTION_UNSPECIFIED:
    default:
        return mud::v1::DIRECTION_NORTH;
    }
}

mud::v1::Direction TurnRightDirection(mud::v1::Direction direction)
{
    switch (direction)
    {
    case mud::v1::DIRECTION_NORTH:
        return mud::v1::DIRECTION_EAST;
    case mud::v1::DIRECTION_EAST:
        return mud::v1::DIRECTION_SOUTH;
    case mud::v1::DIRECTION_SOUTH:
        return mud::v1::DIRECTION_WEST;
    case mud::v1::DIRECTION_WEST:
        return mud::v1::DIRECTION_NORTH;
    case mud::v1::DIRECTION_UNSPECIFIED:
    default:
        return mud::v1::DIRECTION_NORTH;
    }
}

std::pair<int, int> DirectionToGridDelta(mud::v1::Direction direction)
{
    switch (direction)
    {
    case mud::v1::DIRECTION_NORTH:
        return {0, -1};
    case mud::v1::DIRECTION_EAST:
        return {1, 0};
    case mud::v1::DIRECTION_SOUTH:
        return {0, 1};
    case mud::v1::DIRECTION_WEST:
        return {-1, 0};
    case mud::v1::DIRECTION_UNSPECIFIED:
    default:
        return {0, -1};
    }
}

const mud::v1::VisibleSquare* FindCenterSquare(const mud::v1::LocalViewUpdate& view)
{
    for (const auto& square : view.squares())
    {
        if (square.x() == view.center_x() && square.y() == view.center_y())
        {
            return &square;
        }
    }
    return nullptr;
}

bool CanPredictMoveFromCenter(const mud::v1::LocalViewUpdate& view, mud::v1::StepKind kind)
{
    const mud::v1::VisibleSquare* center = FindCenterSquare(view);
    if (!center)
    {
        return true;
    }

    mud::v1::Direction movement_direction = view.facing();
    if (kind == mud::v1::STEP_KIND_MOVE_BACKWARD)
    {
        movement_direction = TurnLeftDirection(TurnLeftDirection(movement_direction));
    }

    auto is_cell_occupied_by_actor = [&](int x, int y) {
        for (const auto& actor : view.actors())
        {
            if (actor.kind() == mud::v1::VisibleActor::KIND_SELF)
            {
                continue;
            }
            if (actor.x() == x && actor.y() == y)
            {
                return true;
            }
        }
        return false;
    };

    const auto [dx, dy] = DirectionToGridDelta(movement_direction);
    const int target_x = view.center_x() + dx;
    const int target_y = view.center_y() + dy;

    switch (movement_direction)
    {
    case mud::v1::DIRECTION_NORTH:
        return center->open_north() && !is_cell_occupied_by_actor(target_x, target_y);
    case mud::v1::DIRECTION_EAST:
        return center->open_east() && !is_cell_occupied_by_actor(target_x, target_y);
    case mud::v1::DIRECTION_SOUTH:
        return center->open_south() && !is_cell_occupied_by_actor(target_x, target_y);
    case mud::v1::DIRECTION_WEST:
        return center->open_west() && !is_cell_occupied_by_actor(target_x, target_y);
    case mud::v1::DIRECTION_UNSPECIFIED:
    default:
        return true;
    }
}

float DirectionToYawRadians(mud::v1::Direction direction)
{
    const FacingVec facing = ToFacingVec(direction);
    return std::atan2(facing.z, facing.x);
}

float NormalizeAngleRadians(float radians)
{
    const float two_pi = 2.0f * kPi;
    float wrapped = std::fmod(radians + kPi, two_pi);
    if (wrapped < 0.0f)
    {
        wrapped += two_pi;
    }
    return wrapped - kPi;
}

float LerpAngleRadians(float from, float to, float t)
{
    return from + (NormalizeAngleRadians(to - from) * t);
}

float SmoothStep(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - (2.0f * t));
}

bool IsSingleTickMoveOrTurn(const mud::v1::LocalViewUpdate& from,
                            const mud::v1::LocalViewUpdate& to)
{
    const int dx = to.center_x() - from.center_x();
    const int dy = to.center_y() - from.center_y();
    const int manhattan = std::abs(dx) + std::abs(dy);
    if (manhattan == 1)
    {
        return true;
    }

    if (manhattan != 0)
    {
        return false;
    }

    const float from_yaw = DirectionToYawRadians(from.facing());
    const float to_yaw = DirectionToYawRadians(to.facing());
    const float yaw_delta = std::abs(NormalizeAngleRadians(to_yaw - from_yaw));
    return yaw_delta > 0.01f && yaw_delta <= (kPi * 0.75f);
}

struct CubeSpec
{
    float tx = 0.0f;
    float ty = 0.0f;
    float tz = 0.0f;
    float sx = 1.0f;
    float sy = 1.0f;
    float sz = 1.0f;
};

void AddCubeSpec(std::vector<CubeSpec>& cubes, float tx, float ty, float tz, float sx, float sy,
                 float sz)
{
    cubes.push_back(CubeSpec{tx, ty, tz, sx, sy, sz});
}

std::uint64_t Fnv1aAppend(std::uint64_t hash, std::uint64_t value)
{
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
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
    std::uint64_t hash = 1469598103934665603ull;
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
    const auto emit_quad = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                               const glm::vec3& d, const glm::vec3& normal)
    {
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

        emit_quad(glm::vec3(min_x, min_y, min_z), glm::vec3(max_x, min_y, min_z),
                  glm::vec3(max_x, max_y, min_z), glm::vec3(min_x, max_y, min_z),
                  glm::vec3(0.0f, 0.0f, -1.0f));
        emit_quad(glm::vec3(max_x, min_y, max_z), glm::vec3(min_x, min_y, max_z),
                  glm::vec3(min_x, max_y, max_z), glm::vec3(max_x, max_y, max_z),
                  glm::vec3(0.0f, 0.0f, 1.0f));
        emit_quad(glm::vec3(min_x, min_y, max_z), glm::vec3(min_x, min_y, min_z),
                  glm::vec3(min_x, max_y, min_z), glm::vec3(min_x, max_y, max_z),
                  glm::vec3(-1.0f, 0.0f, 0.0f));
        emit_quad(glm::vec3(max_x, min_y, min_z), glm::vec3(max_x, min_y, max_z),
                  glm::vec3(max_x, max_y, max_z), glm::vec3(max_x, max_y, min_z),
                  glm::vec3(1.0f, 0.0f, 0.0f));
        emit_quad(glm::vec3(min_x, min_y, max_z), glm::vec3(max_x, min_y, max_z),
                  glm::vec3(max_x, min_y, min_z), glm::vec3(min_x, min_y, min_z),
                  glm::vec3(0.0f, -1.0f, 0.0f));
        emit_quad(glm::vec3(min_x, max_y, min_z), glm::vec3(max_x, max_y, min_z),
                  glm::vec3(max_x, max_y, max_z), glm::vec3(min_x, max_y, max_z),
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

class MudHudWindow final : public frame::gui::GuiWindowInterface
{
public:
    explicit MudHudWindow(std::function<bool()> draw_callback)
        : draw_callback_(std::move(draw_callback))
    {
    }

    bool DrawCallback() override
    {
        return draw_callback_();
    }

    bool End() const override
    {
        return false;
    }

    std::string GetName() const override
    {
        return name_;
    }

    void SetName(const std::string& name) override
    {
        name_ = name;
    }

private:
    std::function<bool()> draw_callback_;
    std::string name_ = "grpcMUD HUD";
};
} // namespace

int ClientApp::Run(int argc, char** argv)
{
    const auto positional_args = absl::ParseCommandLine(argc, argv);
    if (positional_args.size() > 3)
    {
        std::cerr << "Unexpected positional arguments. Use --help for usage." << std::endl;
        return 1;
    }

    // Keep frame internals quiet by default; warn/error is still visible.
    frame::Logger::GetInstance()->set_level(spdlog::level::warn);
    frame::Logger::GetInstance()->flush_on(spdlog::level::warn);

    server_address_ = Trim(absl::GetFlag(FLAGS_server_address));
    if (server_address_.empty())
    {
        std::cerr << "Server address cannot be empty." << std::endl;
        return 1;
    }
    if (positional_args.size() > 1)
    {
        server_address_ = Trim(positional_args[1]);
    }

    player_name_ = Trim(absl::GetFlag(FLAGS_player_name));
    if (positional_args.size() > 2)
    {
        player_name_ = Trim(positional_args[2]);
    }

    const std::string requested_backend = ToLower(Trim(absl::GetFlag(FLAGS_render_api)));
    frame::RenderingAPIEnum rendering_api = frame::RenderingAPIEnum::OPENGL;
    if (requested_backend.empty() || requested_backend == "opengl")
    {
        rendering_api = frame::RenderingAPIEnum::OPENGL;
    }
    else if (requested_backend == "vulkan")
    {
        rendering_api = frame::RenderingAPIEnum::VULKAN;
    }
    else
    {
        std::cerr << "Unsupported --render_api value '" << requested_backend
                  << "'. Expected 'opengl' or 'vulkan'." << std::endl;
        return 1;
    }

    if (rendering_api == frame::RenderingAPIEnum::VULKAN)
    {
        frame::vulkan::EnsureWindowFactoryRegistered();
    }

    std::snprintf(server_address_input_buffer_.data(), server_address_input_buffer_.size(), "%s",
                  server_address_.c_str());
    std::snprintf(player_name_input_buffer_.data(), player_name_input_buffer_.size(), "%s",
                  player_name_.c_str());

    std::unique_ptr<frame::WindowInterface> window;
    try
    {
        window = frame::CreateNewWindow(frame::DrawingTargetEnum::WINDOW,
                                        rendering_api, kDefaultWindowSize);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Failed to create 3D window: " << ex.what() << std::endl;
        return 1;
    }
    if (!window)
    {
        std::cerr << "Failed to create 3D window." << std::endl;
        return 1;
    }

    AddLog("Enter server + player name in HUD and press Connect.");
    AddLog(std::string("Renderer: ") +
           (rendering_api == frame::RenderingAPIEnum::VULKAN ? "vulkan" : "opengl"));

    RegisterKeyBindings(*window);

    std::filesystem::path font_path;
    try
    {
        font_path = frame::file::FindFile("external/frame/asset/font/poppins/Poppins-Regular.ttf");
    }
    catch (const std::exception&)
    {
        font_path.clear();
    }

    auto gui = frame::gui::CreateDrawGui(*window, font_path, 18.0f);
    if (gui)
    {
        gui->AddWindow(std::make_unique<MudHudWindow>([this]() { return DrawHud(); }));
        window->GetDevice().AddPlugin(std::move(gui));
    }
    else
    {
        AddLog("[warn] GUI overlay unavailable; controls still active.");
    }

    if (!RebuildSceneIfDirty(*window))
    {
        std::cerr << "[client] Initial scene build failed; continuing with empty frame."
                  << std::endl;
    }

    frame::WindowReturnEnum window_status = frame::WindowReturnEnum::CONTINUE;
    if (running_.load(std::memory_order_relaxed))
    {
        window_status =
            window->Run([this, &window]() { return RenderFrame(*window); });
    }

    if (window_status == frame::WindowReturnEnum::QUIT)
    {
        running_.store(false, std::memory_order_relaxed);
    }

    const grpc::Status status = session_.Shutdown();
    if (!status.ok() && connected_)
    {
        std::cerr << "RPC ended with error: [" << status.error_code() << "] "
                  << status.error_message() << std::endl;
        return 1;
    }

    return 0;
}

std::string ClientApp::Trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, (last - first) + 1);
}

std::string ClientApp::ToLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string ClientApp::JsonEscape(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size() + 16);
    for (const char c : text)
    {
        switch (c)
        {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(c);
            break;
        }
    }
    return escaped;
}

bool ClientApp::TryConnect()
{
    if (connected_)
    {
        return true;
    }

    const std::string desired_server = Trim(server_address_input_buffer_.data());
    const std::string desired_player_name = Trim(player_name_input_buffer_.data());

    if (desired_server.empty())
    {
        connect_error_message_ = "Server address cannot be empty.";
        AddLog("[connect] " + connect_error_message_);
        return false;
    }
    if (desired_player_name.empty())
    {
        connect_error_message_ = "Player name cannot be empty.";
        AddLog("[connect] " + connect_error_message_);
        return false;
    }

    std::string error_message;
    if (!session_.Connect(desired_server, desired_player_name, &error_message))
    {
        connect_error_message_ =
            error_message.empty() ? "Failed to open stream." : error_message;
        AddLog("[connect] " + connect_error_message_);
        return false;
    }

    server_address_ = desired_server;
    player_name_ = desired_player_name;
    connected_ = true;
    connect_error_message_.clear();
    server_closed_.store(false, std::memory_order_relaxed);
    server_closed_logged_ = false;

    session_.StartReader(
        [this](const mud::v1::ServerMessage& message) { QueueServerMessage(message); },
        [this]() { HandleServerClosed(); });

    AddLog("Connected to " + server_address_ + " as '" + player_name_ + "'.");
    AddLog("3D mode. WASD move/turn, 1/2 attack, 3 guard, Enter chat, Q quit.");
    {
        mud::v1::ClientMessage look_message;
        look_message.mutable_look()->set_request_id(NextRequestId());
        if (!SendMessage(std::move(look_message)))
        {
            AddLog("[warn] Failed to request initial look.");
        }
    }
    RequestInputFocus();
    return true;
}

std::string ClientApp::NextRequestId()
{
    return "req-" + std::to_string(request_counter_++);
}

bool ClientApp::SendMessage(mud::v1::ClientMessage message)
{
    if (!connected_ || server_closed_.load(std::memory_order_relaxed))
    {
        AddLog("[connect] Not connected.");
        return false;
    }

    if (!session_.SendClientMessage(message))
    {
        AddLog("[error] Failed to send command.");
        std::cerr << "[client] Failed to send command to server." << std::endl;
        return false;
    }
    return true;
}

bool ClientApp::SendLookRequest()
{
    mud::v1::ClientMessage message;
    message.mutable_look()->set_request_id(NextRequestId());
    return TrySendGameplayCommand(std::move(message));
}

bool ClientApp::SendStepRequest(mud::v1::StepKind kind)
{
    mud::v1::ClientMessage message;
    auto* step = message.mutable_step();
    step->set_request_id(NextRequestId());
    step->set_kind(kind);
    if (!TrySendGameplayCommand(std::move(message)))
    {
        return false;
    }
    StartPredictedStepAnimation(kind);
    return true;
}

bool ClientApp::SendSayRequest(const std::string& text)
{
    const std::string trimmed = Trim(text);
    if (trimmed.empty())
    {
        return false;
    }

    mud::v1::ClientMessage message;
    auto* say = message.mutable_say();
    say->set_request_id(NextRequestId());
    say->set_text(trimmed);
    return TrySendGameplayCommand(std::move(message));
}

bool ClientApp::SendGuardRequest()
{
    mud::v1::ClientMessage message;
    message.mutable_guard()->set_request_id(NextRequestId());
    return TrySendGameplayCommand(std::move(message));
}

bool ClientApp::SendAttackRequest(mud::v1::WeaponKind weapon)
{
    mud::v1::ClientMessage message;
    auto* attack = message.mutable_attack();
    attack->set_request_id(NextRequestId());
    attack->set_weapon(weapon);
    return TrySendGameplayCommand(std::move(message));
}

void ClientApp::QueueServerMessage(const mud::v1::ServerMessage& message)
{
    std::lock_guard<std::mutex> lock(pending_messages_mutex_);
    pending_messages_.push_back(message);
}

void ClientApp::ProcessQueuedServerMessages()
{
    std::deque<mud::v1::ServerMessage> pending;
    {
        std::lock_guard<std::mutex> lock(pending_messages_mutex_);
        pending.swap(pending_messages_);
    }

    for (const auto& message : pending)
    {
        HandleServerMessageOnMainThread(message);
    }

    if (server_closed_.load(std::memory_order_relaxed) && !server_closed_logged_)
    {
        AddLog("[system] Connection closed.");
        std::cerr << "[client] Server stream closed. Keeping window open." << std::endl;
        server_closed_logged_ = true;
    }
}

void ClientApp::HandleServerMessageOnMainThread(const mud::v1::ServerMessage& message)
{
    auto maybe_respawn_seconds_from_text = [&](const std::string& text) -> int
    {
        const std::string lowered = ToLower(text);
        const auto marker = lowered.find("respawn in ");
        if (marker == std::string::npos)
        {
            return -1;
        }

        int value = 0;
        bool found_digit = false;
        for (std::size_t i = marker; i < lowered.size(); ++i)
        {
            if (!std::isdigit(static_cast<unsigned char>(lowered[i])))
            {
                if (found_digit)
                {
                    break;
                }
                continue;
            }

            found_digit = true;
            value = (value * 10) + (lowered[i] - '0');
        }
        return found_digit ? value : -1;
    };

    const std::uint64_t message_tick_id = message.tick_id();
    std::uint64_t observed_tick = latest_tick_id_.load(std::memory_order_relaxed);
    while (message_tick_id > observed_tick &&
           !latest_tick_id_.compare_exchange_weak(observed_tick, message_tick_id,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed))
    {
    }

    switch (message.payload_case())
    {
    case mud::v1::ServerMessage::kJoinAck:
        AddLog("[join] player_id=" + message.join_ack().player_id());
        AddLog("[motd] " + message.join_ack().motd());
        break;
    case mud::v1::ServerMessage::kCommandAck:
        if (!message.command_ack().accepted())
        {
            AddLog("[ack] " + message.command_ack().request_id() + " rejected \"" +
                   message.command_ack().message() + "\"");
            const int respawn_seconds =
                maybe_respawn_seconds_from_text(message.command_ack().message());
            if (respawn_seconds >= 0)
            {
                death_screen_active_ = true;
                death_seconds_remaining_ = respawn_seconds;
            }
        }
        break;
    case mud::v1::ServerMessage::kWorldEvent:
    {
        std::string prefix = "[event]";
        switch (message.world_event().kind())
        {
        case mud::v1::WorldEvent::KIND_ROOM:
            prefix = "[room]";
            break;
        case mud::v1::WorldEvent::KIND_CHAT:
            prefix = "[chat]";
            break;
        case mud::v1::WorldEvent::KIND_SYSTEM:
            prefix = "[system]";
            break;
        case mud::v1::WorldEvent::KIND_COMBAT:
            prefix = "[combat]";
            break;
        case mud::v1::WorldEvent::KIND_UNSPECIFIED:
        default:
            break;
        }

        AddLog(prefix + " " + message.world_event().text());
        const std::string lowered = ToLower(message.world_event().text());
        if (lowered.find("you were knocked out") != std::string::npos)
        {
            death_screen_active_ = true;
            death_seconds_remaining_ = 10;
        }
        else if (lowered.find("you respawn") != std::string::npos)
        {
            death_screen_active_ = false;
            death_seconds_remaining_ = 0;
        }
        break;
    }
    case mud::v1::ServerMessage::kError:
        AddLog("[error] " + message.error().code() + ": " + message.error().message());
        break;
    case mud::v1::ServerMessage::kPong:
        AddLog("[pong]");
        break;
    case mud::v1::ServerMessage::kTick:
        UpdateTickIntervalEstimate(message.tick());
        TickDeathScreen();
        break;
    case mud::v1::ServerMessage::kView:
        HandleViewUpdate(message.view());
        break;
    case mud::v1::ServerMessage::PAYLOAD_NOT_SET:
    default:
        AddLog("[server] message with no payload");
        break;
    }
}

void ClientApp::HandleServerClosed()
{
    server_closed_.store(true, std::memory_order_relaxed);
}

void ClientApp::HandleActionKey(char key)
{
    if (IsInputFocused())
    {
        return;
    }

    key = static_cast<char>(std::tolower(static_cast<unsigned char>(key)));
    if (key == 'q')
    {
        running_.store(false, std::memory_order_relaxed);
        return;
    }

    if (!connected_ || server_closed_.load(std::memory_order_relaxed))
    {
        return;
    }

    if (death_screen_active_)
    {
        return;
    }

    if (key == '1')
    {
        SendAttackRequest(mud::v1::WEAPON_KIND_MELEE);
        return;
    }
    if (key == '2')
    {
        SendAttackRequest(mud::v1::WEAPON_KIND_RANGED);
        return;
    }
    if (key == '3')
    {
        SendGuardRequest();
        return;
    }
    if (key == 'p')
    {
        if (!session_.SendPing())
        {
            AddLog("[error] Failed to send ping.");
        }
        return;
    }
    if (key == '/')
    {
        chat_input_buffer_[0] = '/';
        chat_input_buffer_[1] = '\0';
        RequestInputFocus();
        return;
    }
    if (key == 'w' || key == 'k')
    {
        SendStepRequest(mud::v1::STEP_KIND_MOVE_FORWARD);
        return;
    }
    if (key == 'a' || key == 'h')
    {
        SendStepRequest(mud::v1::STEP_KIND_TURN_LEFT);
        return;
    }
    if (key == 's' || key == 'j')
    {
        SendStepRequest(mud::v1::STEP_KIND_MOVE_BACKWARD);
        return;
    }
    if (key == 'd' || key == 'l')
    {
        SendStepRequest(mud::v1::STEP_KIND_TURN_RIGHT);
        return;
    }
}

void ClientApp::HandleSubmittedText(const std::string& text)
{
    if (!connected_ || server_closed_.load(std::memory_order_relaxed))
    {
        AddLog("[connect] Connect first.");
        return;
    }

    if (death_screen_active_)
    {
        AddLog("[dead] Controls are disabled until respawn.");
        return;
    }

    const std::string trimmed = Trim(text);
    if (trimmed.empty())
    {
        return;
    }

    if (trimmed[0] != '/')
    {
        SendSayRequest(trimmed);
        return;
    }

    const std::string command = Trim(trimmed.substr(1));
    const auto split = command.find(' ');
    const std::string verb =
        ToLower(split == std::string::npos ? command : command.substr(0, split));
    const std::string argument =
        Trim(split == std::string::npos ? std::string{} : command.substr(split + 1));
    const std::string normalized_argument = ToLower(argument);

    if (verb == "look")
    {
        SendLookRequest();
        return;
    }
    if (verb == "move")
    {
        if (normalized_argument == "forward" || normalized_argument == "f")
        {
            SendStepRequest(mud::v1::STEP_KIND_MOVE_FORWARD);
            return;
        }
        if (normalized_argument == "backward" || normalized_argument == "back" ||
            normalized_argument == "b")
        {
            SendStepRequest(mud::v1::STEP_KIND_MOVE_BACKWARD);
            return;
        }
        AddLog("[usage] /move <forward|backward>");
        return;
    }
    if (verb == "turn")
    {
        if (normalized_argument == "left" || normalized_argument == "l")
        {
            SendStepRequest(mud::v1::STEP_KIND_TURN_LEFT);
            return;
        }
        if (normalized_argument == "right" || normalized_argument == "r")
        {
            SendStepRequest(mud::v1::STEP_KIND_TURN_RIGHT);
            return;
        }
        AddLog("[usage] /turn <left|right>");
        return;
    }
    if (verb == "say")
    {
        if (argument.empty())
        {
            AddLog("[usage] /say <text>");
            return;
        }
        SendSayRequest(argument);
        return;
    }
    if (verb == "guard")
    {
        SendGuardRequest();
        return;
    }
    if (verb == "attack")
    {
        if (normalized_argument.empty() || normalized_argument == "melee" ||
            normalized_argument == "m")
        {
            SendAttackRequest(mud::v1::WEAPON_KIND_MELEE);
            return;
        }
        if (normalized_argument == "ranged" || normalized_argument == "range" ||
            normalized_argument == "r")
        {
            SendAttackRequest(mud::v1::WEAPON_KIND_RANGED);
            return;
        }
        AddLog("[usage] /attack <melee|ranged>");
        return;
    }
    if (verb == "ping")
    {
        if (!session_.SendPing())
        {
            AddLog("[error] Failed to send ping.");
        }
        return;
    }
    if (verb == "help")
    {
        AddLog("[usage] /look /move /turn /say /guard /attack /ping");
        return;
    }

    AddLog("[usage] Unknown command. Try /look, /move, /turn, /say, /guard, /attack, /ping.");
}

void ClientApp::AddLog(std::string line)
{
    logs_.push_back(std::move(line));
    while (logs_.size() > kMaxLogLines)
    {
        logs_.pop_front();
    }
}

void ClientApp::TickDeathScreen()
{
    if (!death_screen_active_)
    {
        return;
    }
    if (death_seconds_remaining_ > 0)
    {
        --death_seconds_remaining_;
    }
}

bool ClientApp::IsInputFocused() const
{
    return input_focused_;
}

void ClientApp::SetInputFocused(bool focused)
{
    input_focused_ = focused;
}

bool ClientApp::ConsumeInputFocusRequest()
{
    const bool was_requested = input_focus_requested_;
    input_focus_requested_ = false;
    return was_requested;
}

void ClientApp::RequestInputFocus()
{
    input_focus_requested_ = true;
}

void ClientApp::HandleViewUpdate(const mud::v1::LocalViewUpdate& view)
{
    const float target_x = static_cast<float>(view.center_x()) * kCellSize;
    const float target_z = static_cast<float>(view.center_y()) * kCellSize;
    const float target_yaw = DirectionToYawRadians(view.facing());

    if (!camera_pose_initialized_)
    {
        camera_pose_initialized_ = true;
        camera_animating_ = false;
        camera_current_x_ = target_x;
        camera_current_z_ = target_z;
        camera_current_yaw_ = target_yaw;
        camera_start_x_ = target_x;
        camera_start_z_ = target_z;
        camera_start_yaw_ = target_yaw;
        camera_target_x_ = target_x;
        camera_target_z_ = target_z;
        camera_target_yaw_ = target_yaw;
    }
    else
    {
        UpdateCameraAnimation();

        const bool animate =
            latest_view_.has_value() && IsSingleTickMoveOrTurn(*latest_view_, view);
        if (animate)
        {
            camera_start_x_ = camera_current_x_;
            camera_start_z_ = camera_current_z_;
            camera_start_yaw_ = camera_current_yaw_;
            camera_target_x_ = target_x;
            camera_target_z_ = target_z;
            camera_target_yaw_ = target_yaw;
            camera_animation_started_at_ = std::chrono::steady_clock::now();
            camera_animation_duration_ = std::chrono::milliseconds(
                std::clamp(tick_interval_estimate_ms_, kMinTickIntervalEstimateMs,
                           kMaxTickIntervalEstimateMs));
            camera_animating_ = true;
        }
        else
        {
            camera_animating_ = false;
            camera_current_x_ = target_x;
            camera_current_z_ = target_z;
            camera_current_yaw_ = target_yaw;
            camera_start_x_ = target_x;
            camera_start_z_ = target_z;
            camera_start_yaw_ = target_yaw;
            camera_target_x_ = target_x;
            camera_target_z_ = target_z;
            camera_target_yaw_ = target_yaw;
        }
    }

    latest_view_ = view;
    scene_dirty_ = true;
}

void ClientApp::UpdateTickIntervalEstimate(const mud::v1::TickEvent& tick)
{
    const std::uint64_t tick_id = tick.tick_id();
    const std::uint64_t tick_time_ms = tick.server_time_ms();

    if (last_tick_sample_id_ != 0 && last_tick_sample_time_ms_ != 0 &&
        tick_id > last_tick_sample_id_ && tick_time_ms > last_tick_sample_time_ms_)
    {
        const std::uint64_t tick_delta = tick_id - last_tick_sample_id_;
        const std::uint64_t time_delta_ms = tick_time_ms - last_tick_sample_time_ms_;
        if (tick_delta > 0)
        {
            const std::uint64_t sample_ms = time_delta_ms / tick_delta;
            if (sample_ms > 0)
            {
                const std::uint64_t smoothed_ms =
                    ((tick_interval_estimate_ms_ * 3) + sample_ms) / 4;
                tick_interval_estimate_ms_ =
                    std::clamp(smoothed_ms, kMinTickIntervalEstimateMs,
                               kMaxTickIntervalEstimateMs);
            }
        }
    }

    if (tick_id >= last_tick_sample_id_ && tick_time_ms >= last_tick_sample_time_ms_)
    {
        last_tick_sample_id_ = tick_id;
        last_tick_sample_time_ms_ = tick_time_ms;
    }
}

void ClientApp::UpdateCameraAnimation()
{
    if (!camera_pose_initialized_)
    {
        return;
    }

    if (!camera_animating_)
    {
        camera_current_x_ = camera_target_x_;
        camera_current_z_ = camera_target_z_;
        camera_current_yaw_ = camera_target_yaw_;
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - camera_animation_started_at_)
            .count();
    const std::int64_t duration_ms = std::max<std::int64_t>(1, camera_animation_duration_.count());
    const float t =
        std::clamp(static_cast<float>(elapsed_ms) / static_cast<float>(duration_ms), 0.0f, 1.0f);
    const float eased_t = SmoothStep(t);

    camera_current_x_ = camera_start_x_ + ((camera_target_x_ - camera_start_x_) * eased_t);
    camera_current_z_ = camera_start_z_ + ((camera_target_z_ - camera_start_z_) * eased_t);
    camera_current_yaw_ = LerpAngleRadians(camera_start_yaw_, camera_target_yaw_, eased_t);

    if (t >= 1.0f)
    {
        camera_animating_ = false;
        camera_current_x_ = camera_target_x_;
        camera_current_z_ = camera_target_z_;
        camera_current_yaw_ = camera_target_yaw_;
    }
}

void ClientApp::StartPredictedStepAnimation(mud::v1::StepKind kind)
{
    if (!camera_pose_initialized_ || !latest_view_.has_value())
    {
        return;
    }

    mud::v1::LocalViewUpdate predicted_view = *latest_view_;
    switch (kind)
    {
    case mud::v1::STEP_KIND_MOVE_FORWARD:
    case mud::v1::STEP_KIND_MOVE_BACKWARD:
    {
        if (!CanPredictMoveFromCenter(predicted_view, kind))
        {
            return;
        }

        auto [dx, dy] = DirectionToGridDelta(predicted_view.facing());
        if (kind == mud::v1::STEP_KIND_MOVE_BACKWARD)
        {
            dx = -dx;
            dy = -dy;
        }
        predicted_view.set_center_x(predicted_view.center_x() + dx);
        predicted_view.set_center_y(predicted_view.center_y() + dy);
        break;
    }
    case mud::v1::STEP_KIND_TURN_LEFT:
        predicted_view.set_facing(TurnLeftDirection(predicted_view.facing()));
        break;
    case mud::v1::STEP_KIND_TURN_RIGHT:
        predicted_view.set_facing(TurnRightDirection(predicted_view.facing()));
        break;
    case mud::v1::STEP_KIND_UNSPECIFIED:
    default:
        return;
    }

    UpdateCameraAnimation();

    const float target_x = static_cast<float>(predicted_view.center_x()) * kCellSize;
    const float target_z = static_cast<float>(predicted_view.center_y()) * kCellSize;
    const float target_yaw = DirectionToYawRadians(predicted_view.facing());

    camera_start_x_ = camera_current_x_;
    camera_start_z_ = camera_current_z_;
    camera_start_yaw_ = camera_current_yaw_;
    camera_target_x_ = target_x;
    camera_target_z_ = target_z;
    camera_target_yaw_ = target_yaw;
    camera_animation_started_at_ = std::chrono::steady_clock::now();
    camera_animation_duration_ =
        std::chrono::milliseconds(std::clamp(tick_interval_estimate_ms_,
                                             kMinTickIntervalEstimateMs,
                                             kMaxTickIntervalEstimateMs));
    camera_animating_ = true;
}

void ClientApp::ApplyCameraPose(frame::WindowInterface& window)
{
    if (!camera_pose_initialized_)
    {
        return;
    }

    auto& camera = window.GetDevice().GetLevel().GetDefaultCamera();
    const float facing_x = std::cos(camera_current_yaw_);
    const float facing_z = std::sin(camera_current_yaw_);

    camera.SetPosition(glm::vec3(camera_current_x_, kCameraHeight, camera_current_z_));
    camera.SetFront(glm::vec3(facing_x, 0.0f, facing_z));
}

std::string ClientApp::BuildBootstrapLevelJson() const
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
    return BuildLevelJsonFromView(bootstrap);
}

std::string ClientApp::BuildLevelJsonFromView(const mud::v1::LocalViewUpdate& view) const
{
    std::unordered_map<std::int64_t, const mud::v1::VisibleSquare*> squares_by_coord;
    squares_by_coord.reserve(static_cast<std::size_t>(view.squares_size()));
    for (const auto& square : view.squares())
    {
        squares_by_coord[CoordKey(square.x(), square.y())] = &square;
    }

    const auto has_square = [&](int x, int y) -> bool
    {
        return squares_by_coord.find(CoordKey(x, y)) != squares_by_coord.end();
    };

    std::vector<CubeSpec> floor_cubes;
    std::vector<CubeSpec> wall_cubes;
    std::vector<CubeSpec> player_cubes;
    std::vector<CubeSpec> npc_cubes;
    std::unordered_set<std::int64_t> ground_coords;
    std::unordered_set<std::int64_t> wall_coords;
    ground_coords.reserve(static_cast<std::size_t>(view.squares_size()) * 3u);
    wall_coords.reserve(static_cast<std::size_t>(view.squares_size()) * 2u);

    for (const auto& square : view.squares())
    {
        const std::int64_t square_key = CoordKey(square.x(), square.y());
        ground_coords.insert(square_key);

        if (square.kind() == mud::v1::SQUARE_KIND_WALL)
        {
            wall_coords.insert(square_key);
            continue;
        }

        if (!square.open_north() && !has_square(square.x(), square.y() - 1))
        {
            wall_coords.insert(CoordKey(square.x(), square.y() - 1));
        }
        if (!square.open_west() && !has_square(square.x() - 1, square.y()))
        {
            wall_coords.insert(CoordKey(square.x() - 1, square.y()));
        }
        if (!square.open_south() && !has_square(square.x(), square.y() + 1))
        {
            wall_coords.insert(CoordKey(square.x(), square.y() + 1));
        }
        if (!square.open_east() && !has_square(square.x() + 1, square.y()))
        {
            wall_coords.insert(CoordKey(square.x() + 1, square.y()));
        }
    }

    if (ground_coords.empty())
    {
        ground_coords.insert(CoordKey(view.center_x(), view.center_y()));
    }
    for (const std::int64_t wall_key : wall_coords)
    {
        ground_coords.insert(wall_key);
    }

    int min_square_x = view.center_x();
    int max_square_x = view.center_x();
    int min_square_y = view.center_y();
    int max_square_y = view.center_y();
    for (const std::int64_t coord_key : ground_coords)
    {
        const int x = CoordXFromKey(coord_key);
        const int y = CoordYFromKey(coord_key);
        min_square_x = std::min(min_square_x, x);
        max_square_x = std::max(max_square_x, x);
        min_square_y = std::min(min_square_y, y);
        max_square_y = std::max(max_square_y, y);
    }

    // Keep the local ground watertight even when some interior cells are omitted from
    // visibility (for example by LOS clipping around corners).
    for (int y = min_square_y; y <= max_square_y; ++y)
    {
        for (int x = min_square_x; x <= max_square_x; ++x)
        {
            ground_coords.insert(CoordKey(x, y));
        }
    }

    const float base_center_x =
        (static_cast<float>(min_square_x + max_square_x) * 0.5f) * kCellSize;
    const float base_center_z =
        (static_cast<float>(min_square_y + max_square_y) * 0.5f) * kCellSize;
    const float base_span_x =
        (static_cast<float>((max_square_x - min_square_x) + 1) + (2.0f * kBaseFloorMarginCells)) *
        kCellSize;
    const float base_span_z =
        (static_cast<float>((max_square_y - min_square_y) + 1) + (2.0f * kBaseFloorMarginCells)) *
        kCellSize;
    AddCubeSpec(floor_cubes, base_center_x, kBaseFloorCenterY, base_center_z, base_span_x,
                kBaseFloorThickness, base_span_z);

    for (const std::int64_t ground_key : ground_coords)
    {
        const float x = static_cast<float>(CoordXFromKey(ground_key)) * kCellSize;
        const float z = static_cast<float>(CoordYFromKey(ground_key)) * kCellSize;
        AddCubeSpec(floor_cubes, x, kFloorCenterY, z, kFloorTileSize, kFloorThickness,
                    kFloorTileSize);
    }

    for (const std::int64_t wall_key : wall_coords)
    {
        const float wall_x = static_cast<float>(CoordXFromKey(wall_key)) * kCellSize;
        const float wall_z = static_cast<float>(CoordYFromKey(wall_key)) * kCellSize;
        AddCubeSpec(wall_cubes, wall_x, kWallCenterY, wall_z, kWallCubeSize, kWallHeight,
                    kWallCubeSize);
    }

    for (const auto& actor : view.actors())
    {
        if (actor.kind() == mud::v1::VisibleActor::KIND_SELF)
        {
            continue;
        }

        const float x = static_cast<float>(actor.x()) * kCellSize;
        const float z = static_cast<float>(actor.y()) * kCellSize;
        if (actor.kind() == mud::v1::VisibleActor::KIND_NPC)
        {
            AddCubeSpec(npc_cubes, x, kGroundY + (kNpcHeight * 0.5f), z, kNpcWidth, kNpcHeight,
                        kNpcWidth);
        }
        else
        {
            AddCubeSpec(player_cubes, x, kGroundY + (kPlayerHeight * 0.5f), z, kPlayerWidth,
                        kPlayerHeight, kPlayerWidth);
        }
    }

    const std::filesystem::path model_root = std::filesystem::path("asset") / "model";
    std::filesystem::create_directories(model_root);
    WriteRaytraceObj(model_root / "grpcmud_floor.obj", floor_cubes);
    WriteRaytraceObj(model_root / "grpcmud_wall.obj", wall_cubes);
    WriteRaytraceObj(model_root / "grpcmud_player.obj", player_cubes);
    WriteRaytraceObj(model_root / "grpcmud_npc.obj", npc_cubes);

    const std::uint64_t geometry_hash =
        Fnv1aAppend(
            Fnv1aAppend(
                Fnv1aAppend(HashCubeSpecs(floor_cubes), HashCubeSpecs(wall_cubes)),
                HashCubeSpecs(player_cubes)),
            HashCubeSpecs(npc_cubes));
    std::ostringstream geometry_revision;
    geometry_revision << std::hex << std::setw(16) << std::setfill('0') << geometry_hash;
    const std::string scene_name = "grpcMUDRaytrace-" + geometry_revision.str();

    const float cam_x = static_cast<float>(view.center_x()) * kCellSize;
    const float cam_z = static_cast<float>(view.center_y()) * kCellSize;
    const FacingVec facing = ToFacingVec(view.facing());
    const float target_x = cam_x + (facing.x * kCameraForwardDistance);
    const float target_z = cam_z + (facing.z * kCameraForwardDistance);

    std::ostringstream json;
    json << "{"
         << "\"name\":\"" << scene_name << "\","
         << "\"default_texture_name\":\"billboard\","
         << "\"textures\":[{"
         << "\"name\":\"billboard\","
         << "\"size\":{\"x\":-1,\"y\":-1},"
         << "\"cubemap\":false,"
         << "\"pixel_element_size\":{\"value\":\"BYTE\"},"
         << "\"pixel_structure\":{\"value\":\"RGB\"}"
         << "},{"
         << "\"name\":\"ground_texture\","
         << "\"cubemap\":false,"
         << "\"pixel_element_size\":{\"value\":\"BYTE\"},"
         << "\"pixel_structure\":{\"value\":\"RGB\"},"
         << "\"file_name\":\"asset/texture/grpcmud_ground_grass.png\""
         << "},{"
         << "\"name\":\"wall_texture\","
         << "\"cubemap\":false,"
         << "\"pixel_element_size\":{\"value\":\"BYTE\"},"
         << "\"pixel_structure\":{\"value\":\"RGB\"},"
         << "\"file_name\":\"asset/texture/grpcmud_wall_rock.jpg\""
         << "}],"
         << "\"programs\":["
         << "{"
         << "\"name\":\"RayTraceProgram\","
         << "\"output_texture_names\":[\"billboard\"],"
         << "\"input_texture_names\":[\"ground_texture\",\"wall_texture\"],"
         << "\"input_scene_type\":{\"value\":\"QUAD\"},"
         << "\"shader_vertex\":\"grpcmud_raytrace.vert\","
         << "\"shader_fragment\":\"grpcmud_raytrace.frag\","
         << "\"shader_compute\":\"\""
         << ",\"bindings\":["
         << "{\"name\":\"ground_texture\",\"binding\":0,\"binding_type\":\"COMBINED_IMAGE_SAMPLER\",\"stages\":[\"FRAGMENT\"]},"
         << "{\"name\":\"wall_texture\",\"binding\":1,\"binding_type\":\"COMBINED_IMAGE_SAMPLER\",\"stages\":[\"FRAGMENT\"]},"
         << "{\"name\":\"FloorTriangles\",\"binding\":2,\"binding_type\":\"STORAGE_BUFFER\",\"stages\":[\"FRAGMENT\"]},"
         << "{\"name\":\"WallTriangles\",\"binding\":3,\"binding_type\":\"STORAGE_BUFFER\",\"stages\":[\"FRAGMENT\"]},"
         << "{\"name\":\"PlayerTriangles\",\"binding\":4,\"binding_type\":\"STORAGE_BUFFER\",\"stages\":[\"FRAGMENT\"]},"
         << "{\"name\":\"NpcTriangles\",\"binding\":5,\"binding_type\":\"STORAGE_BUFFER\",\"stages\":[\"FRAGMENT\"]},"
         << "{\"name\":\"UniformBlock\",\"binding\":6,\"binding_type\":\"UNIFORM_BUFFER\",\"stages\":[\"FRAGMENT\"]}"
         << "]"
         << ",\"uniforms\":["
         << "{\"name\":\"projection_inv\",\"uniform_enum\":\"PROJECTION_INV_MAT4\"},"
         << "{\"name\":\"view_inv\",\"uniform_enum\":\"VIEW_INV_MAT4\"},"
         << "{\"name\":\"camera_position\",\"uniform_enum\":\"CAMERA_POSITION_VEC3\"},"
         << "{\"name\":\"model_inv\",\"uniform_enum\":\"MODEL_INV_MAT4\"}"
         << "]"
         << "},"
         << "{"
         << "\"name\":\"RayTracePreprocessProgram\","
         << "\"input_scene_type\":{\"value\":\"SCENE\"},"
         << "\"shader_vertex\":\"grpcmud_preprocess.vert\","
         << "\"shader_fragment\":\"grpcmud_preprocess.frag\","
         << "\"shader_compute\":\"\""
         << ",\"uniforms\":["
         << "{\"name\":\"projection\",\"uniform_enum\":\"PROJECTION_MAT4\"},"
         << "{\"name\":\"view\",\"uniform_enum\":\"VIEW_MAT4\"},"
         << "{\"name\":\"model\",\"uniform_enum\":\"MODEL_MAT4\"}"
         << "]"
         << "}"
         << "],"
         << "\"materials\":["
         << "{"
         << "\"name\":\"RayTraceMaterial\","
         << "\"program_name\":\"RayTraceProgram\","
         << "\"texture_names\":[\"ground_texture\",\"wall_texture\"],"
         << "\"inner_names\":[\"ground_texture\",\"wall_texture\"],"
         << "\"buffer_names\":[\"FloorMesh.0.triangle\",\"WallMesh.0.triangle\",\"PlayerMesh.0.triangle\",\"NpcMesh.0.triangle\"],"
         << "\"inner_buffer_names\":[\"FloorTriangles\",\"WallTriangles\",\"PlayerTriangles\",\"NpcTriangles\"]"
         << "},"
         << "{"
         << "\"name\":\"RayTracePreprocessMaterial\","
         << "\"program_name\":\"RayTracePreprocessProgram\","
         << "\"texture_names\":[],"
         << "\"inner_names\":[]"
         << "}"
         << "],"
         << "\"scene_tree\":{"
         << "\"default_root_name\":\"root\","
         << "\"default_camera_name\":\"camera\","
         << "\"node_matrices\":[{\"name\":\"root\",\"matrix\":{\"m11\":1,\"m22\":1,\"m33\":1,\"m44\":1}}],"
         << "\"node_cameras\":[{"
         << "\"name\":\"camera\","
         << "\"parent\":\"root\","
         << "\"position\":{\"x\":" << cam_x << ",\"y\":" << kCameraHeight << ",\"z\":" << cam_z
         << "},"
         << "\"target\":{\"x\":" << target_x << ",\"y\":" << kCameraHeight << ",\"z\":" << target_z
         << "},"
         << "\"up\":{\"x\":0,\"y\":1,\"z\":0},"
         << "\"fov_degrees\":68.0,"
         << "\"aspect_ratio\":1.777777,"
         << "\"near_clip\":" << kCameraNearClip << ","
         << "\"far_clip\":200.0"
         << "}],"
         << "\"node_static_meshes\":["
         << "{\"name\":\"RayTracingRendering\",\"parent\":\"root\",\"mesh_enum\":\"QUAD\",\"material_name\":\"RayTraceMaterial\"},"
         << "{\"name\":\"FloorMesh\",\"parent\":\"root\",\"file_name\":\"grpcmud_floor.obj\",\"material_name\":\"RayTracePreprocessMaterial\",\"render_time_enum\":\"PRE_RENDER_TIME\"},"
         << "{\"name\":\"WallMesh\",\"parent\":\"root\",\"file_name\":\"grpcmud_wall.obj\",\"material_name\":\"RayTracePreprocessMaterial\",\"render_time_enum\":\"PRE_RENDER_TIME\"},"
         << "{\"name\":\"PlayerMesh\",\"parent\":\"root\",\"file_name\":\"grpcmud_player.obj\",\"material_name\":\"RayTracePreprocessMaterial\",\"render_time_enum\":\"PRE_RENDER_TIME\"},"
         << "{\"name\":\"NpcMesh\",\"parent\":\"root\",\"file_name\":\"grpcmud_npc.obj\",\"material_name\":\"RayTracePreprocessMaterial\",\"render_time_enum\":\"PRE_RENDER_TIME\"}"
         << "],"
         << "\"node_lights\":[]"
         << "}"
         << "}";
    return json.str();
}

bool ClientApp::RebuildSceneIfDirty(frame::WindowInterface& window)
{
    if (!scene_dirty_)
    {
        return true;
    }

    scene_dirty_ = false;
    try
    {
        const std::string level_json =
            latest_view_ ? BuildLevelJsonFromView(*latest_view_) : BuildBootstrapLevelJson();
        if (level_json == last_level_json_)
        {
            return true;
        }

        if (window.GetDevice().GetDeviceEnum() == frame::RenderingAPIEnum::VULKAN)
        {
            const auto asset_root = frame::file::FindDirectory("asset");
            const auto level_data = frame::json::ParseLevelData(window.GetSize(), level_json,
                                                                asset_root);
            auto* vulkan_device = dynamic_cast<frame::vulkan::Device*>(&window.GetDevice());
            if (!vulkan_device)
            {
                throw std::runtime_error("Vulkan device not available.");
            }
            vulkan_device->StartupFromLevelData(level_data);
        }
        else
        {
            auto level = frame::json::ParseLevel(window.GetSize(), level_json);
            window.GetDevice().Startup(std::move(level));
        }
        last_level_json_ = level_json;
    }
    catch (const std::exception& ex)
    {
        AddLog(std::string("[error] Failed to rebuild 3D scene: ") + ex.what());
        std::cerr << "[client] Failed to rebuild 3D scene: " << ex.what() << std::endl;
        return false;
    }
    return true;
}

bool ClientApp::RenderFrame(frame::WindowInterface& window)
{
    ProcessQueuedServerMessages();
    UpdateCameraAnimation();
    if (!RebuildSceneIfDirty(window))
    {
        return running_.load(std::memory_order_relaxed);
    }
    ApplyCameraPose(window);
    return running_.load(std::memory_order_relaxed);
}

bool ClientApp::DrawHud()
{
    ImGui::TextUnformatted("grpcMUD - 3D client");
    if (!connected_)
    {
        ImGui::TextUnformatted("Connect to start playing.");
        if (!connect_error_message_.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s",
                               connect_error_message_.c_str());
        }

        bool submit_connect = false;
        submit_connect |=
            ImGui::InputText("Server", server_address_input_buffer_.data(),
                             server_address_input_buffer_.size(),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        const bool server_input_focused = ImGui::IsItemActive() || ImGui::IsItemFocused();
        submit_connect |=
            ImGui::InputText("Player Name", player_name_input_buffer_.data(),
                             player_name_input_buffer_.size(),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        const bool player_input_focused = ImGui::IsItemActive() || ImGui::IsItemFocused();
        SetInputFocused(server_input_focused || player_input_focused);

        if (ImGui::Button("Connect"))
        {
            submit_connect = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit"))
        {
            running_.store(false, std::memory_order_relaxed);
        }

        if (submit_connect)
        {
            TryConnect();
        }

        ImGui::Separator();
        ImGui::BeginChild("grpcmud_log_view", ImVec2(0.0f, 260.0f), true);
        for (const auto& line : logs_)
        {
            ImGui::TextWrapped("%s", line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        return true;
    }

    if (death_screen_active_)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "You are down. Respawn in %d second(s).", death_seconds_remaining_);
    }
    ImGui::TextUnformatted("Controls: W/S move, A/D turn, 1 melee, 2 ranged, 3 guard, Enter chat.");

    if (ImGui::Button("Look"))
    {
        SendLookRequest();
    }
    ImGui::SameLine();
    if (ImGui::Button("Ping"))
    {
        if (!session_.SendPing())
        {
            AddLog("[error] Failed to send ping.");
        }
    }

    ImGui::Separator();
    ImGui::BeginChild("grpcmud_log_view", ImVec2(0.0f, 220.0f), true);
    for (const auto& line : logs_)
    {
        ImGui::TextWrapped("%s", line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
    {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    if (ConsumeInputFocusRequest())
    {
        ImGui::SetKeyboardFocusHere();
    }

    const bool submitted = ImGui::InputText("Command / Chat", chat_input_buffer_.data(),
                                            chat_input_buffer_.size(),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
    SetInputFocused(ImGui::IsItemActive() || ImGui::IsItemFocused());

    if (submitted)
    {
        const std::string submitted_text = Trim(chat_input_buffer_.data());
        if (!submitted_text.empty())
        {
            HandleSubmittedText(submitted_text);
        }
        chat_input_buffer_[0] = '\0';
        RequestInputFocus();
    }

    return true;
}

void ClientApp::RegisterKeyBindings(frame::WindowInterface& window)
{
    const auto bind = [&](int keycode, char key)
    {
        window.AddKeyCallback(keycode, [this, key]()
                              {
                                  HandleActionKey(key);
                                  return true;
                              });
    };

    bind(SDLK_Q, 'q');
    bind(SDLK_W, 'w');
    bind(SDLK_A, 'a');
    bind(SDLK_S, 's');
    bind(SDLK_D, 'd');
    bind(SDLK_H, 'h');
    bind(SDLK_J, 'j');
    bind(SDLK_K, 'k');
    bind(SDLK_L, 'l');
    bind(SDLK_1, '1');
    bind(SDLK_2, '2');
    bind(SDLK_3, '3');
    bind(SDLK_P, 'p');
    bind(SDLK_SLASH, '/');

    window.AddKeyCallback(SDLK_RETURN, [this]()
                          {
                              RequestInputFocus();
                              return true;
                          });
    window.AddKeyCallback(SDLK_KP_ENTER, [this]()
                          {
                              RequestInputFocus();
                              return true;
                          });
    window.AddKeyCallback(SDLK_ESCAPE, [this]()
                          {
                              chat_input_buffer_[0] = '\0';
                              SetInputFocused(false);
                              return true;
                          });
}

bool ClientApp::TrySendGameplayCommand(mud::v1::ClientMessage message)
{
    const std::uint64_t current_tick = latest_tick_id_.load(std::memory_order_relaxed);
    if (gameplay_command_sent_this_tick_ && current_tick == last_gameplay_command_tick_id_)
    {
        AddLog("[tick] One command per tick. Wait for next tick.");
        return false;
    }

    if (!SendMessage(std::move(message)))
    {
        return false;
    }

    gameplay_command_sent_this_tick_ = true;
    last_gameplay_command_tick_id_ = current_tick;
    return true;
}
} // namespace grpcmud::client
