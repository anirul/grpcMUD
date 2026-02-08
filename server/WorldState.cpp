#include "WorldState.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <queue>
#include <sstream>

#include "mud.pb.h"

namespace grpcmud::server
{
namespace
{
constexpr int kDefaultMapWidth = 6;
constexpr int kDefaultMapHeight = 6;
constexpr int kPlayerMaxHp = 5;
constexpr int kNpcMaxHp = 4;
constexpr int kMeleeRange = 1;
constexpr int kRangedRange = 3;
constexpr int kMeleeDamage = 2;
constexpr int kRangedDamage = 1;
constexpr int kPlayerRespawnDelaySeconds = 10;

FacingDirection RotateLeft(FacingDirection direction)
{
    switch (direction)
    {
    case FacingDirection::kNorth:
        return FacingDirection::kWest;
    case FacingDirection::kWest:
        return FacingDirection::kSouth;
    case FacingDirection::kSouth:
        return FacingDirection::kEast;
    case FacingDirection::kEast:
        return FacingDirection::kNorth;
    }
    return FacingDirection::kNorth;
}

FacingDirection RotateRight(FacingDirection direction)
{
    switch (direction)
    {
    case FacingDirection::kNorth:
        return FacingDirection::kEast;
    case FacingDirection::kEast:
        return FacingDirection::kSouth;
    case FacingDirection::kSouth:
        return FacingDirection::kWest;
    case FacingDirection::kWest:
        return FacingDirection::kNorth;
    }
    return FacingDirection::kNorth;
}
}

std::string FacingDirectionToString(FacingDirection direction)
{
    switch (direction)
    {
    case FacingDirection::kNorth:
        return "north";
    case FacingDirection::kEast:
        return "east";
    case FacingDirection::kSouth:
        return "south";
    case FacingDirection::kWest:
        return "west";
    }
    return "unknown";
}

WorldState::WorldState(std::string map_db_path)
    : map_db_path_(std::move(map_db_path)), rng_(std::random_device{}())
{
    if (!LoadMapDataFromDisk())
    {
        CreateDefaultMapData();
        SaveMapDataToDisk();
    }

    BuildCoordinateIndex();
    SpawnDefaultNpcs();
}

PlayerSnapshot WorldState::AddPlayer(const std::string& requested_name)
{
    std::lock_guard<std::mutex> lock(mutex_);

    Player player;
    player.id = "player-" + std::to_string(next_player_id_++);
    player.name = requested_name;
    player.square_id = RandomFreeSquareId();
    if (player.square_id.empty() && !squares_.empty())
    {
        player.square_id = squares_.begin()->first;
    }
    player.facing = FacingDirection::kNorth;
    player.alive = true;
    player.respawn_ready_at = std::chrono::steady_clock::time_point::min();
    player.hp = kPlayerMaxHp;

    players_[player.id] = player;
    return ToPlayerSnapshot(player);
}

void WorldState::RemovePlayer(const std::string& player_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    players_.erase(player_id);
}

std::optional<PlayerSnapshot> WorldState::GetPlayer(const std::string& player_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = players_.find(player_id);
    if (it == players_.end())
    {
        return std::nullopt;
    }
    return ToPlayerSnapshot(it->second);
}

MoveResult WorldState::MovePlayer(const std::string& player_id, const std::string& direction)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto player_it = players_.find(player_id);
    if (player_it == players_.end())
    {
        return MoveResult{false, "Player not found.", {}};
    }
    if (!player_it->second.alive)
    {
        return MoveResult{
            false,
            "You are knocked out. Respawn in " +
                std::to_string(RespawnSecondsRemaining(player_it->second)) + " second(s).",
            {}};
    }

    const std::string normalized_direction = ToLower(Trim(direction));
    FacingDirection move_direction = FacingDirection::kNorth;
    bool keep_current_facing = false;
    if (normalized_direction == "forward" || normalized_direction == "f")
    {
        move_direction = player_it->second.facing;
        keep_current_facing = true;
    }
    else if (normalized_direction == "backward" || normalized_direction == "back" ||
             normalized_direction == "b")
    {
        move_direction = OppositeDirection(player_it->second.facing);
        keep_current_facing = true;
    }
    else
    {
        const auto parsed_direction = ParseDirection(normalized_direction);
        if (!parsed_direction)
        {
            return MoveResult{
                false, "Direction is required. Try forward, backward, north, south, east, or west.",
                player_it->second.square_id};
        }
        move_direction = *parsed_direction;
    }

