#include "TerminalUi.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "FpsRenderer.hpp"
#include "MapRenderer.hpp"

namespace grpcmud::client
{
namespace
{
std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
    {
        lines.push_back(line);
    }

    if (!text.empty() && text.back() == '\n')
    {
        lines.push_back({});
    }
    return lines;
}

void BeginFrame(int columns, int rows)
{
#ifdef _WIN32
    const HANDLE output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output_handle != INVALID_HANDLE_VALUE)
    {
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(output_handle, &info))
        {
            const COORD home{0, 0};
            DWORD written = 0;
            const DWORD cells =
                static_cast<DWORD>(std::max(1, columns) * std::max(1, rows));
            FillConsoleOutputCharacterA(output_handle, ' ', cells, home, &written);
            FillConsoleOutputAttribute(output_handle, info.wAttributes, cells, home, &written);
            SetConsoleCursorPosition(output_handle, home);
            return;
        }
    }
#endif

    std::cout << "\x1B[2J\x1B[H";
}
} // namespace

TerminalUi::TerminalUi() : mode_(UiMode::kMove), max_log_lines_(64)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
}

void TerminalUi::SetMode(UiMode mode)
{
    std::lock_guard<std::mutex> lock(mutex_);
    mode_ = mode;
}

UiMode TerminalUi::GetMode() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return mode_;
}

void TerminalUi::SetInputBuffer(std::string value)
{
    std::lock_guard<std::mutex> lock(mutex_);
    input_buffer_ = std::move(value);
}

const std::string TerminalUi::GetInputBuffer() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return input_buffer_;
}

void TerminalUi::AppendInputChar(char c)
{
    std::lock_guard<std::mutex> lock(mutex_);
    input_buffer_.push_back(c);
}

void TerminalUi::BackspaceInput()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!input_buffer_.empty())
    {
        input_buffer_.pop_back();
    }
}

void TerminalUi::ClearInput()
{
    std::lock_guard<std::mutex> lock(mutex_);
    input_buffer_.clear();
}

void TerminalUi::SetView(const mud::v1::LocalViewUpdate& view)
{
    std::lock_guard<std::mutex> lock(mutex_);
    view_ = view;
}

void TerminalUi::AddLog(std::string line)
{
    std::lock_guard<std::mutex> lock(mutex_);
    logs_.push_back(std::move(line));
    while (logs_.size() > max_log_lines_)
    {
        logs_.pop_front();
    }
}

void TerminalUi::SetDeathScreen(bool active, int seconds_remaining)
{
    std::lock_guard<std::mutex> lock(mutex_);
    death_screen_active_ = active;
    death_seconds_remaining_ = (seconds_remaining < 0) ? 0 : seconds_remaining;
}

void TerminalUi::TickDeathScreen()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!death_screen_active_)
    {
        return;
    }
    if (death_seconds_remaining_ > 0)
    {
        --death_seconds_remaining_;
    }
}

bool TerminalUi::IsDeathScreenActive() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return death_screen_active_;
}

void TerminalUi::ToggleRenderMode()
{
    std::lock_guard<std::mutex> lock(mutex_);
    render_map_debug_ = !render_map_debug_;
}

void TerminalUi::SetRenderMapDebug(bool enabled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    render_map_debug_ = enabled;
}

bool TerminalUi::IsRenderMapDebug() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return render_map_debug_;
}

TerminalUi::TerminalSize TerminalUi::DetectTerminalSize()
{
    TerminalSize size;

#ifdef _WIN32
    const HANDLE output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output_handle != INVALID_HANDLE_VALUE)
    {
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(output_handle, &info))
        {
            size.columns = static_cast<int>(info.srWindow.Right - info.srWindow.Left + 1);
            size.rows = static_cast<int>(info.srWindow.Bottom - info.srWindow.Top + 1);
        }
    }
#else
    if (const char* columns = std::getenv("COLUMNS"))
    {
        size.columns = std::max(1, std::atoi(columns));
    }
    if (const char* rows = std::getenv("LINES"))
    {
        size.rows = std::max(1, std::atoi(rows));
    }
#endif

    size.columns = std::max(size.columns, 20);
    size.rows = std::max(size.rows, 10);
    return size;
}

