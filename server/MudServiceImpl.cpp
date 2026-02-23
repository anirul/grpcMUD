#include "MudServiceImpl.hpp"

#include <chrono>
#include <iostream>
#include <limits>
#include <thread>
#include <utility>

namespace grpcmud::server
{
namespace
{
int HexDigitValue(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F')
    {
        return 10 + (c - 'A');
    }
    return -1;
}

std::string PercentDecode(const std::string& text)
{
    std::string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '%' && (i + 2) < text.size())
        {
            const int high = HexDigitValue(text[i + 1]);
            const int low = HexDigitValue(text[i + 2]);
            if (high >= 0 && low >= 0)
            {
                out.push_back(static_cast<char>((high * 16) + low));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

mud::v1::Direction ToProtoDirection(FacingDirection direction)
{
    switch (direction)
    {
    case FacingDirection::kNorth:
        return mud::v1::DIRECTION_NORTH;
    case FacingDirection::kEast:
        return mud::v1::DIRECTION_EAST;
    case FacingDirection::kSouth:
        return mud::v1::DIRECTION_SOUTH;
    case FacingDirection::kWest:
        return mud::v1::DIRECTION_WEST;
    }
    return mud::v1::DIRECTION_UNSPECIFIED;
}

mud::v1::VisibleActor::Kind ToProtoActorKind(ViewActorKind kind)
{
    switch (kind)
    {
    case ViewActorKind::kSelf:
        return mud::v1::VisibleActor::KIND_SELF;
    case ViewActorKind::kPlayer:
        return mud::v1::VisibleActor::KIND_PLAYER;
    case ViewActorKind::kNpc:
        return mud::v1::VisibleActor::KIND_NPC;
    }
    return mud::v1::VisibleActor::KIND_UNSPECIFIED;
}

mud::v1::SquareKind ToProtoSquareKind(SquareKind kind)
{
    switch (kind)
    {
    case SquareKind::kFloor:
        return mud::v1::SQUARE_KIND_FLOOR;
    case SquareKind::kWall:
        return mud::v1::SQUARE_KIND_WALL;
    }
    return mud::v1::SQUARE_KIND_UNSPECIFIED;
}
} // namespace

MudServiceImpl::MudServiceImpl(const std::string& map_db_path,
                               std::chrono::seconds autosave_interval,
                               std::chrono::milliseconds tick_interval)
    : world_(map_db_path),
      autosave_interval_(autosave_interval),
      tick_interval_(tick_interval)
{
    StartAutosaveTask();
}

MudServiceImpl::~MudServiceImpl()
{
    StopAutosaveTask();
}

grpc::Status MudServiceImpl::Play(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<mud::v1::ServerMessage, mud::v1::ClientMessage>* stream)
{
    const std::string peer_address =
        context ? NormalizePeerAddress(context->peer()) : "unknown-peer";
    LogConnectionAdded(peer_address);

    auto session = std::make_shared<ClientSession>(stream);
    std::atomic<bool> done{false};
    bool joined = false;
    std::uint64_t last_gameplay_command_tick = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t next_ranged_attack_ready_tick = 0;
    PlayerSnapshot player;

    std::thread ticker([&]()
    {
        while (!done.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(tick_interval_);
            if (done.load(std::memory_order_relaxed))
            {
                break;
            }

            const std::uint64_t tick_id = tick_counter_.fetch_add(1) + 1;

            if (joined)
            {
                const auto respawned = world_.RespawnPlayerIfReady(player.player_id);
                if (respawned)
                {
                    player = *respawned;
                    if (!SendWorldEvent(session, tick_id, mud::v1::WorldEvent::KIND_SYSTEM,
                                        "You respawn and return to battle.", player.square_id,
                                        player.player_id))
                    {
                        done.store(true, std::memory_order_relaxed);
                        break;
                    }

                    if (!SendWorldEvent(session, tick_id, mud::v1::WorldEvent::KIND_ROOM,
                                        world_.DescribeSquareForPlayer(player.player_id),
                                        player.square_id, player.player_id))
                    {
                        done.store(true, std::memory_order_relaxed);
                        break;
                    }

                    if (!SendViewUpdate(session, tick_id, player.player_id))
                    {
                        done.store(true, std::memory_order_relaxed);
                        break;
                    }
                }
            }

            mud::v1::ServerMessage out = MakeBaseMessage(tick_id);
            auto* tick = out.mutable_tick();
            tick->set_tick_id(tick_id);
            tick->set_server_time_ms(out.server_time_ms());

            if (!session->Write(std::move(out)))
            {
                done.store(true, std::memory_order_relaxed);
                break;
            }
        }
    });

    mud::v1::ClientMessage in;
    while (!done.load(std::memory_order_relaxed) && stream->Read(&in))
    {
        const std::uint64_t current_tick = tick_counter_.load();
        const auto prepare_action = [&](const std::string& request_id) -> bool
        {
            if (!joined)
            {
                if (!SendAck(session, current_tick, request_id, false,
                             "Join first before sending actions."))
                {
                    done.store(true, std::memory_order_relaxed);
                }
                return false;
            }

            const auto respawned = world_.RespawnPlayerIfReady(player.player_id);
            if (respawned)
            {
                player = *respawned;
                if (!SendWorldEvent(session, current_tick, mud::v1::WorldEvent::KIND_SYSTEM,
                                    "You respawn and return to battle.", player.square_id,
                                    player.player_id))
                {
                    done.store(true, std::memory_order_relaxed);
                    return false;
                }

                if (!SendWorldEvent(session, current_tick, mud::v1::WorldEvent::KIND_ROOM,
                                    world_.DescribeSquareForPlayer(player.player_id),
                                    player.square_id, player.player_id))
                {
                    done.store(true, std::memory_order_relaxed);
                    return false;
                }

                if (!SendViewUpdate(session, current_tick, player.player_id))
                {
                    done.store(true, std::memory_order_relaxed);
                    return false;
                }
            }

            const auto respawn_seconds = world_.GetRespawnSecondsRemaining(player.player_id);
            if (respawn_seconds && *respawn_seconds > 0)
            {
                if (!SendAck(session, current_tick, request_id, false,
                             "You are knocked out. Respawn in " +
                                 std::to_string(*respawn_seconds) + " second(s)."))
                {
                    done.store(true, std::memory_order_relaxed);
                }
                return false;
            }
            return true;
        };
        const auto reserve_tick_for_gameplay_command = [&](const std::string& request_id) -> bool
        {
            if (current_tick == last_gameplay_command_tick)
            {
                if (!SendAck(session, current_tick, request_id, false,
                             "Only one command is allowed per tick."))
                {
                    done.store(true, std::memory_order_relaxed);
                }
                return false;
            }

            last_gameplay_command_tick = current_tick;
            return true;
        };

        switch (in.payload_case())
        {
        case mud::v1::ClientMessage::kJoin:
        {
            if (joined)
            {
                if (!SendError(session, current_tick, "ALREADY_JOINED",
                               "JoinRequest ignored because this stream is already joined."))
                {
                    done.store(true, std::memory_order_relaxed);
                }
                break;
            }

            const std::string player_name = Trim(in.join().player_name());
            if (player_name.empty())
            {
                if (!SendError(session, current_tick, "INVALID_PLAYER_NAME",
                               "JoinRequest requires a non-empty player_name."))
                {
                    done.store(true, std::memory_order_relaxed);
                }
                break;
            }

            player = world_.AddPlayer(player_name);
            joined = true;
            RegisterSession(player.player_id, session);

            mud::v1::ServerMessage join_ack = MakeBaseMessage(current_tick);
            auto* ack = join_ack.mutable_join_ack();
            ack->set_player_id(player.player_id);
            ack->set_motd("Welcome to Quest Board PvP. Actions: look, step(move/turn), "
                          "guard, say <text>, attack <melee|ranged>, ping. "
                          "Only one gameplay command per tick is allowed.");
            if (!session->Write(std::move(join_ack)))
            {
                done.store(true, std::memory_order_relaxed);
                break;
            }

            if (!SendWorldEvent(session, current_tick, mud::v1::WorldEvent::KIND_SYSTEM,
                                "You joined as '" + player.player_name + "'.", player.square_id,
                                player.player_id))
            {
                done.store(true, std::memory_order_relaxed);
                break;
            }

            if (!SendWorldEvent(session, current_tick, mud::v1::WorldEvent::KIND_ROOM,
                                world_.DescribeSquareForPlayer(player.player_id), player.square_id,
                                player.player_id))
            {
                done.store(true, std::memory_order_relaxed);
                break;
            }

            if (!SendViewUpdate(session, current_tick, player.player_id))
            {
                done.store(true, std::memory_order_relaxed);
            }
            break;
        }
        case mud::v1::ClientMessage::kLook:
        {
            const std::string request_id =
                in.look().request_id().empty() ? "missing-request-id" : in.look().request_id();
            if (!prepare_action(request_id))
            {
                break;
            }
            if (!reserve_tick_for_gameplay_command(request_id))
            {
                break;
            }

            if (!SendAck(session, current_tick, request_id, true, "ok"))
            {
                done.store(true, std::memory_order_relaxed);
                break;
            }

            const auto fresh_player = world_.GetPlayer(player.player_id);
            const std::string square_id = fresh_player ? fresh_player->square_id : std::string{};
            if (!SendWorldEvent(session, current_tick, mud::v1::WorldEvent::KIND_ROOM,
                                world_.DescribeSquareForPlayer(player.player_id), square_id,
                                player.player_id))
            {
                done.store(true, std::memory_order_relaxed);
                break;
            }

            if (!SendViewUpdate(session, current_tick, player.player_id))
            {
                done.store(true, std::memory_order_relaxed);
            }
            break;
        }
        case mud::v1::ClientMessage::kStep:
        {
            const auto& step = in.step();
            const std::string request_id =
                step.request_id().empty() ? "missing-request-id" : step.request_id();
            if (!prepare_action(request_id))
            {
                break;
            }
            if (!reserve_tick_for_gameplay_command(request_id))
            {
                break;
            }

            if (step.kind() == mud::v1::STEP_KIND_MOVE_FORWARD ||
                step.kind() == mud::v1::STEP_KIND_MOVE_BACKWARD)
            {
                const std::string move_arg =
                    (step.kind() == mud::v1::STEP_KIND_MOVE_FORWARD) ? "forward" : "backward";
                const MoveResult move = world_.MovePlayer(player.player_id, move_arg);
                if (!move.success)
                {
                    if (!SendAck(session, current_tick, request_id, false, move.message))
                    {
                        done.store(true, std::memory_order_relaxed);
                    }
                    break;
                }

                if (!SendAck(session, current_tick, request_id, true, "ok"))
                {
                    done.store(true, std::memory_order_relaxed);
                    break;
                }

                if (!SendWorldEvent(session, current_tick, mud::v1::WorldEvent::KIND_ROOM,
                                    world_.DescribeSquareForPlayer(player.player_id), move.square_id,
                                    player.player_id))
                {
                    done.store(true, std::memory_order_relaxed);
                    break;
                }

                if (!SendViewUpdate(session, current_tick, player.player_id))
                {
                    done.store(true, std::memory_order_relaxed);
                }
                break;
            }

            if (step.kind() == mud::v1::STEP_KIND_TURN_LEFT ||
                step.kind() == mud::v1::STEP_KIND_TURN_RIGHT)
            {
                const std::string turn_arg =
                    (step.kind() == mud::v1::STEP_KIND_TURN_LEFT) ? "left" : "right";
                const TurnResult turn = world_.TurnPlayer(player.player_id, turn_arg);
                if (!turn.success)
                {
                    if (!SendAck(session, current_tick, request_id, false, turn.message))
                    {
                        done.store(true, std::memory_order_relaxed);
                    }
                    break;
                }

                if (!SendAck(session, current_tick, request_id, true, "ok"))
                {
                    done.store(true, std::memory_order_relaxed);
                    break;
                }

                const auto fresh_player = world_.GetPlayer(player.player_id);
                const std::string square_id = fresh_player ? fresh_player->square_id : std::string{};
                if (!SendWorldEvent(session, current_tick, mud::v1::WorldEvent::KIND_SYSTEM,
                                    "You now face " + FacingDirectionToString(turn.facing) + ".",
                                    square_id, player.player_id))
                {
                    done.store(true, std::memory_order_relaxed);
                    break;
                }

                if (!SendViewUpdate(session, current_tick, player.player_id))
                {
                    done.store(true, std::memory_order_relaxed);
                }
                break;
            }

            if (!SendAck(session, current_tick, request_id, false, "Invalid step kind."))
            {
                done.store(true, std::memory_order_relaxed);
            }
            break;
        }
        case mud::v1::ClientMessage::kSay:
        {
            const auto& say = in.say();
            const std::string request_id =
                say.request_id().empty() ? "missing-request-id" : say.request_id();
            if (!prepare_action(request_id))
            {
                break;
            }
            if (!reserve_tick_for_gameplay_command(request_id))
            {
                break;
            }

            const std::string text = Trim(say.text());
            if (text.empty())
            {
                if (!SendAck(session, current_tick, request_id, false, "Usage: say <text>"))
                {
                    done.store(true, std::memory_order_relaxed);
                }
                break;
            }

            if (!SendAck(session, current_tick, request_id, true, "ok"))
            {
                done.store(true, std::memory_order_relaxed);
                break;
            }

            const auto speaker = world_.GetPlayer(player.player_id);
            if (speaker)
            {
                BroadcastSay(*speaker, text, current_tick);
            }
            break;
        }
        case mud::v1::ClientMessage::kGuard:
        {
            const auto& guard_req = in.guard();
            const std::string request_id =
                guard_req.request_id().empty() ? "missing-request-id" : guard_req.request_id();
            if (!prepare_action(request_id))
            {
                break;
            }
            if (!reserve_tick_for_gameplay_command(request_id))
            {
                break;
            }

            const GuardResult guard = world_.GuardPlayer(player.player_id);
            if (!guard.success)
            {
                if (!SendAck(session, current_tick, request_id, false, guard.message))
                {
                    done.store(true, std::memory_order_relaxed);
                }
                break;
            }

            if (!SendAck(session, current_tick, request_id, true, "ok"))
            {
                done.store(true, std::memory_order_relaxed);
                break;
            }

            const auto fresh_player = world_.GetPlayer(player.player_id);
            const std::string square_id = fresh_player ? fresh_player->square_id : std::string{};
            if (!SendWorldEvent(session, current_tick, mud::v1::WorldEvent::KIND_SYSTEM,
                                "You raise your guard toward " +
                                    FacingDirectionToString(guard.facing) + ".",
                                square_id, player.player_id))
            {
                done.store(true, std::memory_order_relaxed);
                break;
            }

            if (!SendViewUpdate(session, current_tick, player.player_id))
            {
                done.store(true, std::memory_order_relaxed);
            }
            break;
        }
        case mud::v1::ClientMessage::kAttack:
        {
            const auto& attack_req = in.attack();
            const std::string request_id =
                attack_req.request_id().empty() ? "missing-request-id" : attack_req.request_id();
            if (!prepare_action(request_id))
            {
                break;
            }
            if (!reserve_tick_for_gameplay_command(request_id))
            {
                break;
            }

            WeaponType weapon = WeaponType::kMelee;
            if (attack_req.weapon() == mud::v1::WEAPON_KIND_MELEE)
            {
                weapon = WeaponType::kMelee;
            }
            else if (attack_req.weapon() == mud::v1::WEAPON_KIND_RANGED)
            {
                weapon = WeaponType::kRanged;
            }
            else
            {
                if (!SendAck(session, current_tick, request_id, false,
                             "Attack weapon is required (melee or ranged)."))
                {
                    done.store(true, std::memory_order_relaxed);
                }
                break;
            }

            if (weapon == WeaponType::kRanged)
            {
                if (current_tick < next_ranged_attack_ready_tick)
                {
                    const std::uint64_t ticks_remaining = next_ranged_attack_ready_tick - current_tick;
                    if (!SendAck(session, current_tick, request_id, false,
                                 "Ranged attack reloading. Ready in " +
                                     std::to_string(ticks_remaining) + " tick(s)."))
                    {
                        done.store(true, std::memory_order_relaxed);
                    }
                    break;
                }
                next_ranged_attack_ready_tick = current_tick + 2;
            }

            const AttackResult attack = world_.AttackInFacing(player.player_id, weapon);
            if (!attack.success)
            {
                if (!SendAck(session, current_tick, request_id, false, attack.message))
                {
                    done.store(true, std::memory_order_relaxed);
                }
                break;
            }

            if (!SendAck(session, current_tick, request_id, true, "ok"))
            {
                done.store(true, std::memory_order_relaxed);
                break;
            }

            const std::string attacker_message =
                attack.attacker_message.empty() ? attack.message : attack.attacker_message;
            if (!SendWorldEvent(session, current_tick, mud::v1::WorldEvent::KIND_COMBAT,
                                attacker_message, attack.attacker_square_id,
                                attack.attacker_id))
            {
                done.store(true, std::memory_order_relaxed);
                break;
            }

            if (!SendViewUpdate(session, current_tick, player.player_id))
            {
                done.store(true, std::memory_order_relaxed);
                break;
            }

            if (attack.target_is_player && attack.target_id != player.player_id &&
                !attack.target_message.empty())
            {
                const auto target_session = GetSession(attack.target_id);
                if (target_session)
                {
                    SendWorldEvent(target_session, current_tick, mud::v1::WorldEvent::KIND_COMBAT,
                                   attack.target_message, attack.target_square_id,
                                   attack.attacker_id);
                    SendViewUpdate(target_session, current_tick, attack.target_id);
                }
            }
            break;
        }
        case mud::v1::ClientMessage::kPing:
        {
            mud::v1::ServerMessage out = MakeBaseMessage(current_tick);
            out.mutable_pong();
            if (!session->Write(std::move(out)))
            {
                done.store(true, std::memory_order_relaxed);
            }
            break;
        }
        case mud::v1::ClientMessage::PAYLOAD_NOT_SET:
        default:
            if (!SendError(session, current_tick, "BAD_MESSAGE", "ClientMessage has no payload."))
            {
                done.store(true, std::memory_order_relaxed);
            }
            break;
        }
    }

    done.store(true, std::memory_order_relaxed);
    if (ticker.joinable())
    {
        ticker.join();
    }

    if (joined)
    {
        UnregisterSession(player.player_id);
        world_.RemovePlayer(player.player_id);
    }

    LogConnectionRemoved(peer_address);
    return grpc::Status::OK;
}

std::uint64_t MudServiceImpl::NowMs()
{
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return static_cast<std::uint64_t>(ms.count());
}

std::string MudServiceImpl::Trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, (last - first) + 1);
}

std::string MudServiceImpl::NormalizePeerAddress(const std::string& peer)
{
    std::string normalized = peer;
    if (peer.rfind("ipv4:", 0) == 0)
    {
        normalized = peer.substr(5);
    }
    else if (peer.rfind("ipv6:", 0) == 0)
    {
        normalized = peer.substr(5);
    }
    else if (peer.rfind("dns:", 0) == 0)
    {
        normalized = peer.substr(4);
    }

    normalized = PercentDecode(normalized);
    return normalized.empty() ? "unknown-peer" : normalized;
}

mud::v1::ServerMessage MudServiceImpl::MakeBaseMessage(std::uint64_t tick_id) const
{
    mud::v1::ServerMessage msg;
    msg.set_server_time_ms(NowMs());
    msg.set_tick_id(tick_id);
    return msg;
}

bool MudServiceImpl::SendError(const std::shared_ptr<ClientSession>& session, std::uint64_t tick_id,
                               const std::string& code, const std::string& message) const
{
    mud::v1::ServerMessage out = MakeBaseMessage(tick_id);
    auto* error = out.mutable_error();
    error->set_code(code);
    error->set_message(message);
    return session->Write(std::move(out));
}

bool MudServiceImpl::SendAck(const std::shared_ptr<ClientSession>& session, std::uint64_t tick_id,
                             const std::string& request_id, bool accepted,
                             const std::string& message) const
{
    mud::v1::ServerMessage out = MakeBaseMessage(tick_id);
    auto* ack = out.mutable_command_ack();
    ack->set_request_id(request_id);
    ack->set_accepted(accepted);
    ack->set_message(message);
    return session->Write(std::move(out));
}

bool MudServiceImpl::SendWorldEvent(const std::shared_ptr<ClientSession>& session,
                                    std::uint64_t tick_id, mud::v1::WorldEvent::Kind kind,
                                    const std::string& text, const std::string& room_id,
                                    const std::string& actor_id) const
{
    mud::v1::ServerMessage out = MakeBaseMessage(tick_id);
    auto* event = out.mutable_world_event();
    event->set_kind(kind);
    event->set_text(text);
    if (!room_id.empty())
    {
        event->set_room_id(room_id);
    }
    if (!actor_id.empty())
    {
        event->set_actor_id(actor_id);
    }
    return session->Write(std::move(out));
}

bool MudServiceImpl::SendViewUpdate(const std::shared_ptr<ClientSession>& session,
                                    std::uint64_t tick_id, const std::string& player_id) const
{
    const auto view = world_.BuildLocalView(player_id, view_front_squares_, view_back_squares_,
                                            view_side_squares_);
    if (!view)
    {
        return false;
    }
    const auto first_person = world_.BuildFirstPersonView(player_id, first_person_depth_squares_);

    mud::v1::ServerMessage out = MakeBaseMessage(tick_id);
    auto* view_out = out.mutable_view();
    view_out->set_center_square_id(view->center_square_id);
    view_out->set_center_x(view->center_x);
    view_out->set_center_y(view->center_y);
    view_out->set_facing(ToProtoDirection(view->facing));
    view_out->set_radius(view->radius);

    for (const auto& square : view->squares)
    {
        auto* square_out = view_out->add_squares();
        square_out->set_square_id(square.square_id);
        square_out->set_x(square.x);
        square_out->set_y(square.y);
        square_out->set_open_north(square.open_north);
        square_out->set_open_east(square.open_east);
        square_out->set_open_south(square.open_south);
        square_out->set_open_west(square.open_west);
        square_out->set_kind(ToProtoSquareKind(square.kind));
    }

    for (const auto& actor : view->actors)
    {
        auto* actor_out = view_out->add_actors();
        actor_out->set_actor_id(actor.actor_id);
        actor_out->set_name(actor.name);
        actor_out->set_x(actor.x);
        actor_out->set_y(actor.y);
        actor_out->set_kind(ToProtoActorKind(actor.kind));
        actor_out->set_facing(ToProtoDirection(actor.facing));
    }

    if (first_person)
    {
        auto* fp_out = view_out->mutable_first_person();
        fp_out->set_facing(ToProtoDirection(first_person->facing));
        fp_out->set_max_depth(first_person->max_depth);

        for (const auto& cell : first_person->cells)
        {
            auto* cell_out = fp_out->add_cells();
            cell_out->set_depth(cell.depth);
            cell_out->set_lane(cell.lane);
            cell_out->set_visible(cell.visible);
            cell_out->set_open_forward(cell.open_forward);
            cell_out->set_open_left(cell.open_left);
            cell_out->set_open_right(cell.open_right);
            if (cell.has_actor)
            {
                cell_out->set_actor_kind(ToProtoActorKind(cell.actor_kind));
                cell_out->set_actor_facing(ToProtoDirection(cell.actor_facing));
                cell_out->set_actor_name(cell.actor_name);
            }
        }
    }

    return session->Write(std::move(out));
}

void MudServiceImpl::RegisterSession(const std::string& player_id,
                                     const std::shared_ptr<ClientSession>& session)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_[player_id] = session;
}