    auto square_it = squares_.find(player_it->second.square_id);
    if (square_it == squares_.end())
    {
        return MoveResult{false, "Current square is invalid.", {}};
    }

    const auto next_square_id = NeighborSquareId(square_it->second, move_direction);
    if (!next_square_id)
    {
        return MoveResult{
            false, "No open edge in direction '" + FacingDirectionToString(move_direction) + "'.",
            player_it->second.square_id};
    }

    if (IsSquareOccupied(*next_square_id, player_id))
    {
        return MoveResult{false, "Square '" + *next_square_id + "' is occupied.", player_it->second.square_id};
    }

    player_it->second.square_id = *next_square_id;
    if (!keep_current_facing)
    {
        player_it->second.facing = move_direction;
    }
    player_it->second.guarding = false;
    return MoveResult{true, "ok", player_it->second.square_id};
}

TurnResult WorldState::TurnPlayer(const std::string& player_id, const std::string& direction)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto player_it = players_.find(player_id);
    if (player_it == players_.end())
    {
        return TurnResult{false, "Player not found.", FacingDirection::kNorth};
    }
    if (!player_it->second.alive)
    {
        return TurnResult{
            false,
            "You are knocked out. Respawn in " +
                std::to_string(RespawnSecondsRemaining(player_it->second)) + " second(s).",
            FacingDirection::kNorth};
    }

    const std::string normalized_direction = ToLower(Trim(direction));
    if (normalized_direction == "left" || normalized_direction == "l")
    {
        player_it->second.facing = RotateLeft(player_it->second.facing);
    }
    else if (normalized_direction == "right" || normalized_direction == "r")
    {
        player_it->second.facing = RotateRight(player_it->second.facing);
    }
    else
    {
        const auto parsed_direction = ParseDirection(normalized_direction);
        if (!parsed_direction)
        {
            return TurnResult{
                false, "Direction is required. Try left, right, north, south, east, or west.",
                player_it->second.facing};
        }
        player_it->second.facing = *parsed_direction;
    }
    player_it->second.guarding = false;

    return TurnResult{true, "ok", player_it->second.facing};
}

GuardResult WorldState::GuardPlayer(const std::string& player_id)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto player_it = players_.find(player_id);
    if (player_it == players_.end())
    {
        return GuardResult{false, "Player not found.", FacingDirection::kNorth};
    }
    if (!player_it->second.alive)
    {
        return GuardResult{
            false,
            "You are knocked out. Respawn in " +
                std::to_string(RespawnSecondsRemaining(player_it->second)) + " second(s).",
            FacingDirection::kNorth};
    }

    player_it->second.guarding = true;
    return GuardResult{true, "ok", player_it->second.facing};
}

