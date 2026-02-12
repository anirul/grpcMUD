#include "FpsRenderer.hpp"

#include <algorithm>
#include <array>
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
    kFrontUnknown,
    kEdge,
    kActorSelf,
    kActorPlayer,
    kActorNpc
};

enum class CorridorFlag
{
    kUnknown,
    kOpen,
    kWall
};

struct Rect
{
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
};

struct Point
{
    int x = 0;
    int y = 0;
};

Point LerpPoint(const Point& from, const Point& to, double t)
{
    return Point{
        static_cast<int>(std::lround(static_cast<double>(from.x) +
                                     static_cast<double>(to.x - from.x) * t)),
        static_cast<int>(std::lround(static_cast<double>(from.y) +
                                     static_cast<double>(to.y - from.y) * t)),
    };
}

std::int64_t CellKey(int depth, int lane)
{
    return (static_cast<std::int64_t>(depth) << 32) ^ static_cast<std::uint32_t>(lane + 1024);
}

int StyleColor(SurfaceStyle style)
{
    switch (style)
    {
    case SurfaceStyle::kVoid:
        return 16;
    case SurfaceStyle::kCeiling:
        return 244;
    case SurfaceStyle::kFloor:
        return 240;
    case SurfaceStyle::kLeftWall:
        return 62;
    case SurfaceStyle::kLeftOpen:
        return 24;
    case SurfaceStyle::kRightWall:
        return 96;
    case SurfaceStyle::kRightOpen:
        return 53;
    case SurfaceStyle::kFrontWall:
        return 246;
    case SurfaceStyle::kFrontOpen:
        return 236;
    case SurfaceStyle::kFrontUnknown:
        return 239;
    case SurfaceStyle::kEdge:
        return 255;
    case SurfaceStyle::kActorSelf:
        return 226;
    case SurfaceStyle::kActorPlayer:
        return 45;
    case SurfaceStyle::kActorNpc:
        return 196;
    }
    return 16;
}

void SetPixel(std::vector<std::vector<SurfaceStyle>>& canvas, int x, int y, SurfaceStyle style)
{
    const int height = static_cast<int>(canvas.size());
    if (height <= 0)
    {
        return;
    }
    const int width = static_cast<int>(canvas[0].size());
    if (x < 0 || x >= width || y < 0 || y >= height)
    {
        return;
    }
    canvas[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = style;
}

void DrawLine(std::vector<std::vector<SurfaceStyle>>& canvas, Point start, Point end,
              SurfaceStyle style)
{
    const int dx = end.x - start.x;
    const int dy = end.y - start.y;
    const int steps = std::max(std::abs(dx), std::abs(dy));
    if (steps <= 0)
    {
        SetPixel(canvas, start.x, start.y, style);
        return;
    }

    for (int step = 0; step <= steps; ++step)
    {
        const double ratio = static_cast<double>(step) / static_cast<double>(steps);
        const int x = static_cast<int>(std::lround(static_cast<double>(start.x) +
                                                   static_cast<double>(dx) * ratio));
        const int y = static_cast<int>(std::lround(static_cast<double>(start.y) +
                                                   static_cast<double>(dy) * ratio));
        SetPixel(canvas, x, y, style);
    }
}

void FillPolygon(std::vector<std::vector<SurfaceStyle>>& canvas, const std::vector<Point>& points,
                 SurfaceStyle style)
{
    const int height = static_cast<int>(canvas.size());
    if (height <= 0 || points.size() < 3)
    {
        return;
    }
    const int width = static_cast<int>(canvas[0].size());

    int min_y = points[0].y;
    int max_y = points[0].y;
    for (const Point& point : points)
    {
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }

    min_y = std::max(min_y, 0);
    max_y = std::min(max_y, height - 1);

    for (int y = min_y; y <= max_y; ++y)
    {
        std::vector<double> intersections;
        intersections.reserve(points.size());

        for (std::size_t i = 0; i < points.size(); ++i)
        {
            const Point& a = points[i];
            const Point& b = points[(i + 1) % points.size()];
            if (a.y == b.y)
            {
                continue;
            }

            if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y))
            {
                const double t = static_cast<double>(y - a.y) /
                                 static_cast<double>(b.y - a.y);
                intersections.push_back(static_cast<double>(a.x) +
                                        static_cast<double>(b.x - a.x) * t);
            }
        }

        if (intersections.size() < 2)
        {
            continue;
        }

        std::sort(intersections.begin(), intersections.end());
        for (std::size_t i = 0; i + 1 < intersections.size(); i += 2)
        {
            int x_start = static_cast<int>(std::ceil(intersections[i]));
            int x_end = static_cast<int>(std::floor(intersections[i + 1]));
            x_start = std::max(x_start, 0);
            x_end = std::min(x_end, width - 1);
            for (int x = x_start; x <= x_end; ++x)
            {
                SetPixel(canvas, x, y, style);
            }
        }
    }
}

