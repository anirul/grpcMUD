#include "ViewMath.hpp"

#include <algorithm>
#include <cmath>

namespace grpcmud::client::viewmath
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;

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

} // namespace

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

bool IsSingleTickMoveOrTurn(
    const mud::v1::LocalViewUpdate& from,
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

} // namespace grpcmud::client::viewmath
