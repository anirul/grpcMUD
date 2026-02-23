#pragma once

#include <atomic>
#include <array>
#include <chrono>
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
    static std::string JsonEscape(const std::string& text);
    bool TryConnect();

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
    void HandleViewUpdate(const mud::v1::LocalViewUpdate& view);
    void UpdateTickIntervalEstimate(const mud::v1::TickEvent& tick);
    void UpdateCameraAnimation();
    void StartPredictedStepAnimation(mud::v1::StepKind kind);
    void ApplyCameraPose(frame::WindowInterface& window);
    bool RebuildSceneIfDirty(frame::WindowInterface& window);
    bool RenderFrame(frame::WindowInterface& window);
    bool DrawHud();
    void RegisterKeyBindings(frame::WindowInterface& window);
    bool TrySendGameplayCommand(mud::v1::ClientMessage message);

    std::string server_address_;
    std::string player_name_;
    std::array<char, 256> server_address_input_buffer_{};
    std::array<char, 128> player_name_input_buffer_{};
    bool connected_ = false;
    std::string connect_error_message_;

    GrpcSession session_;
    std::atomic<bool> running_{true};
    std::atomic<bool> server_closed_{false};
    std::atomic<std::uint64_t> latest_tick_id_{0};
    std::uint64_t last_gameplay_command_tick_id_ = 0;
    bool gameplay_command_sent_this_tick_ = false;
    std::uint64_t tick_interval_estimate_ms_ = 500;
    std::uint64_t last_tick_sample_id_ = 0;
    std::uint64_t last_tick_sample_time_ms_ = 0;
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
    std::string last_level_json_;
    bool camera_pose_initialized_ = false;
    bool camera_animating_ = false;
    float camera_current_x_ = 0.0f;
    float camera_current_z_ = 0.0f;
    float camera_current_yaw_ = 0.0f;
    float camera_start_x_ = 0.0f;
    float camera_start_z_ = 0.0f;
    float camera_start_yaw_ = 0.0f;
    float camera_target_x_ = 0.0f;
    float camera_target_z_ = 0.0f;
    float camera_target_yaw_ = 0.0f;
    std::chrono::steady_clock::time_point camera_animation_started_at_{};
    std::chrono::milliseconds camera_animation_duration_{500};

    std::mutex pending_messages_mutex_;
    std::deque<mud::v1::ServerMessage> pending_messages_;
};
} // namespace grpcmud::client