void FillQuad(std::vector<std::vector<SurfaceStyle>>& canvas, const std::array<Point, 4>& quad,
              SurfaceStyle style)
{
    FillPolygon(canvas, std::vector<Point>(quad.begin(), quad.end()), style);
}

Rect MakePerspectiveRect(int width, int height, int depth_index, int max_depth_index)
{
    const int scene_size = std::max(8, std::min(width, height) - 2);
    const int half_base = std::max(4, scene_size / 2);

    const double ratio =
        static_cast<double>(depth_index) / static_cast<double>(std::max(1, max_depth_index));
    const double curve = std::pow(ratio, 1.05);
    const double min_scale = 0.18;
    const double scale = 1.0 - (1.0 - min_scale) * curve;

    const int half_size = std::max(3, static_cast<int>(std::lround(half_base * scale)));
    const int center_x = width / 2;
    const int center_y = height / 2;

    Rect rect;
    rect.left = std::clamp(center_x - half_size, 0, width - 1);
    rect.right = std::clamp(center_x + half_size, rect.left + 2, width - 1);
    rect.top = std::clamp(center_y - half_size, 0, height - 1);
    rect.bottom = std::clamp(center_y + half_size, rect.top + 2, height - 1);
    return rect;
}

CorridorFlag SideFlag(const mud::v1::FirstPersonCell* cell, bool left)
{
    if (cell == nullptr || !cell->visible())
    {
        return CorridorFlag::kUnknown;
    }
    return left ? (cell->open_left() ? CorridorFlag::kOpen : CorridorFlag::kWall)
                : (cell->open_right() ? CorridorFlag::kOpen : CorridorFlag::kWall);
}

std::string CorridorText(CorridorFlag flag)
{
    switch (flag)
    {
    case CorridorFlag::kOpen:
        return "open";
    case CorridorFlag::kWall:
        return "wall";
    case CorridorFlag::kUnknown:
    default:
        return "unknown";
    }
}

SurfaceStyle SideStyle(CorridorFlag flag, bool left)
{
    switch (flag)
    {
    case CorridorFlag::kOpen:
        return left ? SurfaceStyle::kLeftOpen : SurfaceStyle::kRightOpen;
    case CorridorFlag::kWall:
        return left ? SurfaceStyle::kLeftWall : SurfaceStyle::kRightWall;
    case CorridorFlag::kUnknown:
    default:
        return left ? SurfaceStyle::kLeftWall : SurfaceStyle::kRightWall;
    }
}

void FillSidePanel(std::vector<std::vector<SurfaceStyle>>& canvas, const Rect& near_rect,
                   const Rect& far_rect, bool left_side, CorridorFlag flag)
{
    const Point near_top = left_side ? Point{near_rect.left, near_rect.top}
                                     : Point{near_rect.right, near_rect.top};
    const Point near_bottom = left_side ? Point{near_rect.left, near_rect.bottom}
                                        : Point{near_rect.right, near_rect.bottom};
    const Point far_top = left_side ? Point{far_rect.left, far_rect.top}
                                    : Point{far_rect.right, far_rect.top};
    const Point far_bottom = left_side ? Point{far_rect.left, far_rect.bottom}
                                       : Point{far_rect.right, far_rect.bottom};

    if (flag != CorridorFlag::kOpen)
    {
        FillQuad(canvas,
                 {{{near_top.x, near_top.y},
                   {near_bottom.x, near_bottom.y},
                   {far_bottom.x, far_bottom.y},
                   {far_top.x, far_top.y}}},
                 SideStyle(flag, left_side));
        return;
    }

    const Point near_split_top = LerpPoint(near_top, near_bottom, 0.34);
    const Point near_split_bottom = LerpPoint(near_top, near_bottom, 0.68);
    const Point far_split_top = LerpPoint(far_top, far_bottom, 0.34);
    const Point far_split_bottom = LerpPoint(far_top, far_bottom, 0.68);

    FillQuad(canvas,
             {{{near_top.x, near_top.y},
               {near_split_top.x, near_split_top.y},
               {far_split_top.x, far_split_top.y},
               {far_top.x, far_top.y}}},
             SurfaceStyle::kCeiling);

    FillQuad(canvas,
             {{{near_split_top.x, near_split_top.y},
               {near_split_bottom.x, near_split_bottom.y},
               {far_split_bottom.x, far_split_bottom.y},
               {far_split_top.x, far_split_top.y}}},
             left_side ? SurfaceStyle::kLeftOpen : SurfaceStyle::kRightOpen);

    FillQuad(canvas,
             {{{near_split_bottom.x, near_split_bottom.y},
               {near_bottom.x, near_bottom.y},
               {far_bottom.x, far_bottom.y},
               {far_split_bottom.x, far_split_bottom.y}}},
             SurfaceStyle::kFloor);
}

