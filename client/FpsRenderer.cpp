#include "FpsRenderer.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>

namespace grpcmud::client
{
namespace
{
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

std::string CenterText(const std::string& text, std::size_t width)
{
    if (text.size() >= width)
    {
        return text.substr(0, width);
    }
    const std::size_t pad_left = (width - text.size()) / 2;
    const std::size_t pad_right = width - text.size() - pad_left;
    return std::string(pad_left, ' ') + text + std::string(pad_right, ' ');
}
} // namespace

std::string FpsRenderer::Render(const mud::v1::FirstPersonView& view)
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

    const auto token = [&](int depth, int lane) -> std::string
    {
        const auto* cell = get_cell(depth, lane);
        if (cell == nullptr || !cell->visible())
        {
            return "??";
        }
        if (cell->actor_kind() != mud::v1::VisibleActor::KIND_UNSPECIFIED)
        {
            std::string out;
            out.push_back(ActorGlyph(cell->actor_kind()));
            out.push_back(FacingGlyph(cell->actor_facing()));
            return out;
        }
        if (!cell->open_forward())
        {
            return "##";
        }
        if (!cell->open_left() && !cell->open_right())
        {
            return "[]";
        }
        if (!cell->open_left())
        {
            return "|.";
        }
        if (!cell->open_right())
        {
            return ".|";
        }
        return "..";
    };

    std::string front_state = "unknown";
    std::string left_state = "unknown";
    std::string right_state = "unknown";
    if (const auto* center = get_cell(0, 0); center != nullptr && center->visible())
    {
        front_state = center->open_forward() ? "open" : "wall";
        left_state = center->open_left() ? "open" : "wall";
        right_state = center->open_right() ? "open" : "wall";
    }

    const auto row_depth2 = "[" + token(2, -2) + "][" + token(2, -1) + "][" + token(2, 0) + "][" +
                            token(2, 1) + "][" + token(2, 2) + "]";
    const auto row_depth1 =
        "[" + token(1, -1) + "]   [" + token(1, 0) + "]   [" + token(1, 1) + "]";
    const auto row_depth0 = "[" + token(0, 0) + "]";

    constexpr std::size_t kInnerWidth = 47;

    std::ostringstream out;
    out << "+" << std::string(kInnerWidth, '-') << "+\n";
    out << "|" << CenterText("Questing FPS View", kInnerWidth) << "|\n";
    out << "|" << std::string(kInnerWidth, ' ') << "|\n";
    out << "|" << CenterText(row_depth2, kInnerWidth) << "|\n";
    out << "|" << CenterText(row_depth1, kInnerWidth) << "|\n";
    out << "|" << CenterText(row_depth0, kInnerWidth) << "|\n";
    out << "+" << std::string(kInnerWidth, '-') << "+\n";
    out << "     Front: " << front_state << "  Left: " << left_state
        << "  Right: " << right_state << '\n';
    out << "     Legend: @^ you, P> player, N< npc, ## wall, ?? unseen\n";
    return out.str();
}
} // namespace grpcmud::client