void MudServiceImpl::UnregisterSession(const std::string& player_id)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(player_id);
}

std::vector<std::pair<NearbyPlayer, std::shared_ptr<ClientSession>>>
MudServiceImpl::ResolveRecipients(const std::vector<NearbyPlayer>& nearby)
{
    std::vector<std::pair<NearbyPlayer, std::shared_ptr<ClientSession>>> recipients;
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    for (const NearbyPlayer& candidate : nearby)
    {
        auto it = sessions_.find(candidate.player.player_id);
        if (it == sessions_.end())
        {
            continue;
        }

        auto session = it->second.lock();
        if (!session)
        {
            sessions_.erase(it);
            continue;
        }

        recipients.push_back({candidate, std::move(session)});
    }

    return recipients;
}

std::shared_ptr<ClientSession> MudServiceImpl::GetSession(const std::string& player_id)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    auto it = sessions_.find(player_id);
    if (it == sessions_.end())
    {
        return nullptr;
    }

    auto session = it->second.lock();
    if (!session)
    {
        sessions_.erase(it);
    }
    return session;
}

void MudServiceImpl::BroadcastSay(const PlayerSnapshot& speaker, const std::string& text,
                                  std::uint64_t tick_id)
{
    const std::vector<NearbyPlayer> nearby =
        world_.GetPlayersWithinRange(speaker.square_id, say_range_squares_);
    const auto recipients = ResolveRecipients(nearby);

    for (const auto& [target, session] : recipients)
    {
        std::string message;
        if (target.player.player_id == speaker.player_id)
        {
            message = "You say: " + text;
        }
        else if (target.distance == 0)
        {
            message = speaker.player_name + " says: " + text;
        }
        else if (target.distance == 1)
        {
            message = "You hear " + speaker.player_name + " from an adjacent square: " + text;
        }
        else
        {
            message = "You hear " + speaker.player_name + " " +
                      std::to_string(target.distance) + " squares away: " + text;
        }

        SendWorldEvent(session, tick_id, mud::v1::WorldEvent::KIND_CHAT, message, speaker.square_id,
                       speaker.player_id);
    }
}