void DrawOpenSideGuides(std::vector<std::vector<SurfaceStyle>>& canvas, const Rect& near_rect,
                        const Rect& far_rect, bool left_side)
{
    const Point near_top = left_side ? Point{near_rect.left, near_rect.top}
                                     : Point{near_rect.right, near_rect.top};
    const Point near_bottom = left_side ? Point{near_rect.left, near_rect.bottom}
                                        : Point{near_rect.right, near_rect.bottom};
    const Point far_top = left_side ? Point{far_rect.left, far_rect.top}
                                    : Point{far_rect.right, far_rect.top};
    const Point far_bottom = left_side ? Point{far_rect.left, far_rect.bottom}
                                       : Point{far_rect.right, far_rect.bottom};

    const Point near_band_top = LerpPoint(near_top, near_bottom, 0.34);
    const Point near_band_bottom = LerpPoint(near_top, near_bottom, 0.68);
    const Point far_band_top = LerpPoint(far_top, far_bottom, 0.34);
    const Point far_band_bottom = LerpPoint(far_top, far_bottom, 0.68);

    DrawLine(canvas, near_band_top, near_band_bottom, SurfaceStyle::kEdge);
    DrawLine(canvas, far_band_top, far_band_bottom, SurfaceStyle::kEdge);
    DrawLine(canvas, near_band_top, far_band_top, SurfaceStyle::kEdge);
    DrawLine(canvas, near_band_bottom, far_band_bottom, SurfaceStyle::kEdge);

    for (double t : {0.25, 0.5, 0.75})
    {
        const Point top = LerpPoint(near_band_top, far_band_top, t);
        const Point bottom = LerpPoint(near_band_bottom, far_band_bottom, t);
        DrawLine(canvas, top, bottom, SurfaceStyle::kEdge);
    }
}

