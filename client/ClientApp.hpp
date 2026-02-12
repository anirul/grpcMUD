#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "GrpcSession.hpp"
#include "TerminalUi.hpp"
#include "gameplay.pb.h"

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
    static bool IsPrintableChar(int ch);

    std::string NextRequestId();
    bool SendMessage(mud::v1::ClientMessage message);
    bool SendLookRequest();
    bool SendStepRequest(mud::v1::StepKind kind);
    bool SendSayRequest(const std::string& text);
    bool SendGuardRequest();
    bool SendAttackRequest(mud::v1::WeaponKind weapon);
    void HandleServerMessage(const mud::v1::ServerMessage& message);
    void HandleServerClosed();
    void HandleKeyEvent(int ch);
    void HandleMoveInput(int ch);
    void HandleTextInput(int ch);
    bool TrySendMoveOrTurnCommand(mud::v1::ClientMessage message);

    std::string server_address_;
    std::string player_name_;

    TerminalUi ui_;
    GrpcSession session_;
    std::atomic<bool> running_{true};
    std::atomic<bool> server_closed_{false};
    std::atomic<std::uint64_t> latest_tick_id_{0};
    std::uint64_t last_move_command_tick_id_ = 0;
    bool move_command_sent_this_tick_ = false;
    std::uint64_t request_counter_ = 1;
};
} // namespace grpcmud::client
