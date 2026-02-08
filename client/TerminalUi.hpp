#pragma once

#include <deque>
#include <mutex>
#include <optional>
#include <string>

#include "mud.pb.h"

namespace grpcmud::client
{
enum class UiMode
{
    kExplore,
    kCommandSelect,
    kMove,
    kSay,
    kCustomCommand
};

class TerminalUi
{
public:
    TerminalUi();

    void SetMode(UiMode mode);
    UiMode GetMode() const;

    void SetInputBuffer(std::string value);
    const std::string GetInputBuffer() const;
    void AppendInputChar(char c);
    void BackspaceInput();
    void ClearInput();

    void SetView(const mud::v1::LocalViewUpdate& view);
    void AddLog(std::string line);
    void SetDeathScreen(bool active, int seconds_remaining);
    void TickDeathScreen();
    bool IsDeathScreenActive() const;

    void Render() const;

private:
    static const char* ModeName(UiMode mode);
    static std::string FacingLabel(mud::v1::Direction direction);

    mutable std::mutex mutex_;
    std::optional<mud::v1::LocalViewUpdate> view_;
    std::deque<std::string> logs_;
    std::string input_buffer_;
    UiMode mode_;
    std::size_t max_log_lines_;
    bool death_screen_active_ = false;
    int death_seconds_remaining_ = 0;
};
} // namespace grpcmud::client
