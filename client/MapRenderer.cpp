#include "MapRenderer.hpp"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <unordered_map>

namespace grpcmud::client
{
namespace
{
enum class BoundaryState
{
    kOpen,
    kWall,
    kUnknown
};

struct ActorMarker
{
    char glyph = '?';
    char facing = '?';
    int rank = 0;
};

std::int64_t CoordKey(int x, int y)
{
    return (static_cast<std::int64_t>(x) << 32) ^
           static_cast<std::uint32_t>(y);
}

char ActorGlyph(mud::v1::VisibleActor::Kind kind)
{
    switch (kind)
    {
    case mud::v1::VisibleActor::KIND_SELF:
        return '@';
    case mud::v1::VisibleActor::KIND_PLAYER:
        return 'P';
    case mud::v1::VisibleActor::KIND_NPC:
        return 'N';
    case mud::v1::VisibleActor::KIND_UNSPECIFIED:
    default:
        return '?';
    }
}

int ActorRank(mud::v1::VisibleActor::Kind kind)
{
    switch (kind)
    {
    case mud::v1::VisibleActor::KIND_SELF:
        return 3;
    case mud::v1::VisibleActor::KIND_PLAYER:
        return 2;
    case mud::v1::VisibleActor::KIND_NPC:
        return 1;
    case mud::v1::VisibleActor::KIND_UNSPECIFIED:
    default:
        return 0;
    }
}

char FacingGlyph(mud::v1::Direction direction)
{
    switch (direction)
    {
    case mud::v1::DIRECTION_NORTH:
        return '^';
    case mud::v1::DIRECTION_EAST:
        return '>';
    case mud::v1::DIRECTION_SOUTH:
        return 'v';
    case mud::v1::DIRECTION_WEST:
        return '<';
    case mud::v1::DIRECTION_UNSPECIFIED:
    default:
        return '?';
    }
}

ActorMarker ToActorMarker(const mud::v1::VisibleActor& actor)
{
    ActorMarker marker;
    marker.glyph = ActorGlyph(actor.kind());
    marker.facing = FacingGlyph(actor.facing());
    marker.rank = ActorRank(actor.kind());
    return marker;
}
} // namespace

std::string MapRenderer::Render(const mud::v1::LocalViewUpdate& view)
{
    std::unordered_map<std::int64_t, const mud::v1::VisibleSquare*> squares_by_coord;
    squares_by_coord.reserve(static_cast<std::size_t>(view.squares_size()));
    for (const auto& square : view.squares())
    {
        squares_by_coord[CoordKey(square.x(), square.y())] = &square;
    }
    const int radius = std::max(0, view.radius());
    const int min_x = view.center_x() - radius;
    const int max_x = view.center_x() + radius;
    const int min_y = view.center_y() - radius;
    const int max_y = view.center_y() + radius;

    std::unordered_map<std::int64_t, ActorMarker> actor_by_coord;
    actor_by_coord.reserve(static_cast<std::size_t>(view.actors_size()));
    for (const auto& actor : view.actors())
    {
        const std::int64_t key = CoordKey(actor.x(), actor.y());
        const ActorMarker marker = ToActorMarker(actor);

        auto it = actor_by_coord.find(key);
        if (it == actor_by_coord.end())
        {
            actor_by_coord[key] = marker;
        }
        else if (marker.rank >= it->second.rank)
        {
            it->second = marker;
        }
    }

    const auto find_square = [&](int x, int y) -> const mud::v1::VisibleSquare*
    {
        const auto it = squares_by_coord.find(CoordKey(x, y));
        return (it == squares_by_coord.end()) ? nullptr : it->second;
    };

    const auto is_wall_square = [&](const mud::v1::VisibleSquare* square) -> bool
    {
        return square != nullptr && square->kind() == mud::v1::SQUARE_KIND_WALL;
    };

    const auto horizontal_boundary = [&](int x, int boundary_y) -> BoundaryState
    {
        const auto* above = find_square(x, boundary_y - 1);
        const auto* below = find_square(x, boundary_y);

        if (above && below)
        {
            return (!above->open_south() || !below->open_north()) ? BoundaryState::kWall
                                                                   : BoundaryState::kOpen;
        }
        if (above && !below)
        {
            return above->open_south() ? BoundaryState::kUnknown : BoundaryState::kWall;
        }
        if (!above && below)
        {
            return below->open_north() ? BoundaryState::kUnknown : BoundaryState::kWall;
        }
        if (above || below)
        {
            return BoundaryState::kUnknown;
        }
        return BoundaryState::kUnknown;
    };

    const auto vertical_boundary = [&](int boundary_x, int y) -> BoundaryState
    {
        const auto* left = find_square(boundary_x - 1, y);
        const auto* right = find_square(boundary_x, y);

        if (left && right)
        {
            return (!left->open_east() || !right->open_west()) ? BoundaryState::kWall
                                                                : BoundaryState::kOpen;
        }
        if (left && !right)
        {
            return left->open_east() ? BoundaryState::kUnknown : BoundaryState::kWall;
        }
        if (!left && right)
        {
            return right->open_west() ? BoundaryState::kUnknown : BoundaryState::kWall;
        }
        if (left || right)
        {
            return BoundaryState::kUnknown;
        }
        return BoundaryState::kUnknown;
    };

    std::ostringstream out;
    for (int y = min_y; y <= max_y; ++y)
    {
        // Top wall segment for row y.
        for (int x = min_x; x <= max_x; ++x)
        {
            out << '+';
            const BoundaryState boundary = horizontal_boundary(x, y);
            if (boundary == BoundaryState::kWall)
            {
                out << "---";
            }
            else if (boundary == BoundaryState::kUnknown)
            {
                out << "???";
            }
            else
            {
                out << "   ";
            }
        }
        out << "+\n";

        // Cell segment with vertical walls.
        for (int x = min_x; x <= max_x; ++x)
        {
            const BoundaryState left_boundary = vertical_boundary(x, y);
            if (left_boundary == BoundaryState::kWall)
            {
                out << '|';
            }
            else if (left_boundary == BoundaryState::kUnknown)
            {
                out << '?';
            }
            else
            {
                out << ' ';
            }

            const auto* square = find_square(x, y);
            if (square != nullptr)
            {
                if (is_wall_square(square))
                {
                    out << "###";
                }
                else
                {
                    const auto actor_it = actor_by_coord.find(CoordKey(x, y));
                    if (actor_it != actor_by_coord.end())
                    {
                        out << actor_it->second.glyph << actor_it->second.facing << ' ';
                    }
                    else
                    {
                        out << " . ";
                    }
                }
            }
            else
            {
                out << "???";
            }
        }

        const BoundaryState right_boundary = vertical_boundary(max_x + 1, y);
        if (right_boundary == BoundaryState::kWall)
        {
            out << '|';
        }
        else if (right_boundary == BoundaryState::kUnknown)
        {
            out << '?';
        }
        else
        {
            out << ' ';
        }
        out << '\n';
    }

    // Bottom wall segment (boundary after the last row).
    for (int x = min_x; x <= max_x; ++x)
    {
        out << '+';
        const BoundaryState boundary = horizontal_boundary(x, max_y + 1);
        if (boundary == BoundaryState::kWall)
        {
            out << "---";
        }
        else if (boundary == BoundaryState::kUnknown)
        {
            out << "???";
        }
        else
        {
            out << "   ";
        }
    }
    out << "+\n";

    return out.str();
}
} // namespace grpcmud::client