AttackResult WorldState::AttackInFacing(const std::string& player_id, WeaponType weapon)
{
    std::lock_guard<std::mutex> lock(mutex_);

    AttackResult result;

    auto attacker_it = players_.find(player_id);
    if (attacker_it == players_.end())
    {
        result.message = "Player not found.";
        return result;
    }
    if (!attacker_it->second.alive)
    {
        result.message = "You are knocked out. Respawn in " +
                         std::to_string(RespawnSecondsRemaining(attacker_it->second)) +
                         " second(s).";
        return result;
    }
    attacker_it->second.guarding = false;

    const int range = (weapon == WeaponType::kMelee) ? kMeleeRange : kRangedRange;
    const int damage = (weapon == WeaponType::kMelee) ? kMeleeDamage : kRangedDamage;
    const std::string weapon_name = (weapon == WeaponType::kMelee) ? "melee" : "ranged";

    std::string scan_square_id = attacker_it->second.square_id;
    auto target_player_it = players_.end();
    auto target_npc_it = npcs_.end();
    int hit_distance = 0;

    for (int step = 1; step <= range; ++step)
    {
        auto square_it = squares_.find(scan_square_id);
        if (square_it == squares_.end())
        {
            break;
        }

        const auto next_square_id = NeighborSquareId(square_it->second, attacker_it->second.facing);
        if (!next_square_id)
        {
            break;
        }

        scan_square_id = *next_square_id;

        for (auto it = players_.begin(); it != players_.end(); ++it)
        {
            if (it->first != player_id && it->second.alive &&
                it->second.square_id == scan_square_id)
            {
                target_player_it = it;
                hit_distance = step;
                break;
            }
        }
        if (target_player_it != players_.end())
        {
            break;
        }

        for (auto it = npcs_.begin(); it != npcs_.end(); ++it)
        {
            if (it->second.square_id == scan_square_id)
            {
                target_npc_it = it;
                hit_distance = step;
                break;
            }
        }
        if (target_npc_it != npcs_.end())
        {
            break;
        }
    }

    if (target_player_it == players_.end() && target_npc_it == npcs_.end())
    {
        result.message = "No target in front within " + std::to_string(range) + " square(s).";
        return result;
    }

    result.success = true;
    result.attacker_id = attacker_it->second.id;
    result.attacker_name = attacker_it->second.name;
    result.attacker_square_id = attacker_it->second.square_id;
    result.distance = hit_distance;
    result.damage = damage;

    if (target_player_it != players_.end())
    {
        Player& target = target_player_it->second;
        const auto is_attacker_in_front_arc = [&](const Player& defending_player) -> bool
        {
            auto defending_square_it = squares_.find(defending_player.square_id);
            if (defending_square_it == squares_.end())
            {
                return false;
            }

            std::string cursor = defending_square_it->first;
            while (true)
            {
                auto current_it = squares_.find(cursor);
                if (current_it == squares_.end())
                {
                    return false;
                }

                const auto next_id = NeighborSquareId(current_it->second, defending_player.facing);
                if (!next_id)
                {
                    return false;
                }
                if (*next_id == attacker_it->second.square_id)
                {
                    return true;
                }

                cursor = *next_id;
            }
        };

        if (target.guarding && is_attacker_in_front_arc(target))
        {
            target.guarding = false;
            result.success = true;
            result.target_id = target.id;
            result.target_name = target.name;
            result.target_square_id = target.square_id;
            result.target_is_player = true;
            result.message = "Your attack is blocked by " + target.name + "'s guard.";
            result.attacker_message = result.message;
            result.target_message =
                "You guard from the front and block " + attacker_it->second.name + "'s attack.";
            return result;
        }

        const int hp_after_hit = std::max(0, target.hp - damage);

        result.target_id = target.id;
        result.target_name = target.name;
        result.target_square_id = target.square_id;
        result.target_is_player = true;

        target.hp = hp_after_hit;

        if (target.hp <= 0)
        {
            result.defeated = true;
            target.alive = false;
            target.guarding = false;
            target.respawn_ready_at =
                std::chrono::steady_clock::now() + std::chrono::seconds(kPlayerRespawnDelaySeconds);
            result.message = "You hit " + target.name + " with " + weapon_name + " for " +
                             std::to_string(damage) + " damage and knock them out.";
            result.attacker_message =
                result.message + " They are down for " +
                std::to_string(kPlayerRespawnDelaySeconds) + " seconds.";
            result.target_message = attacker_it->second.name + " hits you with " + weapon_name +
                                    " for " + std::to_string(damage) +
                                    " damage. You were knocked out. Respawn in " +
                                    std::to_string(kPlayerRespawnDelaySeconds) + " seconds.";
        }
        else
        {
            result.message = "You hit " + target.name + " with " + weapon_name + " for " +
                             std::to_string(damage) + " damage.";
            result.attacker_message =
                result.message + " (Target HP: " + std::to_string(target.hp) + ")";
            result.target_message = attacker_it->second.name + " hits you with " + weapon_name +
                                    " for " + std::to_string(damage) + " damage. (HP: " +
                                    std::to_string(target.hp) + ")";
        }

        return result;
    }

    Npc target = target_npc_it->second;
    target_npc_it->second.hp = std::max(0, target_npc_it->second.hp - damage);

    result.target_id = target.id;
    result.target_name = target.name;
    result.target_square_id = target.square_id;
    result.target_is_player = false;

    if (target_npc_it->second.hp <= 0)
    {
        result.defeated = true;
        result.message = "You hit " + target.name + " with " + weapon_name + " for " +
                         std::to_string(damage) + " damage and defeat it.";
        result.attacker_message = result.message;
        npcs_.erase(target_npc_it);
    }
    else
    {
        result.message = "You hit " + target.name + " with " + weapon_name + " for " +
                         std::to_string(damage) + " damage.";
        result.attacker_message = result.message + " (Target HP: " +
                                  std::to_string(target_npc_it->second.hp) + ")";
    }

    return result;
}