void TerminalUi::Render() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const TerminalSize terminal_size = DetectTerminalSize();

    std::vector<std::string> lines;
    lines.reserve(static_cast<std::size_t>(terminal_size.rows));

    const std::string view_label = render_map_debug_ ? "Map" : "FPS";
    lines.push_back("grpcMUD | mode=" + std::string(ModeName(mode_)) + " | view=" + view_label +
                    " | term=" + std::to_string(terminal_size.columns) + "x" +
                    std::to_string(terminal_size.rows));

    if (death_screen_active_)
    {
        lines.push_back("==========================");
        lines.push_back("      YOU ARE DOWN");
        lines.push_back("Respawn in " + std::to_string(death_seconds_remaining_) + " second(s).");
        lines.push_back("==========================");
    }
    else if (view_)
    {
        lines.push_back("Center: " + view_->center_square_id() + " (" +
                        std::to_string(view_->center_x()) + "," +
                        std::to_string(view_->center_y()) + ") facing " +
                        FacingLabel(view_->facing()));

        if (!render_map_debug_ && view_->has_first_person())
        {
            const std::string frame =
                FpsRenderer::Render(view_->first_person(), terminal_size.columns, terminal_size.rows);
            const auto frame_lines = SplitLines(frame);
            lines.insert(lines.end(), frame_lines.begin(), frame_lines.end());
        }
        else
        {
            const auto map_lines = SplitLines(MapRenderer::Render(*view_));
            lines.insert(lines.end(), map_lines.begin(), map_lines.end());
        }
    }
    else
    {
        lines.push_back("Waiting for local view...");
    }

    std::string controls_line;
    switch (mode_)
    {
    case UiMode::kMove:
        if (death_screen_active_)
        {
            controls_line = "[Dead] controls disabled. Q=quit";
        }
        else
        {
            controls_line =
                "[Move] W/S move A/D turn 1 melee 2 ranged 3 guard V view Enter chat P ping Q quit";
        }
        break;
    case UiMode::kCustomCommand:
        if (death_screen_active_)
        {
            controls_line = "[Dead] controls disabled. Q=quit";
        }
        else
        {
            controls_line =
                "[Input] Enter send, /view map|fps, /look, /move, /turn, /say, /guard, /attack, /ping: " +
                input_buffer_;
        }
        break;
    case UiMode::kExplore:
    case UiMode::kCommandSelect:
    case UiMode::kSay:
        if (death_screen_active_)
        {
            controls_line = "[Dead] controls disabled. Q=quit";
        }
        else
        {
            controls_line =
                "[Move] W/S move A/D turn 1 melee 2 ranged 3 guard V view Enter chat P ping Q quit";
        }
        break;
    }

    const int rows_for_logs =
        std::max(0, terminal_size.rows - static_cast<int>(lines.size()) - 1);
    if (rows_for_logs > 1)
    {
        lines.push_back("Logs:");
        const int log_slots = rows_for_logs - 1;
        const int log_count = std::min<int>(log_slots, static_cast<int>(logs_.size()));
        const int start = static_cast<int>(logs_.size()) - log_count;
        for (int index = start; index < static_cast<int>(logs_.size()); ++index)
        {
            lines.push_back("  " + logs_[static_cast<std::size_t>(index)]);
        }
    }

    lines.push_back(controls_line);

    if (static_cast<int>(lines.size()) > terminal_size.rows)
    {
        lines.resize(static_cast<std::size_t>(terminal_size.rows));
    }

    BeginFrame(terminal_size.columns, terminal_size.rows);
    for (int row = 0; row < terminal_size.rows; ++row)
    {
        if (row < static_cast<int>(lines.size()))
        {
            std::cout << lines[static_cast<std::size_t>(row)];
        }
        std::cout << "\x1B[K";
        if (row + 1 < terminal_size.rows)
        {
            std::cout << '\n';
        }
    }

    std::cout.flush();
}

const char* TerminalUi::ModeName(UiMode mode)
{
    switch (mode)
    {
    case UiMode::kExplore:
        return "Move";
    case UiMode::kCommandSelect:
        return "Move";
    case UiMode::kMove:
        return "Move";
    case UiMode::kSay:
        return "Input";
    case UiMode::kCustomCommand:
        return "Input";
    }
    return "Unknown";
}

std::string TerminalUi::FacingLabel(mud::v1::Direction direction)
{
    switch (direction)
    {
    case mud::v1::DIRECTION_NORTH:
        return "north";
    case mud::v1::DIRECTION_EAST:
        return "east";
    case mud::v1::DIRECTION_SOUTH:
        return "south";
    case mud::v1::DIRECTION_WEST:
        return "west";
    case mud::v1::DIRECTION_UNSPECIFIED:
    default:
        return "unknown";
    }
}
} // namespace grpcmud::client
