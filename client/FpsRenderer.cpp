#include "FpsRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace grpcmud::client
{
namespace
{
enum class SurfaceStyle
{
    kVoid,
    kCeiling,
    kFloor,
    kLeftWall,
    kLeftOpen,
    kRightWall,
    kRightOpen,
    kFrontWall,
    kFrontOpen,
    kFrontUnknown
};

struct Pixel
{
    char ch = ' ';
    SurfaceStyle style = SurfaceStyle::kVoid;
};

struct CorridorState
{
    bool known = false;
    bool open = false;
};

std::int64_t CellKey(int depth, int lane)
{
    return (static_cast<std::int64_t>(depth) << 32) ^ static_cast<std::uint32_t>(lane + 1024);
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

const char* AnsiForStyle(SurfaceStyle style)
{
    switch (style)
    {
    case SurfaceStyle::kVoid:
        return "\x1b[0m";
    case SurfaceStyle::kCeiling:
    case SurfaceStyle::kFloor:
        return "\x1b[48;5;240m\x1b[38;5;252m";
    case SurfaceStyle::kLeftWall:
        return "\x1b[48;5;238m\x1b[38;5;252m";
    case SurfaceStyle::kLeftOpen:
        return "\x1b[48;5;235m\x1b[38;5;252m";
    case SurfaceStyle::kRightWall:
        return "\x1b[48;5;239m\x1b[38;5;252m";
    case SurfaceStyle::kRightOpen:
        return "\x1b[48;5;235m\x1b[38;5;252m";
    case SurfaceStyle::kFrontWall:
        return "\x1b[48;5;244m\x1b[38;5;255m";
    case SurfaceStyle::kFrontOpen:
        return "\x1b[48;5;236m\x1b[38;5;255m";
    case SurfaceStyle::kFrontUnknown:
        return "\x1b[48;5;241m\x1b[38;5;255m";
    }
    return "\x1b[0m";
}

int InterpolateInt(int start, int end, double ratio)
{
    return static_cast<int>(std::lround(static_cast<double>(start) +
                                        (static_cast<double>(end - start) * ratio)));
}

void DrawLine(std::vector<std::vector<Pixel>>& canvas, int x0, int y0, int x1, int y1, char ch)
{
    const int height = static_cast<int>(canvas.size());
    if (height == 0)
    {
        return;
    }
    const int width = static_cast<int>(canvas[0].size());
    if (width == 0)
    {
        return;
    }

    const int dx = x1 - x0;
    const int dy = y1 - y0;
    const int steps = std::max(std::abs(dx), std::abs(dy));

    if (steps == 0)
    {
        if (x0 >= 0 && x0 < width && y0 >= 0 && y0 < height)
        {
            canvas[y0][x0].ch = ch;
        }
        return;
    }

    for (int step = 0; step <= steps; ++step)
    {
        const double ratio = static_cast<double>(step) / static_cast<double>(steps);
        const int x = InterpolateInt(x0, x1, ratio);
        const int y = InterpolateInt(y0, y1, ratio);
        if (x >= 0 && x < width && y >= 0 && y < height)
        {
            canvas[y][x].ch = ch;
        }
    }
}

std::string CorridorStateText(CorridorState state)
{
    if (!state.known)
    {
        return "unknown";
    }
    return state.open ? "open" : "wall";
}
} // namespace

std::string FpsRenderer::Render(const mud::v1::FirstPersonView& view, int terminal_columns,
                                int terminal_rows)
{
    std::unordered_map<std::int64_t, const mud::v1::FirstPersonCell*> cells;
    cells.reserve(static_cast<std::size_t>(view.cells_size()));
    for (const auto& cell : view.cells())
    {
        cells[CellKey(cell.depth(), cell.lane())] = &cell;
    }

    const auto get_cell = [&](int depth, int lane) -> const mud::v1::FirstPersonCell*
    {
        const auto it = cells.find(CellKey(depth, lane));
        return (it == cells.end()) ? nullptr : it->second;
    };

    CorridorState left_state;
    CorridorState right_state;
    CorridorState front_state;

    bool front_unknown = false;
    int front_depth_bucket = 2; // 0=near wall, 1=mid wall, 2=far/unknown, 3=open

    const auto* center = get_cell(0, 0);
    if (center != nullptr && center->visible())
    {
        left_state.known = true;
        left_state.open = center->open_left();

        right_state.known = true;
        right_state.open = center->open_right();

        if (!center->open_forward())
        {
            front_state.known = true;
            front_state.open = false;
            front_depth_bucket = 0;
        }
        else
        {
            const auto* depth1 = get_cell(1, 0);
            if (depth1 == nullptr || !depth1->visible())
            {
                front_state.known = false;
                front_unknown = true;
                front_depth_bucket = 2;
            }
            else if (!depth1->open_forward())
            {
                front_state.known = true;
                front_state.open = false;
                front_depth_bucket = 1;
            }
            else
            {
                const auto* depth2 = get_cell(2, 0);
                if (depth2 != nullptr && depth2->visible() && !depth2->open_forward())
                {
                    front_state.known = true;
                    front_state.open = false;
                    front_depth_bucket = 2;
                }
                else
                {
                    front_state.known = true;
                    front_state.open = true;
                    front_depth_bucket = 3;
                }
            }
        }
    }
    else
    {
        front_unknown = true;
        front_depth_bucket = 2;
    }

    int nearest_actor_depth = std::numeric_limits<int>::max();
    mud::v1::VisibleActor::Kind nearest_actor_kind = mud::v1::VisibleActor::KIND_UNSPECIFIED;
    mud::v1::Direction nearest_actor_facing = mud::v1::DIRECTION_UNSPECIFIED;
    for (const auto& cell : view.cells())
    {
        if (!cell.visible() || cell.lane() != 0 || cell.depth() <= 0)
        {
            continue;
        }
        if (cell.actor_kind() == mud::v1::VisibleActor::KIND_UNSPECIFIED)
        {
            continue;
        }
        if (cell.depth() < nearest_actor_depth)
        {
            nearest_actor_depth = cell.depth();
            nearest_actor_kind = cell.actor_kind();
            nearest_actor_facing = cell.actor_facing();
        }
    }

    const int scene_width = std::clamp(terminal_columns - 2, 30, 120);
    const int scene_height =
        std::clamp((terminal_rows > 0 ? (terminal_rows / 2) : 14), 10, 22);

    const int margin_x_pct[] = {16, 23, 30, 35};
    const int margin_y_pct[] = {14, 20, 26, 31};
    int margin_x = std::max(2, (scene_width * margin_x_pct[front_depth_bucket]) / 100);
    int margin_y = std::max(1, (scene_height * margin_y_pct[front_depth_bucket]) / 100);

    int front_left = margin_x;
    int front_right = scene_width - 1 - margin_x;
    int front_top = margin_y;
    int front_bottom = scene_height - 1 - margin_y;

    if ((front_right - front_left + 1) < 8)
    {
        const int center_x = scene_width / 2;
        front_left = std::max(2, center_x - 4);
        front_right = std::min(scene_width - 3, center_x + 3);
    }
    if ((front_bottom - front_top + 1) < 4)
    {
        const int center_y = scene_height / 2;
        front_top = std::max(1, center_y - 2);
        front_bottom = std::min(scene_height - 2, center_y + 1);
    }

    std::vector<std::vector<Pixel>> canvas(
        static_cast<std::size_t>(scene_height),
        std::vector<Pixel>(static_cast<std::size_t>(scene_width), Pixel{}));

    const auto set_surface = [&](int x, int y, SurfaceStyle style)
    {
        if (x < 0 || x >= scene_width || y < 0 || y >= scene_height)
        {
            return;
        }
        canvas[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)].style = style;
        canvas[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)].ch = ' ';
    };

    const auto set_char = [&](int x, int y, char ch)
    {
        if (x < 0 || x >= scene_width || y < 0 || y >= scene_height)
        {
            return;
        }
        canvas[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)].ch = ch;
    };

    const auto fill_span = [&](int y, int x0, int x1, SurfaceStyle style)
    {
        if (y < 0 || y >= scene_height)
        {
            return;
        }
        const int start = std::max(0, std::min(x0, x1));
        const int end = std::min(scene_width - 1, std::max(x0, x1));
        for (int x = start; x <= end; ++x)
        {
            set_surface(x, y, style);
        }
    };

    auto left_inner_x = [&](int y) -> int
    {
        if (y < front_top)
        {
            const int denom = std::max(1, front_top);
            const double ratio = static_cast<double>(y) / static_cast<double>(denom);
            return InterpolateInt(0, front_left, ratio);
        }
        if (y <= front_bottom)
        {
            return front_left;
        }
        const int denom = std::max(1, (scene_height - 1) - front_bottom);
        const double ratio = static_cast<double>(y - front_bottom) / static_cast<double>(denom);
        return InterpolateInt(front_left, 0, ratio);
    };

    auto right_inner_x = [&](int y) -> int
    {
        if (y < front_top)
        {
            const int denom = std::max(1, front_top);
            const double ratio = static_cast<double>(y) / static_cast<double>(denom);
            return InterpolateInt(scene_width - 1, front_right, ratio);
        }
        if (y <= front_bottom)
        {
            return front_right;
        }
        const int denom = std::max(1, (scene_height - 1) - front_bottom);
        const double ratio = static_cast<double>(y - front_bottom) / static_cast<double>(denom);
        return InterpolateInt(front_right, scene_width - 1, ratio);
    };

    const SurfaceStyle left_surface =
        left_state.known ? (left_state.open ? SurfaceStyle::kLeftOpen : SurfaceStyle::kLeftWall)
                         : SurfaceStyle::kLeftWall;
    const SurfaceStyle right_surface = right_state.known
                                           ? (right_state.open ? SurfaceStyle::kRightOpen
                                                               : SurfaceStyle::kRightWall)
                                           : SurfaceStyle::kRightWall;

    for (int y = 0; y < scene_height; ++y)
    {
        fill_span(y, 0, left_inner_x(y), left_surface);
        fill_span(y, right_inner_x(y), scene_width - 1, right_surface);
    }

    for (int y = 0; y <= front_top; ++y)
    {
        const int denom = std::max(1, front_top);
        const double ratio = static_cast<double>(y) / static_cast<double>(denom);
        const int x0 = InterpolateInt(0, front_left, ratio);
        const int x1 = InterpolateInt(scene_width - 1, front_right, ratio);
        fill_span(y, x0, x1, SurfaceStyle::kCeiling);
    }

    for (int y = front_bottom; y < scene_height; ++y)
    {
        const int denom = std::max(1, (scene_height - 1) - front_bottom);
        const double ratio = static_cast<double>(y - front_bottom) / static_cast<double>(denom);
        const int x0 = InterpolateInt(front_left, 0, ratio);
        const int x1 = InterpolateInt(front_right, scene_width - 1, ratio);
        fill_span(y, x0, x1, SurfaceStyle::kFloor);
    }

    SurfaceStyle front_surface = SurfaceStyle::kFrontUnknown;
    if (!front_unknown && front_state.known)
    {
        front_surface = front_state.open ? SurfaceStyle::kFrontOpen : SurfaceStyle::kFrontWall;
    }
    for (int y = front_top; y <= front_bottom; ++y)
    {
        fill_span(y, front_left, front_right, front_surface);
    }

    DrawLine(canvas, 0, 0, scene_width - 1, 0, '-');
    DrawLine(canvas, 0, scene_height - 1, scene_width - 1, scene_height - 1, '-');
    DrawLine(canvas, 0, 0, 0, scene_height - 1, '|');
    DrawLine(canvas, scene_width - 1, 0, scene_width - 1, scene_height - 1, '|');
    set_char(0, 0, '+');
    set_char(scene_width - 1, 0, '+');
    set_char(0, scene_height - 1, '+');
    set_char(scene_width - 1, scene_height - 1, '+');

    DrawLine(canvas, 0, 0, front_left, front_top, '/');
    DrawLine(canvas, scene_width - 1, 0, front_right, front_top, '\\');
    DrawLine(canvas, 0, scene_height - 1, front_left, front_bottom, '\\');
    DrawLine(canvas, scene_width - 1, scene_height - 1, front_right, front_bottom, '/');

    DrawLine(canvas, front_left, front_top, front_right, front_top, '-');
    DrawLine(canvas, front_left, front_bottom, front_right, front_bottom, '-');
    DrawLine(canvas, front_left, front_top, front_left, front_bottom, '|');
    DrawLine(canvas, front_right, front_top, front_right, front_bottom, '|');
    set_char(front_left, front_top, '+');
    set_char(front_right, front_top, '+');
    set_char(front_left, front_bottom, '+');
    set_char(front_right, front_bottom, '+');

    if (nearest_actor_depth != std::numeric_limits<int>::max())
    {
        const int max_depth = std::max(1, view.max_depth());
        const int clamped_depth = std::clamp(nearest_actor_depth, 1, max_depth);
        const double ratio = static_cast<double>(clamped_depth) /
                             static_cast<double>(max_depth + 1);
        const int actor_x = (front_left + front_right) / 2;
        const int actor_y = front_top +
                            std::max(1, static_cast<int>(
                                            std::lround(static_cast<double>(front_bottom - front_top) *
                                                        ratio)));

        set_char(actor_x, actor_y, ActorGlyph(nearest_actor_kind));
        if (actor_x + 1 < front_right)
        {
            set_char(actor_x + 1, actor_y, FacingGlyph(nearest_actor_facing));
        }
    }

    std::ostringstream out;
    for (int y = 0; y < scene_height; ++y)
    {
        SurfaceStyle active_style = SurfaceStyle::kVoid;
        out << AnsiForStyle(active_style);
        for (int x = 0; x < scene_width; ++x)
        {
            const Pixel& pixel = canvas[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
            if (pixel.style != active_style)
            {
                active_style = pixel.style;
                out << AnsiForStyle(active_style);
            }
            out << pixel.ch;
        }
        out << "\x1b[0m\n";
    }

    out << "Front: " << CorridorStateText(front_state) << "  Left: " << CorridorStateText(left_state)
        << "  Right: " << CorridorStateText(right_state) << "\n";
    out << "Legend: @^ you, P> player, N< npc, perspective lines=room edges\n";
    return out.str();
}
} // namespace grpcmud::client