std::optional<int> WorldState::GetRespawnSecondsRemaining(const std::string& player_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto player_it = players_.find(player_id);
    if (player_it == players_.end())
    {
        return std::nullopt;
    }

    if (player_it->second.alive)
    {
        return 0;
    }
    return RespawnSecondsRemaining(player_it->second);
}

std::optional<PlayerSnapshot> WorldState::RespawnPlayerIfReady(const std::string& player_id)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto player_it = players_.find(player_id);
    if (player_it == players_.end())
    {
        return std::nullopt;
    }

    Player& player = player_it->second;
    if (player.alive)
    {
        return std::nullopt;
    }
    if (RespawnSecondsRemaining(player) > 0)
    {
        return std::nullopt;
    }

    RespawnPlayer(player);
    return ToPlayerSnapshot(player);
}

std::string WorldState::DescribeSquareForPlayer(const std::string& player_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto player_it = players_.find(player_id);
    if (player_it == players_.end())
    {
        return "Player not found.";
    }
    if (!player_it->second.alive)
    {
        return "You are knocked out. Respawn in " +
               std::to_string(RespawnSecondsRemaining(player_it->second)) + " second(s).";
    }

    auto square_it = squares_.find(player_it->second.square_id);
    if (square_it == squares_.end())
    {
        return "Current square is invalid.";
    }

    const Square& square = square_it->second;

    std::vector<std::string> exits;
    const auto add_exit = [&](FacingDirection direction)
    {
        if (NeighborSquareId(square, direction))
        {
            exits.push_back(FacingDirectionToString(direction));
        }
    };
    add_exit(FacingDirection::kNorth);
    add_exit(FacingDirection::kEast);
    add_exit(FacingDirection::kSouth);
    add_exit(FacingDirection::kWest);

    std::string front_info = "blocked";
    const auto front_square_id = NeighborSquareId(square, player_it->second.facing);
    if (front_square_id)
    {
        front_info = "clear";

        for (const auto& [id, player] : players_)
        {
            if (id != player_id && player.alive && player.square_id == *front_square_id)
            {
                front_info = "player " + player.name;
                break;
            }
        }

        if (front_info == "clear")
        {
            for (const auto& [id, npc] : npcs_)
            {
                (void)id;
                if (npc.square_id == *front_square_id)
                {
                    front_info = "npc " + npc.name;
                    break;
                }
            }
        }
    }

    std::ostringstream out;
    out << "Square " << square.id << " [" << square.x << "," << square.y << "]. "
        << square.description << " Exits: " << Join(exits, ", ") << ". "
        << "Facing: " << FacingDirectionToString(player_it->second.facing) << ". "
        << "Guard: " << (player_it->second.guarding ? "up" : "down") << ". "
        << "HP: " << player_it->second.hp << ". "
        << "Front: " << front_info << ".";
    return out.str();
}

