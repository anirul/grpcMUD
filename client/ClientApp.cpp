#include "ClientApp.hpp"

#include <conio.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <thread>
#include <utility>

namespace grpcmud::client
{
int ClientApp::Run(int argc, char** argv)
{
    server_address_ = (argc > 1) ? argv[1] : "localhost:50051";
    if (argc > 2)
    {
        player_name_ = Trim(argv[2]);
    }
    else
    {
        player_name_ = ReadPlayerNameFromPrompt();
    }

    if (player_name_.empty())
    {
        std::cerr << "Player name cannot be empty." << std::endl;
        return 1;
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
    ui_.AddLog("Move mode: W/S=forward/backward, A/D=turn, 1=melee, 2=ranged, 3=guard.");
    ui_.AddLog("Press Enter to chat. Prefix with '/' to send a command.");
    ui_.Render();

    while (running_.load(std::memory_order_relaxed))
    {
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

bool ClientApp::SendCommandText(const std::string& text)
{
    const std::string trimmed = Trim(text);
    if (trimmed.empty())
    {
        return false;
    }

    const std::string request_id = "req-" + std::to_string(request_counter_++);
    if (!session_.SendCommand(request_id, trimmed))
    {
        ui_.AddLog("[error] Failed to send command.");
        running_.store(false, std::memory_order_relaxed);
        return false;
    }

    return true;
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
        ui_.AddLog("[ack] " + message.command_ack().request_id() + " accepted=" +
                   (message.command_ack().accepted() ? "true" : "false") + " \"" +
                   message.command_ack().message() + "\"");
        if (!message.command_ack().accepted())
        {
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
        SendCommandText("attack melee");
        ui_.Render();
        return;
    }
    if (key == '2')
    {
        SendCommandText("attack ranged");
        ui_.Render();
        return;
    }
    if (key == '3')
    {
        SendCommandText("guard");
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
    if (key == '/')
    {
        ui_.SetMode(UiMode::kCustomCommand);
        ui_.SetInputBuffer("/");
        ui_.Render();
        return;
    }
    if (key == 'w' || key == 'k')
    {
        TrySendMoveOrTurnCommand("move forward");
        ui_.Render();
        return;
    }
    if (key == 'a' || key == 'h')
    {
        TrySendMoveOrTurnCommand("turn left");
        ui_.Render();
        return;
    }
    if (key == 's' || key == 'j')
    {
        TrySendMoveOrTurnCommand("move backward");
        ui_.Render();
        return;
    }
    if (key == 'd' || key == 'l')
    {
        TrySendMoveOrTurnCommand("turn right");
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
                SendCommandText(Trim(text.substr(1)));
            }
            else
            {
                SendCommandText("say " + text);
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

bool ClientApp::TrySendMoveOrTurnCommand(const std::string& command_text)
{
    const std::uint64_t current_tick = latest_tick_id_.load(std::memory_order_relaxed);
    if (move_command_sent_this_tick_ && current_tick == last_move_command_tick_id_)
    {
        ui_.AddLog("[move] One move/turn per tick. Wait for next tick.");
        return false;
    }

    if (!SendCommandText(command_text))
    {
        return false;
    }

    move_command_sent_this_tick_ = true;
    last_move_command_tick_id_ = current_tick;
    return true;
}
} // namespace grpcmud::client
