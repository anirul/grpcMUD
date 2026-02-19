#pragma once

#include <atomic>
#include <array>
#include <deque>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "GrpcSession.hpp"
#include "gameplay.pb.h"

namespace frame
{
struct WindowInterface;
}

namespace grpcmud::client
{
class ClientApp
{
public:
    int Run(int argc, char** argv);

private:
    static std::string Trim(const std::string& text);
    static std::string ToLower(std::string text);
    static std::string ReadPlayerNameFromPrompt();
    static std::string JsonEscape(const std::string& text);

    std::string NextRequestId();
    bool SendMessage(mud::v1::ClientMessage message);
    bool SendLookRequest();
    bool SendStepRequest(mud::v1::StepKind kind);
    bool SendSayRequest(const std::string& text);
    bool SendGuardRequest();
    bool SendAttackRequest(mud::v1::WeaponKind weapon);
    void QueueServerMessage(const mud::v1::ServerMessage& message);
    void ProcessQueuedServerMessages();
    void HandleServerMessageOnMainThread(const mud::v1::ServerMessage& message);
    void HandleServerClosed();
    void HandleActionKey(char key);
    void HandleSubmittedText(const std::string& text);
    void AddLog(std::string line);
    void TickDeathScreen();
    bool IsInputFocused() const;
    void SetInputFocused(bool focused);
    bool ConsumeInputFocusRequest();
    void RequestInputFocus();

    std::string BuildBootstrapLevelJson() const;
    std::string BuildLevelJsonFromView(const mud::v1::LocalViewUpdate& view) const;
    bool RebuildSceneIfDirty(frame::WindowInterface& window);
    bool RenderFrame(frame::WindowInterface& window);
    bool DrawHud();
    void RegisterKeyBindings(frame::WindowInterface& window);
    bool TrySendMoveOrTurnCommand(mud::v1::ClientMessage message);

    std::string server_address_;
    std::string player_name_;

    GrpcSession session_;
    std::atomic<bool> running_{true};
    std::atomic<bool> server_closed_{false};
    std::atomic<std::uint64_t> latest_tick_id_{0};
    std::uint64_t last_move_command_tick_id_ = 0;
    bool move_command_sent_this_tick_ = false;
    std::uint64_t request_counter_ = 1;

    bool server_closed_logged_ = false;
    bool scene_dirty_ = true;
    bool death_screen_active_ = false;
    int death_seconds_remaining_ = 0;
    bool input_focused_ = false;
    bool input_focus_requested_ = false;
    std::deque<std::string> logs_;
    std::array<char, 512> chat_input_buffer_{};
    std::optional<mud::v1::LocalViewUpdate> latest_view_;

    std::mutex pending_messages_mutex_;
    std::deque<mud::v1::ServerMessage> pending_messages_;
};
} // namespace grpcmud::client