std::optional<LocalView> WorldState::BuildLocalView(const std::string& player_id, int radius) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto player_it = players_.find(player_id);
    if (player_it == players_.end())
    {
        return std::nullopt;
    }

    auto center_it = squares_.find(player_it->second.square_id);
    if (center_it == squares_.end())
    {
        return std::nullopt;
    }

    LocalView view;
    view.center_square_id = center_it->second.id;
    view.center_x = center_it->second.x;
    view.center_y = center_it->second.y;
    view.facing = player_it->second.facing;
    view.radius = std::max(0, radius);
    std::unordered_set<std::string> visible_square_ids;
    visible_square_ids.reserve(squares_.size());

    for (const auto& [square_id, square] : squares_)
    {
        const int dx = std::abs(square.x - view.center_x);
        const int dy = std::abs(square.y - view.center_y);
        if (dx > view.radius || dy > view.radius)
        {
            continue;
        }
        if (!HasLineOfSight(center_it->second, square))
        {
            continue;
        }

        visible_square_ids.insert(square_id);
        view.squares.push_back(ViewSquare{
            square.id, square.x, square.y, square.open_north, square.open_east, square.open_south,
            square.open_west});
    }

    for (const auto& [id, player] : players_)
    {
        if (!player.alive)
        {
            continue;
        }

        if (visible_square_ids.find(player.square_id) == visible_square_ids.end())
        {
            continue;
        }

        auto square_it = squares_.find(player.square_id);
        if (square_it == squares_.end())
        {
            continue;
        }

        ViewActor actor;
        actor.actor_id = id;
        actor.name = player.name;
        actor.x = square_it->second.x;
        actor.y = square_it->second.y;
        actor.kind = (id == player_id) ? ViewActorKind::kSelf : ViewActorKind::kPlayer;
        actor.facing = player.facing;
        view.actors.push_back(actor);
    }

    for (const auto& [id, npc] : npcs_)
    {
        if (visible_square_ids.find(npc.square_id) == visible_square_ids.end())
        {
            continue;
        }

        auto square_it = squares_.find(npc.square_id);
        if (square_it == squares_.end())
        {
            continue;
        }

        ViewActor actor;
        actor.actor_id = id;
        actor.name = npc.name;
        actor.x = square_it->second.x;
        actor.y = square_it->second.y;
        actor.kind = ViewActorKind::kNpc;
        actor.facing = npc.facing;
        view.actors.push_back(actor);
    }

    std::sort(view.squares.begin(), view.squares.end(),
              [](const ViewSquare& lhs, const ViewSquare& rhs)
              {
                  if (lhs.y != rhs.y)
                  {
                      return lhs.y < rhs.y;
                  }
                  return lhs.x < rhs.x;
              });

    std::sort(view.actors.begin(), view.actors.end(),
              [](const ViewActor& lhs, const ViewActor& rhs)
              {
                  if (lhs.y != rhs.y)
                  {
                      return lhs.y < rhs.y;
                  }
                  if (lhs.x != rhs.x)
                  {
                      return lhs.x < rhs.x;
                  }
                  return lhs.actor_id < rhs.actor_id;
              });

    return view;
}

std::vector<NearbyPlayer> WorldState::GetPlayersWithinRange(const std::string& origin_square_id,
                                                            int max_distance) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NearbyPlayer> result;

    const auto distances = DistancesFrom(origin_square_id, max_distance);
    if (distances.empty())
    {
        return result;
    }

    for (const auto& [id, player] : players_)
    {
        (void)id;
        if (!player.alive)
        {
            continue;
        }
        auto dist_it = distances.find(player.square_id);
        if (dist_it == distances.end())
        {
            continue;
        }
        result.push_back(NearbyPlayer{ToPlayerSnapshot(player), dist_it->second});
    }

    return result;
}

std::string WorldState::ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string WorldState::Trim(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, (last - first) + 1);
}

std::optional<FacingDirection> WorldState::ParseDirection(const std::string& direction_text)
{
    const std::string value = ToLower(Trim(direction_text));
    if (value == "north" || value == "n")
    {
        return FacingDirection::kNorth;
    }
    if (value == "east" || value == "e")
    {
        return FacingDirection::kEast;
    }
    if (value == "south" || value == "s")
    {
        return FacingDirection::kSouth;
    }
    if (value == "west" || value == "w")
    {
        return FacingDirection::kWest;
    }
    return std::nullopt;
}

std::string WorldState::CoordinateKey(int x, int y)
{
    return std::to_string(x) + ":" + std::to_string(y);
}

std::string WorldState::Join(const std::vector<std::string>& items, const std::string& delimiter)
{
    if (items.empty())
    {
        return {};
    }

    std::ostringstream out;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        if (i > 0)
        {
            out << delimiter;
        }
        out << items[i];
    }
    return out.str();
}

bool WorldState::LoadMapDataFromDisk()
{
    const std::filesystem::path db_path(map_db_path_);
    if (!std::filesystem::exists(db_path))
    {
        return false;
    }

    std::ifstream input(db_path, std::ios::binary);
    if (!input.is_open())
    {
        return false;
    }

    mud::v1::SquareMapData map_data;
    if (!map_data.ParseFromIstream(&input))
    {
        return false;
    }

    if (map_data.squares().empty())
    {
        return false;
    }

    squares_.clear();
    map_width_ = 0;
    map_height_ = 0;

    for (const auto& record : map_data.squares())
    {
        if (record.square_id().empty())
        {
            continue;
        }

        Square square;
        square.id = record.square_id();
        square.x = record.x();
        square.y = record.y();
        square.open_north = record.open_north();
        square.open_east = record.open_east();
        square.open_south = record.open_south();
        square.open_west = record.open_west();
        square.description = record.description();
        if (square.description.empty())
        {
            square.description = "A training square on the quest board.";
        }

        squares_[square.id] = square;
        map_width_ = std::max(map_width_, square.x + 1);
        map_height_ = std::max(map_height_, square.y + 1);
    }

    return !squares_.empty();
}

