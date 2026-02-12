#include "ClientApp.hpp"

#include <conio.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <thread>
#include <utility>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

ABSL_FLAG(std::string, server_address, "localhost:50051", "gRPC server address.");
ABSL_FLAG(std::string, player_name, "",
          "Player name to join as. If empty, the client prompts interactively.");

namespace grpcmud::client
{
int ClientApp::Run(int argc, char** argv)
{
    const auto positional_args = absl::ParseCommandLine(argc, argv);

    if (positional_args.size() > 3)
    {
        std::cerr << "Unexpected positional arguments. Use --help for usage." << std::endl;
        return 1;
    }

    server_address_ = Trim(absl::GetFlag(FLAGS_server_address));
    if (server_address_.empty())
    {
        std::cerr << "Server address cannot be empty." << std::endl;
        return 1;
    }
    if (positional_args.size() > 1)
    {
        server_address_ = Trim(positional_args[1]);
    }

    player_name_ = Trim(absl::GetFlag(FLAGS_player_name));
    if (positional_args.size() > 2)
    {
        player_name_ = Trim(positional_args[2]);
    }

    if (player_name_.empty())
    {
        player_name_ = ReadPlayerNameFromPrompt();
    }

    std::string error_message;
    if (!session_.Connect(server_address_, player_name_, &error_message))
    {
        std::cerr << error_message << std::endl;
        return 1;
    }

    session_.StartReader([this](const mud::v1::ServerMessage& message)
                         { HandleServerMessage(message); },
                         [this]() { HandleServerClosed(); });

    ui_.SetMode(UiMode::kMove);
    ui_.AddLog("Connected to " + server_address_ + " as '" + player_name_ + "'.");
    ui_.AddLog("FPS default. V toggles map/fps. Enter opens chat/commands.");
    ui_.Render();

    constexpr auto kRenderInterval = std::chrono::milliseconds(250);
    auto next_render_at = std::chrono::steady_clock::now() + kRenderInterval;

    while (running_.load(std::memory_order_relaxed))
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_render_at)
        {
            ui_.Render();
            next_render_at = now + kRenderInterval;
        }

        if (!_kbhit())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            if (server_closed_.load(std::memory_order_relaxed))
            {
                break;
            }
            continue;
        }

        int ch = _getch();
        if (ch == 0 || ch == 224)
        {
            // Consume extended key sequence (arrows, F-keys) and ignore.
            (void)_getch();
            continue;
        }

        HandleKeyEvent(ch);
    }

    const grpc::Status status = session_.Shutdown();
    if (!status.ok())
    {
        std::cerr << "RPC ended with error: [" << status.error_code() << "] "
                  << status.error_message() << std::endl;
        return 1;
    }

    return 0;
}

std::string ClientApp::Trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, (last - first) + 1);
}

std::string ClientApp::ToLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string ClientApp::ReadPlayerNameFromPrompt()
{
    std::string name;
    while (name.empty())
    {
        std::cout << "Enter player name: ";
        if (!std::getline(std::cin, name))
        {
            return {};
        }
        name = Trim(name);
        if (name.empty())
        {
            std::cout << "Player name cannot be empty." << '\n';
        }
    }
    return name;
}

bool ClientApp::IsPrintableChar(int ch)
{
    return ch >= 32 && ch <= 126;
}

std::string ClientApp::NextRequestId()
{
    return "req-" + std::to_string(request_counter_++);
}