void MudServiceImpl::LogConnectionAdded(const std::string& peer_address)
{
    std::size_t connected_clients = 0;
    {
        std::lock_guard<std::mutex> lock(connection_count_mutex_);
        connected_clients = ++connected_clients_;
    }
    std::cout << connected_clients << "# added " << peer_address << " to the MUD." << std::endl;
}

void MudServiceImpl::LogConnectionRemoved(const std::string& peer_address)
{
    std::size_t connected_clients = 0;
    {
        std::lock_guard<std::mutex> lock(connection_count_mutex_);
        if (connected_clients_ > 0)
        {
            --connected_clients_;
        }
        connected_clients = connected_clients_;
    }
    std::cout << connected_clients << "# removed " << peer_address << " from the MUD."
              << std::endl;
}

void MudServiceImpl::StartAutosaveTask()
{
    autosave_thread_ = std::thread([this]()
    {
        std::unique_lock<std::mutex> lock(autosave_mutex_);

        while (!autosave_stop_.load(std::memory_order_relaxed))
        {
            const bool stop_requested = autosave_cv_.wait_for(
                lock, autosave_interval_, [this]()
                {
                    return autosave_stop_.load(std::memory_order_relaxed);
                });

            if (stop_requested || autosave_stop_.load(std::memory_order_relaxed))
            {
                break;
            }

            lock.unlock();
            const bool saved = world_.SaveMapToDisk();
            if (!saved)
            {
                std::cerr << "autosave failed" << std::endl;
            }
            lock.lock();
        }
    });
}

void MudServiceImpl::StopAutosaveTask()
{
    autosave_stop_.store(true, std::memory_order_relaxed);
    autosave_cv_.notify_all();

    if (autosave_thread_.joinable())
    {
        autosave_thread_.join();
    }

    if (!world_.SaveMapToDisk())
    {
        std::cerr << "final map save failed" << std::endl;
    }
}
} // namespace grpcmud::server