bool WorldState::SaveMapDataToDisk() const
{
    std::filesystem::path db_path(map_db_path_);
    if (db_path.has_parent_path())
    {
        std::filesystem::create_directories(db_path.parent_path());
    }

    mud::v1::SquareMapData map_data;

    std::vector<std::string> ids;
    ids.reserve(squares_.size());
    for (const auto& [id, square] : squares_)
    {
        (void)square;
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());

    for (const auto& id : ids)
    {
        const Square& square = squares_.at(id);
        auto* record = map_data.add_squares();
        record->set_square_id(square.id);
        record->set_x(square.x);
        record->set_y(square.y);
        record->set_open_north(square.open_north);
        record->set_open_east(square.open_east);
        record->set_open_south(square.open_south);
        record->set_open_west(square.open_west);
        record->set_description(square.description);
    }

    std::ofstream output(db_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        return false;
    }
    return map_data.SerializeToOstream(&output);
}

void WorldState::CreateDefaultMapData()
{
    squares_.clear();
    map_width_ = kDefaultMapWidth;
    map_height_ = kDefaultMapHeight;

    for (int y = 0; y < map_height_; ++y)
    {
        for (int x = 0; x < map_width_; ++x)
        {
            Square square;
            square.id = "sq-" + std::to_string(x) + "-" + std::to_string(y);
            square.x = x;
            square.y = y;

            square.open_north = y > 0;
            square.open_south = y < (map_height_ - 1);
            square.open_east = x < (map_width_ - 1);
            square.open_west = x > 0;

            // One long divider wall with gates at top and bottom rows.
            if (x == 2 && y > 0 && y < (map_height_ - 1))
            {
                square.open_east = false;
            }
            if (x == 3 && y > 0 && y < (map_height_ - 1))
            {
                square.open_west = false;
            }

            square.description = "A battle square on the quest board.";
            squares_[square.id] = square;
        }
    }
}

void WorldState::BuildCoordinateIndex()
{
    coordinate_index_.clear();
    map_width_ = 0;
    map_height_ = 0;

    for (const auto& [id, square] : squares_)
    {
        coordinate_index_[CoordinateKey(square.x, square.y)] = id;
        map_width_ = std::max(map_width_, square.x + 1);
        map_height_ = std::max(map_height_, square.y + 1);
    }
}

void WorldState::SpawnDefaultNpcs()
{
    npcs_.clear();

    const auto add_npc = [&](const std::string& name, int x, int y)
    {
        auto key_it = coordinate_index_.find(CoordinateKey(x, y));
        if (key_it == coordinate_index_.end())
        {
            return;
        }

        const std::string& square_id = key_it->second;
        if (IsSquareOccupied(square_id))
        {
            return;
        }

        Npc npc;
        npc.id = "npc-" + std::to_string(next_npc_id_++);
        npc.name = name;
        npc.square_id = square_id;
        std::uniform_int_distribution<int> facing_dist(0, 3);
        npc.facing = static_cast<FacingDirection>(facing_dist(rng_));
        npc.hp = kNpcMaxHp;
        npcs_[npc.id] = npc;
    };

    add_npc("wolf scout", map_width_ - 1, map_height_ - 1);
    add_npc("bandit archer", map_width_ - 1, 0);
    add_npc("rust sentinel", map_width_ / 2, map_height_ / 2);
}

bool WorldState::IsSquareOpen(const Square& square, FacingDirection direction) const
{
    switch (direction)
    {
    case FacingDirection::kNorth:
        return square.open_north;
    case FacingDirection::kEast:
        return square.open_east;
    case FacingDirection::kSouth:
        return square.open_south;
    case FacingDirection::kWest:
        return square.open_west;
    }
    return false;
}

