#pragma once

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ClientSession.hpp"
#include "WorldState.hpp"
#include "mud.grpc.pb.h"

namespace grpcmud::server
{
class MudServiceImpl final : public mud::v1::MudService::Service
{
public:
    explicit MudServiceImpl(const std::string& map_db_path = "data/world_map.pb");

    grpc::Status Play(
        grpc::ServerContext* context,
        grpc::ServerReaderWriter<mud::v1::ServerMessage, mud::v1::ClientMessage>* stream) override;

private:
    static std::uint64_t NowMs();
    static std::string Trim(const std::string& text);
    static std::string ToLower(std::string text);
    static std::pair<std::string, std::string> SplitCommand(const std::string& text);
    static std::string NormalizePeerAddress(const std::string& peer);

    mud::v1::ServerMessage MakeBaseMessage(std::uint64_t tick_id) const;

    bool SendError(const std::shared_ptr<ClientSession>& session, std::uint64_t tick_id,
                   const std::string& code, const std::string& message) const;
    bool SendAck(const std::shared_ptr<ClientSession>& session, std::uint64_t tick_id,
                 const std::string& request_id, bool accepted,
                 const std::string& message) const;
    bool SendWorldEvent(const std::shared_ptr<ClientSession>& session, std::uint64_t tick_id,
                        mud::v1::WorldEvent::Kind kind, const std::string& text,
                        const std::string& room_id = std::string{},
                        const std::string& actor_id = std::string{}) const;
    bool SendViewUpdate(const std::shared_ptr<ClientSession>& session, std::uint64_t tick_id,
                        const std::string& player_id) const;

    void RegisterSession(const std::string& player_id, const std::shared_ptr<ClientSession>& session);
    void UnregisterSession(const std::string& player_id);

    std::vector<std::pair<NearbyPlayer, std::shared_ptr<ClientSession>>>
    ResolveRecipients(const std::vector<NearbyPlayer>& nearby);
    std::shared_ptr<ClientSession> GetSession(const std::string& player_id);

    void BroadcastSay(const PlayerSnapshot& speaker, const std::string& text, std::uint64_t tick_id);
    void LogConnectionAdded(const std::string& peer_address);
    void LogConnectionRemoved(const std::string& peer_address);

    WorldState world_;
    std::atomic<std::uint64_t> tick_counter_{0};

    std::mutex connection_count_mutex_;
    std::size_t connected_clients_ = 0;

    std::mutex sessions_mutex_;
    std::unordered_map<std::string, std::weak_ptr<ClientSession>> sessions_;
    const int say_range_squares_ = 3;
    const int view_radius_squares_ = 4;
};
} // namespace grpcmud::server
