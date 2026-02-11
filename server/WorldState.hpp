#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace grpcmud::server
{
enum class FacingDirection
{
    kNorth,
    kEast,
    kSouth,
    kWest
};

enum class WeaponType
{
    kMelee,
    kRanged
};

enum class SquareKind
{
    kFloor,
    kWall
};

std::string FacingDirectionToString(FacingDirection direction);

struct ViewSquare
{
    std::string square_id;
    int x = 0;
    int y = 0;
    SquareKind kind = SquareKind::kFloor;
    bool open_north = false;
    bool open_east = false;
    bool open_south = false;
    bool open_west = false;
};

enum class ViewActorKind
{
    kSelf,
    kPlayer,
    kNpc
};

struct ViewActor
{
    std::string actor_id;
    std::string name;
    int x = 0;
    int y = 0;
    ViewActorKind kind = ViewActorKind::kPlayer;
    FacingDirection facing = FacingDirection::kNorth;
};

struct LocalView
{
    std::string center_square_id;
    int center_x = 0;
    int center_y = 0;
    FacingDirection facing = FacingDirection::kNorth;
    int radius = 0;
    std::vector<ViewSquare> squares;
    std::vector<ViewActor> actors;
};

struct FirstPersonCell
{
    int depth = 0;
    int lane = 0;
    bool visible = false;
    bool open_forward = false;
    bool open_left = false;
    bool open_right = false;
    bool has_actor = false;
    ViewActorKind actor_kind = ViewActorKind::kPlayer;
    FacingDirection actor_facing = FacingDirection::kNorth;
    std::string actor_name;
};

struct FirstPersonView
{
    FacingDirection facing = FacingDirection::kNorth;
    int max_depth = 0;
    std::vector<FirstPersonCell> cells;
};

struct PlayerSnapshot
{
    std::string player_id;
    std::string player_name;
    std::string square_id;
    FacingDirection facing = FacingDirection::kNorth;
    int hp = 0;
};

struct NearbyPlayer
{
    PlayerSnapshot player;
    int distance = 0;
};

struct MoveResult
{
    bool success = false;
    std::string message;
    std::string square_id;
};

struct TurnResult
{
    bool success = false;
    std::string message;
    FacingDirection facing = FacingDirection::kNorth;
};

struct GuardResult
{
    bool success = false;
    std::string message;
    FacingDirection facing = FacingDirection::kNorth;
};

struct AttackResult
{
    bool success = false;
    std::string message;

    std::string attacker_id;
    std::string attacker_name;
    std::string attacker_square_id;

    std::string target_id;
    std::string target_name;
    std::string target_square_id;
    bool target_is_player = false;

    int distance = 0;
    int damage = 0;
    bool defeated = false;
    bool target_respawned = false;
    std::string respawn_square_id;

    std::string attacker_message;
    std::string target_message;
};

class WorldState
{
public:
    explicit WorldState(std::string map_db_path = "data/world_map.pb");

    PlayerSnapshot AddPlayer(const std::string& requested_name);
    void RemovePlayer(const std::string& player_id);
    std::optional<PlayerSnapshot> GetPlayer(const std::string& player_id) const;

    MoveResult MovePlayer(const std::string& player_id, const std::string& direction);
    TurnResult TurnPlayer(const std::string& player_id, const std::string& direction);
    GuardResult GuardPlayer(const std::string& player_id);
    AttackResult AttackInFacing(const std::string& player_id, WeaponType weapon);
    std::optional<int> GetRespawnSecondsRemaining(const std::string& player_id) const;
    std::optional<PlayerSnapshot> RespawnPlayerIfReady(const std::string& player_id);

    std::string DescribeSquareForPlayer(const std::string& player_id) const;
    std::optional<LocalView> BuildLocalView(const std::string& player_id, int radius) const;
    std::optional<FirstPersonView> BuildFirstPersonView(const std::string& player_id,
                                                        int max_depth) const;
    std::vector<NearbyPlayer> GetPlayersWithinRange(const std::string& origin_square_id,
                                                    int max_distance) const;

private:
    struct Square
    {
        std::string id;
        int x = 0;
        int y = 0;
        SquareKind kind = SquareKind::kFloor;
        bool open_north = false;
        bool open_east = false;
        bool open_south = false;
        bool open_west = false;
        std::string description;
    };

    struct Player
    {
        std::string id;
        std::string name;
        std::string square_id;
        FacingDirection facing = FacingDirection::kNorth;
        bool guarding = false;
        bool alive = true;
        std::chrono::steady_clock::time_point respawn_ready_at;
        int hp = 0;
    };

    struct Npc
    {
        std::string id;
        std::string name;
        std::string square_id;
        FacingDirection facing = FacingDirection::kNorth;
        int hp = 0;
    };

    static std::string ToLower(std::string value);
    static std::string Trim(const std::string& value);
    static std::optional<FacingDirection> ParseDirection(const std::string& direction_text);

    static std::string CoordinateKey(int x, int y);
    static std::string Join(const std::vector<std::string>& items, const std::string& delimiter);

    bool LoadMapDataFromDisk();
    bool SaveMapDataToDisk() const;
    void CreateDefaultMapData();
    void RecomputeOpenEdgesFromWalls();
    bool HasWallSquares() const;
    void BuildCoordinateIndex();
    void SpawnDefaultNpcs();

    bool IsSquareOpen(const Square& square, FacingDirection direction) const;
    FacingDirection OppositeDirection(FacingDirection direction) const;
    std::optional<std::string> NeighborSquareId(const Square& square,
                                                FacingDirection direction) const;
    bool HasLineOfSight(const Square& from, const Square& to) const;
    std::unordered_map<std::string, int> DistancesFrom(const std::string& origin_square_id,
                                                       int max_distance) const;

    bool IsSquareOccupiedByPlayer(const std::string& square_id,
                                  const std::string& ignored_player_id = std::string{}) const;
    bool IsSquareOccupiedByNpc(const std::string& square_id) const;
    bool IsSquareOccupied(const std::string& square_id,
                          const std::string& ignored_player_id = std::string{}) const;

    std::string RandomFreeSquareId(const std::unordered_set<std::string>& exclusions = {}) const;
    int RespawnSecondsRemaining(const Player& player) const;
    void RespawnPlayer(Player& player);
    static PlayerSnapshot ToPlayerSnapshot(const Player& player);

    std::string map_db_path_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Square> squares_;
    std::unordered_map<std::string, std::string> coordinate_index_;
    std::unordered_map<std::string, Player> players_;
    std::unordered_map<std::string, Npc> npcs_;
    std::uint64_t next_player_id_ = 1;
    std::uint64_t next_npc_id_ = 1;
    int map_width_ = 0;
    int map_height_ = 0;
    mutable std::mt19937 rng_;
};
} // namespace grpcmud::server