FacingDirection WorldState::OppositeDirection(FacingDirection direction) const
{
    switch (direction)
    {
    case FacingDirection::kNorth:
        return FacingDirection::kSouth;
    case FacingDirection::kEast:
        return FacingDirection::kWest;
    case FacingDirection::kSouth:
        return FacingDirection::kNorth;
    case FacingDirection::kWest:
        return FacingDirection::kEast;
    }
    return FacingDirection::kNorth;
}

std::optional<std::string> WorldState::NeighborSquareId(const Square& square,
                                                        FacingDirection direction) const
{
    if (!IsSquareOpen(square, direction))
    {
        return std::nullopt;
    }

    int nx = square.x;
    int ny = square.y;
    switch (direction)
    {
    case FacingDirection::kNorth:
        --ny;
        break;
    case FacingDirection::kEast:
        ++nx;
        break;
    case FacingDirection::kSouth:
        ++ny;
        break;
    case FacingDirection::kWest:
        --nx;
        break;
    }

    auto coord_it = coordinate_index_.find(CoordinateKey(nx, ny));
    if (coord_it == coordinate_index_.end())
    {
        return std::nullopt;
    }

    const std::string& neighbor_id = coord_it->second;
    auto neighbor_it = squares_.find(neighbor_id);
    if (neighbor_it == squares_.end())
    {
        return std::nullopt;
    }

    if (!IsSquareOpen(neighbor_it->second, OppositeDirection(direction)))
    {
        return std::nullopt;
    }

    return neighbor_id;
}

bool WorldState::CanTraverseBetweenCells(int from_x, int from_y, int to_x, int to_y) const
{
    const int dx = to_x - from_x;
    const int dy = to_y - from_y;
    if ((std::abs(dx) + std::abs(dy)) != 1)
    {
        return false;
    }

    auto from_coord_it = coordinate_index_.find(CoordinateKey(from_x, from_y));
    auto to_coord_it = coordinate_index_.find(CoordinateKey(to_x, to_y));
    if (from_coord_it == coordinate_index_.end() || to_coord_it == coordinate_index_.end())
    {
        return false;
    }

    auto from_square_it = squares_.find(from_coord_it->second);
    if (from_square_it == squares_.end())
    {
        return false;
    }

    FacingDirection direction = FacingDirection::kNorth;
    if (dx == 1)
    {
        direction = FacingDirection::kEast;
    }
    else if (dx == -1)
    {
        direction = FacingDirection::kWest;
    }
    else if (dy == 1)
    {
        direction = FacingDirection::kSouth;
    }

    const auto neighbor_id = NeighborSquareId(from_square_it->second, direction);
    return neighbor_id && *neighbor_id == to_coord_it->second;
}

bool WorldState::HasLineOfSight(const Square& from, const Square& to) const
{
    if (from.id == to.id)
    {
        return true;
    }

    const int dx_cells = to.x - from.x;
    const int dy_cells = to.y - from.y;
    const int span = std::max(std::abs(dx_cells), std::abs(dy_cells));
    if (span == 0)
    {
        return true;
    }

    const int sample_count = span * 24;
    const double start_x = static_cast<double>(from.x) + 0.5;
    const double start_y = static_cast<double>(from.y) + 0.5;
    const double ray_x = static_cast<double>(dx_cells);
    const double ray_y = static_cast<double>(dy_cells);

    int current_x = from.x;
    int current_y = from.y;

    for (int i = 1; i <= sample_count; ++i)
    {
        const double t = static_cast<double>(i) / static_cast<double>(sample_count);
        const int sample_x = static_cast<int>(std::floor(start_x + (ray_x * t)));
        const int sample_y = static_cast<int>(std::floor(start_y + (ray_y * t)));

        if (sample_x == current_x && sample_y == current_y)
        {
            continue;
        }

        const int step_x = sample_x - current_x;
        const int step_y = sample_y - current_y;
        if (std::abs(step_x) > 1 || std::abs(step_y) > 1)
        {
            return false;
        }

        bool traversable = false;
        if (step_x != 0 && step_y != 0)
        {
            // Crossing exactly near a corner: accept only if one L-shaped path is open.
            const bool horizontal_then_vertical =
                CanTraverseBetweenCells(current_x, current_y, current_x + step_x, current_y) &&
                CanTraverseBetweenCells(current_x + step_x, current_y, sample_x, sample_y);
            const bool vertical_then_horizontal =
                CanTraverseBetweenCells(current_x, current_y, current_x, current_y + step_y) &&
                CanTraverseBetweenCells(current_x, current_y + step_y, sample_x, sample_y);
            traversable = horizontal_then_vertical || vertical_then_horizontal;
        }
        else
        {
            traversable = CanTraverseBetweenCells(current_x, current_y, sample_x, sample_y);
        }

        if (!traversable)
        {
            return false;
        }

        current_x = sample_x;
        current_y = sample_y;
        if (current_x == to.x && current_y == to.y)
        {
            return true;
        }
    }

    return current_x == to.x && current_y == to.y;
}

