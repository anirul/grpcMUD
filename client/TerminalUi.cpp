#include "TerminalUi.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <utility>

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
TerminalUi::TerminalUi() : mode_(UiMode::kMove), max_log_lines_(14)
{
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

    std::cout << "\x1B[2J\x1B[H";
    std::cout << "grpcMUD Tactical Client" << '\n';
    std::cout << "Mode: " << ModeName(mode_) << '\n';
    std::cout << "Terminal: " << terminal_size.columns << "x" << terminal_size.rows << '\n';

    if (death_screen_active_)
    {
        std::cout << "==========================" << '\n';
        std::cout << "      YOU ARE DOWN" << '\n';
        std::cout << "Respawn in " << death_seconds_remaining_ << " second(s)." << '\n';
        std::cout << "==========================" << '\n';
    }
    else if (view_)
    {
        std::cout << "Center: " << view_->center_square_id() << " (" << view_->center_x() << ","
                  << view_->center_y() << ") "
                  << "Facing: " << FacingLabel(view_->facing()) << '\n';
        std::cout << "View: " << (render_map_debug_ ? "Map (debug)" : "FPS") << '\n';
        if (!render_map_debug_ && view_->has_first_person())
        {
            std::cout << FpsRenderer::Render(view_->first_person(), terminal_size.columns,
                                             terminal_size.rows);
        }
        else
        {
            std::cout << "Legend: @^=you P>=player N<=npc .=floor ###=wall ???=fog walls=|---"
                      << '\n';
            std::cout << MapRenderer::Render(*view_);
        }
    }
    else
    {
        std::cout << "Waiting for local view..." << '\n';
    }

    std::cout << '\n';
    std::cout << "Logs:" << '\n';
    for (const auto& line : logs_)
    {
        std::cout << "  " << line << '\n';
    }

    std::cout << '\n';
    switch (mode_)
    {
    case UiMode::kMove:
        if (death_screen_active_)
        {
            std::cout << "[Dead] Controls disabled while knocked out. Q:quit" << '\n';
        }
        else
        {
            std::cout << "[Move] W/S:move A/D:turn 1:melee 2:ranged 3:guard V:view Enter:chat(/cmd) P:ping Q:quit"
                      << '\n';
        }
        break;
    case UiMode::kCustomCommand:
        if (death_screen_active_)
        {
            std::cout << "[Dead] Controls disabled while knocked out. Q:quit" << '\n';
        }
        else
        {
            std::cout << "[Input] Enter to send. Chat by default, '/<cmd>' for commands ('/view map|fps'): "
                      << input_buffer_ << '\n';
        }
        break;
    case UiMode::kExplore:
    case UiMode::kCommandSelect:
    case UiMode::kSay:
        if (death_screen_active_)
        {
            std::cout << "[Dead] Controls disabled while knocked out. Q:quit" << '\n';
        }
        else
        {
            std::cout << "[Move] W/S:move A/D:turn 1:melee 2:ranged 3:guard V:view Enter:chat(/cmd) P:ping Q:quit"
                      << '\n';
        }
        break;
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