SurfaceStyle ActorStyle(mud::v1::VisibleActor::Kind actor_kind)
{
    switch (actor_kind)
    {
    case mud::v1::VisibleActor::KIND_SELF:
        return SurfaceStyle::kActorSelf;
    case mud::v1::VisibleActor::KIND_PLAYER:
        return SurfaceStyle::kActorPlayer;
    case mud::v1::VisibleActor::KIND_NPC:
        return SurfaceStyle::kActorNpc;
    case mud::v1::VisibleActor::KIND_UNSPECIFIED:
    default:
        return SurfaceStyle::kFrontUnknown;
    }
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

    const int max_depth = std::max(1, view.max_depth());
    const int depth_layers = std::max(4, max_depth + 2);

    const int char_width = std::clamp(terminal_columns - 2, 36, 140);
    const int char_height = std::clamp(terminal_rows - 9, 8, 34);
    const int pixel_width = char_width;
    const int pixel_height = char_height * 2;

    std::vector<std::vector<SurfaceStyle>> canvas(
        static_cast<std::size_t>(pixel_height),
        std::vector<SurfaceStyle>(static_cast<std::size_t>(pixel_width), SurfaceStyle::kVoid));

    std::vector<Rect> rects;
    rects.reserve(static_cast<std::size_t>(depth_layers + 1));
    for (int depth_index = 0; depth_index <= depth_layers; ++depth_index)
    {
        rects.push_back(MakePerspectiveRect(pixel_width, pixel_height, depth_index, depth_layers));
    }

    CorridorFlag front_flag = CorridorFlag::kUnknown;
    int wall_depth = -1;
    const auto* center = get_cell(0, 0);
    if (center != nullptr && center->visible())
    {
        if (!center->open_forward())
        {
            front_flag = CorridorFlag::kWall;
            wall_depth = 0;
        }
        else
        {
            bool unresolved = false;
            for (int depth = 1; depth <= max_depth; ++depth)
            {
                const auto* depth_cell = get_cell(depth, 0);
                if (depth_cell == nullptr || !depth_cell->visible())
                {
                    unresolved = true;
                    break;
                }
                if (!depth_cell->open_forward())
                {
                    front_flag = CorridorFlag::kWall;
                    wall_depth = depth;
                    unresolved = false;
                    break;
                }
            }

            if (wall_depth < 0)
            {
                front_flag = unresolved ? CorridorFlag::kUnknown : CorridorFlag::kOpen;
            }
        }
    }

    int front_layer = depth_layers;
    if (wall_depth >= 0)
    {
        front_layer = std::min(depth_layers, wall_depth + 1);
    }
    else if (front_flag == CorridorFlag::kUnknown)
    {
        front_layer = std::min(depth_layers, max_depth + 1);
    }
    front_layer = std::max(front_layer, 1);

    CorridorFlag left_flag = CorridorFlag::kUnknown;
    CorridorFlag right_flag = CorridorFlag::kUnknown;
    if (center != nullptr && center->visible())
    {
        left_flag = center->open_left() ? CorridorFlag::kOpen : CorridorFlag::kWall;
        right_flag = center->open_right() ? CorridorFlag::kOpen : CorridorFlag::kWall;
    }

    for (int layer = 0; layer < front_layer; ++layer)
    {
        const Rect& near_rect = rects[static_cast<std::size_t>(layer)];
        const Rect& far_rect = rects[static_cast<std::size_t>(layer + 1)];

        const int sample_depth = std::min(layer, max_depth);
        const auto* sample_cell = get_cell(sample_depth, 0);
        const CorridorFlag layer_left_flag = SideFlag(sample_cell, true);
        const CorridorFlag layer_right_flag = SideFlag(sample_cell, false);

        FillSidePanel(canvas, near_rect, far_rect, true, layer_left_flag);
        FillSidePanel(canvas, near_rect, far_rect, false, layer_right_flag);
        if (layer_left_flag == CorridorFlag::kOpen)
        {
            DrawOpenSideGuides(canvas, near_rect, far_rect, true);
        }
        if (layer_right_flag == CorridorFlag::kOpen)
        {
            DrawOpenSideGuides(canvas, near_rect, far_rect, false);
        }

        FillQuad(canvas,
                 {{{near_rect.left, near_rect.top},
                   {near_rect.right, near_rect.top},
                   {far_rect.right, far_rect.top},
                   {far_rect.left, far_rect.top}}},
                 SurfaceStyle::kCeiling);

        FillQuad(canvas,
                 {{{near_rect.left, near_rect.bottom},
                   {near_rect.right, near_rect.bottom},
                   {far_rect.right, far_rect.bottom},
                   {far_rect.left, far_rect.bottom}}},
                 SurfaceStyle::kFloor);

    }

    const Rect& front_rect = rects[static_cast<std::size_t>(front_layer)];
    SurfaceStyle front_style = SurfaceStyle::kFrontUnknown;
    if (front_flag == CorridorFlag::kWall)
    {
        front_style = SurfaceStyle::kFrontWall;
    }
    else if (front_flag == CorridorFlag::kOpen)
    {
        front_style = SurfaceStyle::kVoid;
    }
    FillQuad(canvas,
             {{{front_rect.left, front_rect.top},
               {front_rect.right, front_rect.top},
               {front_rect.right, front_rect.bottom},
               {front_rect.left, front_rect.bottom}}},
             front_style);

    const Rect& back_rect = rects.front();
    DrawLine(canvas, {back_rect.left, back_rect.top}, {front_rect.left, front_rect.top},
             SurfaceStyle::kEdge);
    DrawLine(canvas, {back_rect.right, back_rect.top}, {front_rect.right, front_rect.top},
             SurfaceStyle::kEdge);
    DrawLine(canvas, {back_rect.left, back_rect.bottom}, {front_rect.left, front_rect.bottom},
             SurfaceStyle::kEdge);
    DrawLine(canvas, {back_rect.right, back_rect.bottom}, {front_rect.right, front_rect.bottom},
             SurfaceStyle::kEdge);

    DrawLine(canvas, {front_rect.left, front_rect.top}, {front_rect.right, front_rect.top},
             SurfaceStyle::kEdge);
    DrawLine(canvas, {front_rect.left, front_rect.bottom}, {front_rect.right, front_rect.bottom},
             SurfaceStyle::kEdge);
    DrawLine(canvas, {front_rect.left, front_rect.top}, {front_rect.left, front_rect.bottom},
             SurfaceStyle::kEdge);
    DrawLine(canvas, {front_rect.right, front_rect.top}, {front_rect.right, front_rect.bottom},
             SurfaceStyle::kEdge);

    for (int layer = 1; layer <= front_layer; ++layer)
    {
        const Rect& level = rects[static_cast<std::size_t>(layer)];
        DrawLine(canvas, {level.left, level.top}, {level.right, level.top}, SurfaceStyle::kEdge);
        DrawLine(canvas, {level.left, level.bottom}, {level.right, level.bottom},
                 SurfaceStyle::kEdge);
        DrawLine(canvas, {level.left, level.top}, {level.left, level.bottom}, SurfaceStyle::kEdge);
        DrawLine(canvas, {level.right, level.top}, {level.right, level.bottom},
                 SurfaceStyle::kEdge);
    }

    int nearest_actor_depth = std::numeric_limits<int>::max();
    mud::v1::VisibleActor::Kind nearest_actor_kind = mud::v1::VisibleActor::KIND_UNSPECIFIED;
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
        }
    }

    if (nearest_actor_depth != std::numeric_limits<int>::max())
    {
        const int actor_layer = std::min(front_layer, nearest_actor_depth + 1);
        const Rect& actor_rect = rects[static_cast<std::size_t>(actor_layer)];
        const int center_x = (actor_rect.left + actor_rect.right) / 2;
        const int center_y = (actor_rect.top + actor_rect.bottom) / 2;
        const SurfaceStyle actor_style = ActorStyle(nearest_actor_kind);
        const int actor_width = actor_rect.right - actor_rect.left + 1;
        const int actor_height = actor_rect.bottom - actor_rect.top + 1;

        int half_w = std::clamp(actor_width / 10, 1, 6);
        int half_h = std::clamp(actor_height / 8, 2, 8);
        if (nearest_actor_kind == mud::v1::VisibleActor::KIND_NPC)
        {
            half_w = std::max(half_w, 2);
            half_h = std::max(half_h, 3);
        }

        for (int dy = -half_h; dy <= half_h; ++dy)
        {
            for (int dx = -half_w; dx <= half_w; ++dx)
            {
                SetPixel(canvas, center_x + dx, center_y + dy, actor_style);
            }
        }
    }

    std::ostringstream out;
    for (int y = 0; y < pixel_height; y += 2)
    {
        int active_fg = -1;
        int active_bg = -1;

        for (int x = 0; x < pixel_width; ++x)
        {
            const SurfaceStyle top_style =
                canvas[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
            const SurfaceStyle bottom_style =
                canvas[static_cast<std::size_t>(std::min(y + 1, pixel_height - 1))]
                      [static_cast<std::size_t>(x)];

            const int top_color = StyleColor(top_style);
            const int bottom_color = StyleColor(bottom_style);
            const bool split = (top_color != bottom_color);

            const int fg = split ? top_color : bottom_color;
            const int bg = bottom_color;
            if (fg != active_fg || bg != active_bg)
            {
                out << "\x1b[38;5;" << fg << "m\x1b[48;5;" << bg << "m";
                active_fg = fg;
                active_bg = bg;
            }

            out << (split ? "\xE2\x96\x80" : " ");
        }

        out << "\x1b[0m\n";
    }

    out << "Front: " << CorridorText(front_flag) << "  Left: " << CorridorText(left_flag)
        << "  Right: " << CorridorText(right_flag) << "\n";
    out << "Open sides continue as ceiling/open-space/floor bands. Actor colors: yellow=self cyan=player red=npc\n";
    return out.str();
}
} // namespace grpcmud::client