std::unordered_map<std::string, int> WorldState::DistancesFrom(const std::string& origin_square_id,
                                                               int max_distance) const
{
    std::unordered_map<std::string, int> distances;
    if (squares_.find(origin_square_id) == squares_.end())
    {
        return distances;
    }

    std::queue<std::string> frontier;
    frontier.push(origin_square_id);
    distances[origin_square_id] = 0;

    while (!frontier.empty())
    {
        const std::string current_id = frontier.front();
        frontier.pop();

        const int current_distance = distances[current_id];
        if (current_distance >= max_distance)
        {
            continue;
        }

        const Square& current_square = squares_.at(current_id);
        for (FacingDirection direction :
             {FacingDirection::kNorth, FacingDirection::kEast, FacingDirection::kSouth, FacingDirection::kWest})
        {
            const auto next_id = NeighborSquareId(current_square, direction);
            if (!next_id)
            {
                continue;
            }

            if (distances.find(*next_id) != distances.end())
            {
                continue;
            }

            distances[*next_id] = current_distance + 1;
            frontier.push(*next_id);
        }
    }

    return distances;
}

bool WorldState::IsSquareOccupiedByPlayer(const std::string& square_id,
                                          const std::string& ignored_player_id) const
{
    for (const auto& [id, player] : players_)
    {
        if (!player.alive)
        {
            continue;
        }
        if (!ignored_player_id.empty() && id == ignored_player_id)
        {
            continue;
        }
        if (player.square_id == square_id)
        {
            return true;
        }
    }
    return false;
}

bool WorldState::IsSquareOccupiedByNpc(const std::string& square_id) const
{
    for (const auto& [id, npc] : npcs_)
    {
        (void)id;
        if (npc.square_id == square_id)
        {
            return true;
        }
    }
    return false;
}

bool WorldState::IsSquareOccupied(const std::string& square_id,
                                  const std::string& ignored_player_id) const
{
    return IsSquareOccupiedByPlayer(square_id, ignored_player_id) || IsSquareOccupiedByNpc(square_id);
}

std::string WorldState::RandomFreeSquareId(const std::unordered_set<std::string>& exclusions) const
{
    std::vector<std::string> candidates;
    candidates.reserve(squares_.size());

    for (const auto& [id, square] : squares_)
    {
        (void)square;
        if (exclusions.find(id) != exclusions.end())
        {
            continue;
        }
        if (!IsSquareOccupied(id))
        {
            candidates.push_back(id);
        }
    }

    if (candidates.empty())
    {
        return {};
    }

    std::uniform_int_distribution<std::size_t> dist(0, candidates.size() - 1);
    return candidates[dist(rng_)];
}

int WorldState::RespawnSecondsRemaining(const Player& player) const
{
    if (player.alive)
    {
        return 0;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= player.respawn_ready_at)
    {
        return 0;
    }

    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(player.respawn_ready_at - now).count();
    return std::max(1, static_cast<int>((remaining_ms + 999) / 1000));
}

void WorldState::RespawnPlayer(Player& player)
{
    std::string respawn_square_id = RandomFreeSquareId();
    if (respawn_square_id.empty())
    {
        if (!player.square_id.empty())
        {
            respawn_square_id = player.square_id;
        }
        else if (!squares_.empty())
        {
            respawn_square_id = squares_.begin()->first;
        }
    }

    player.square_id = respawn_square_id;
    player.facing = FacingDirection::kNorth;
    player.guarding = false;
    player.alive = true;
    player.respawn_ready_at = std::chrono::steady_clock::time_point::min();
    player.hp = kPlayerMaxHp;
}

PlayerSnapshot WorldState::ToPlayerSnapshot(const Player& player)
{
    return PlayerSnapshot{player.id, player.name, player.square_id, player.facing, player.hp};
}
} // namespace grpcmud::server