bool ClientApp::SendMessage(mud::v1::ClientMessage message)
{
    if (!session_.SendClientMessage(message))
    {
        ui_.AddLog("[error] Failed to send command.");
        running_.store(false, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool ClientApp::SendLookRequest()
{
    mud::v1::ClientMessage message;
    message.mutable_look()->set_request_id(NextRequestId());
    return SendMessage(std::move(message));
}

bool ClientApp::SendStepRequest(mud::v1::StepKind kind)
{
    mud::v1::ClientMessage message;
    auto* step = message.mutable_step();
    step->set_request_id(NextRequestId());
    step->set_kind(kind);
    return TrySendMoveOrTurnCommand(std::move(message));
}

bool ClientApp::SendSayRequest(const std::string& text)
{
    const std::string trimmed = Trim(text);
    if (trimmed.empty())
    {
        return false;
    }

    mud::v1::ClientMessage message;
    auto* say = message.mutable_say();
    say->set_request_id(NextRequestId());
    say->set_text(trimmed);
    return SendMessage(std::move(message));
}

bool ClientApp::SendGuardRequest()
{
    mud::v1::ClientMessage message;
    message.mutable_guard()->set_request_id(NextRequestId());
    return SendMessage(std::move(message));
}

bool ClientApp::SendAttackRequest(mud::v1::WeaponKind weapon)
{
    mud::v1::ClientMessage message;
    auto* attack = message.mutable_attack();
    attack->set_request_id(NextRequestId());
    attack->set_weapon(weapon);
    return SendMessage(std::move(message));
}

void ClientApp::HandleServerMessage(const mud::v1::ServerMessage& message)
{
    auto maybe_respawn_seconds_from_text = [&](const std::string& text) -> int
    {
        const std::string lowered = ToLower(text);
        const auto marker = lowered.find("respawn in ");
        if (marker == std::string::npos)
        {
            return -1;
        }

        int value = 0;
        bool found_digit = false;
        for (std::size_t i = marker; i < lowered.size(); ++i)
        {
            if (!std::isdigit(static_cast<unsigned char>(lowered[i])))
            {
                if (found_digit)
                {
                    break;
                }
                continue;
            }

            found_digit = true;
            value = (value * 10) + (lowered[i] - '0');
        }
        return found_digit ? value : -1;
    };

    const std::uint64_t message_tick_id = message.tick_id();
    std::uint64_t observed_tick = latest_tick_id_.load(std::memory_order_relaxed);
    while (message_tick_id > observed_tick &&
           !latest_tick_id_.compare_exchange_weak(observed_tick, message_tick_id,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed))
    {
    }

    switch (message.payload_case())
    {
    case mud::v1::ServerMessage::kJoinAck:
        ui_.AddLog("[join] player_id=" + message.join_ack().player_id());
        ui_.AddLog("[motd] " + message.join_ack().motd());
        break;
    case mud::v1::ServerMessage::kCommandAck:
        if (!message.command_ack().accepted())
        {
            ui_.AddLog("[ack] " + message.command_ack().request_id() + " rejected \"" +
                       message.command_ack().message() + "\"");
            const int respawn_seconds =
                maybe_respawn_seconds_from_text(message.command_ack().message());
            if (respawn_seconds >= 0)
            {
                ui_.SetDeathScreen(true, respawn_seconds);
                ui_.SetMode(UiMode::kMove);
                ui_.ClearInput();
            }
        }
        break;
    case mud::v1::ServerMessage::kWorldEvent:
    {
        std::string prefix = "[event]";
        switch (message.world_event().kind())
        {
        case mud::v1::WorldEvent::KIND_ROOM:
            prefix = "[room]";
            break;
        case mud::v1::WorldEvent::KIND_CHAT:
            prefix = "[chat]";
            break;
        case mud::v1::WorldEvent::KIND_SYSTEM:
            prefix = "[system]";
            break;
        case mud::v1::WorldEvent::KIND_COMBAT:
            prefix = "[combat]";
            break;
        case mud::v1::WorldEvent::KIND_UNSPECIFIED:
        default:
            break;
        }
        ui_.AddLog(prefix + " " + message.world_event().text());
        const std::string lowered_text = ToLower(message.world_event().text());
        if (lowered_text.find("you were knocked out") != std::string::npos)
        {
            ui_.SetDeathScreen(true, 10);
            ui_.SetMode(UiMode::kMove);
            ui_.ClearInput();
        }
        else if (lowered_text.find("you respawn") != std::string::npos)
        {
            ui_.SetDeathScreen(false, 0);
            ui_.SetMode(UiMode::kMove);
            ui_.ClearInput();
        }
        break;
    }
    case mud::v1::ServerMessage::kError:
        ui_.AddLog("[error] " + message.error().code() + ": " + message.error().message());
        break;
    case mud::v1::ServerMessage::kPong:
        ui_.AddLog("[pong]");
        break;
    case mud::v1::ServerMessage::kTick:
        ui_.TickDeathScreen();
        break;
    case mud::v1::ServerMessage::kView:
        ui_.SetView(message.view());
        break;
    case mud::v1::ServerMessage::PAYLOAD_NOT_SET:
    default:
        ui_.AddLog("[server] message with no payload");
        break;
    }

    ui_.Render();
}

void ClientApp::HandleServerClosed()
{
    server_closed_.store(true, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    ui_.AddLog("[system] Connection closed.");
    ui_.Render();
}

void ClientApp::HandleKeyEvent(int ch)
{
    if (ui_.IsDeathScreenActive())
    {
        const char key = static_cast<char>(std::tolower(ch));
        if (key == 'q')
        {
            running_.store(false, std::memory_order_relaxed);
        }
        ui_.Render();
        return;
    }

    const UiMode mode = ui_.GetMode();
    if (mode == UiMode::kCustomCommand)
    {
        HandleTextInput(ch);
        return;
    }

    if (ch == '\r' || ch == '\n')
    {
        ui_.SetMode(UiMode::kCustomCommand);
        ui_.ClearInput();
        ui_.Render();
        return;
    }

    HandleMoveInput(ch);
}

void ClientApp::HandleMoveInput(int ch)
{
    const char key = static_cast<char>(std::tolower(ch));
    if (key == 'q')
    {
        running_.store(false, std::memory_order_relaxed);
        return;
    }
    if (key == '1')
    {
        SendAttackRequest(mud::v1::WEAPON_KIND_MELEE);
        ui_.Render();
        return;
    }
    if (key == '2')
    {
        SendAttackRequest(mud::v1::WEAPON_KIND_RANGED);
        ui_.Render();
        return;
    }
    if (key == '3')
    {
        SendGuardRequest();
        ui_.Render();
        return;
    }
    if (key == 'p')
    {
        if (!session_.SendPing())
        {
            ui_.AddLog("[error] Failed to send ping.");
            running_.store(false, std::memory_order_relaxed);
        }
        ui_.Render();
        return;
    }
    if (key == 'v')
    {
        ui_.ToggleRenderMode();
        ui_.AddLog(std::string("[ui] View mode: ") +
                   (ui_.IsRenderMapDebug() ? "map (debug)" : "fps"));
        ui_.Render();
        return;
    }
    if (key == '/')
    {
        ui_.SetMode(UiMode::kCustomCommand);
        ui_.SetInputBuffer("/");
        ui_.Render();
        return;
    }
    if (key == 'w' || key == 'k')
    {
        SendStepRequest(mud::v1::STEP_KIND_MOVE_FORWARD);
        ui_.Render();
        return;
    }
    if (key == 'a' || key == 'h')
    {
        SendStepRequest(mud::v1::STEP_KIND_TURN_LEFT);
        ui_.Render();
        return;
    }
    if (key == 's' || key == 'j')
    {
        SendStepRequest(mud::v1::STEP_KIND_MOVE_BACKWARD);
        ui_.Render();
        return;
    }
    if (key == 'd' || key == 'l')
    {
        SendStepRequest(mud::v1::STEP_KIND_TURN_RIGHT);
        ui_.Render();
        return;
    }
}

void ClientApp::HandleTextInput(int ch)
{
    if (ch == '\r' || ch == '\n')
    {
        const std::string text = Trim(ui_.GetInputBuffer());
        if (!text.empty())
        {
            if (text[0] == '/')
            {
                const std::string command = Trim(text.substr(1));
                const std::string lowered = ToLower(command);
                if (lowered == "view")
                {
                    ui_.ToggleRenderMode();
                    ui_.AddLog(std::string("[ui] View mode: ") +
                               (ui_.IsRenderMapDebug() ? "map (debug)" : "fps"));
                }
                else if (lowered == "view map")
                {
                    ui_.SetRenderMapDebug(true);
                    ui_.AddLog("[ui] View mode: map (debug)");
                }
                else if (lowered == "view fps")
                {
                    ui_.SetRenderMapDebug(false);
                    ui_.AddLog("[ui] View mode: fps");
                }
                else
                {
                    const auto split = command.find(' ');
                    const std::string verb =
                        ToLower(split == std::string::npos ? command : command.substr(0, split));
                    const std::string argument =
                        Trim(split == std::string::npos ? std::string{} : command.substr(split + 1));
                    const std::string normalized_argument = ToLower(argument);

                    if (verb == "look")
                    {
                        SendLookRequest();
                    }
                    else if (verb == "move")
                    {
                        if (normalized_argument == "forward" || normalized_argument == "f")
                        {
                            SendStepRequest(mud::v1::STEP_KIND_MOVE_FORWARD);
                        }
                        else if (normalized_argument == "backward" || normalized_argument == "back" ||
                                 normalized_argument == "b")
                        {
                            SendStepRequest(mud::v1::STEP_KIND_MOVE_BACKWARD);
                        }
                        else
                        {
                            ui_.AddLog("[usage] /move <forward|backward>");
                        }
                    }
                    else if (verb == "turn")
                    {
                        if (normalized_argument == "left" || normalized_argument == "l")
                        {
                            SendStepRequest(mud::v1::STEP_KIND_TURN_LEFT);
                        }
                        else if (normalized_argument == "right" || normalized_argument == "r")
                        {
                            SendStepRequest(mud::v1::STEP_KIND_TURN_RIGHT);
                        }
                        else
                        {
                            ui_.AddLog("[usage] /turn <left|right>");
                        }
                    }
                    else if (verb == "say")
                    {
                        if (argument.empty())
                        {
                            ui_.AddLog("[usage] /say <text>");
                        }
                        else
                        {
                            SendSayRequest(argument);
                        }
                    }
                    else if (verb == "guard")
                    {
                        SendGuardRequest();
                    }
                    else if (verb == "attack")
                    {
                        if (normalized_argument.empty() || normalized_argument == "melee" ||
                            normalized_argument == "m")
                        {
                            SendAttackRequest(mud::v1::WEAPON_KIND_MELEE);
                        }
                        else if (normalized_argument == "ranged" || normalized_argument == "range" ||
                                 normalized_argument == "r")
                        {
                            SendAttackRequest(mud::v1::WEAPON_KIND_RANGED);
                        }
                        else
                        {
                            ui_.AddLog("[usage] /attack <melee|ranged>");
                        }
                    }
                    else if (verb == "ping")
                    {
                        if (!session_.SendPing())
                        {
                            ui_.AddLog("[error] Failed to send ping.");
                            running_.store(false, std::memory_order_relaxed);
                        }
                    }
                    else
                    {
                        ui_.AddLog("[usage] Unknown command. Try /look, /move, /turn, /say, /guard, /attack, /ping.");
                    }
                }
            }
            else
            {
                SendSayRequest(text);
            }
        }
        ui_.ClearInput();
        ui_.SetMode(UiMode::kMove);
        ui_.Render();
        return;
    }
    if (ch == '\b' || ch == 127)
    {
        ui_.BackspaceInput();
        ui_.Render();
        return;
    }

    if (IsPrintableChar(ch))
    {
        ui_.AppendInputChar(static_cast<char>(ch));
        ui_.Render();
    }
}

bool ClientApp::TrySendMoveOrTurnCommand(mud::v1::ClientMessage message)
{
    const std::uint64_t current_tick = latest_tick_id_.load(std::memory_order_relaxed);
    if (move_command_sent_this_tick_ && current_tick == last_move_command_tick_id_)
    {
        ui_.AddLog("[move] One move/turn per tick. Wait for next tick.");
        return false;
    }

    if (!SendMessage(std::move(message)))
    {
        return false;
    }

    move_command_sent_this_tick_ = true;
    last_move_command_tick_id_ = current_tick;
    return true;
}
} // namespace grpcmud::client
