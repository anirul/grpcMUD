#pragma once

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ClientSession.hpp"
#include "WorldState.hpp"
#include "gameplay.grpc.pb.h"

namespace grpcmud::server
{
class MudServiceImpl final : public mud::v1::MudService::Service
{
public:
    explicit MudServiceImpl(const std::string& map_db_path = "data/world_state.json",
                            std::chrono::seconds autosave_interval = std::chrono::seconds(60),
                            std::chrono::milliseconds tick_interval = std::chrono::milliseconds(500));
    ~MudServiceImpl();

    grpc::Status Play(
        grpc::ServerContext* context,
        grpc::ServerReaderWriter<mud::v1::ServerMessage, mud::v1::ClientMessage>* stream) override;

private:
    static std::uint64_t NowMs();
    static std::string Trim(const std::string& text);
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
    void StartAutosaveTask();
    void StopAutosaveTask();

    WorldState world_;
    std::atomic<std::uint64_t> tick_counter_{0};

    std::mutex connection_count_mutex_;
    std::size_t connected_clients_ = 0;

    std::mutex sessions_mutex_;
    std::unordered_map<std::string, std::weak_ptr<ClientSession>> sessions_;
    const int say_range_squares_ = 3;
    const int view_front_squares_ = 3;
    const int view_back_squares_ = 1;
    const int view_side_squares_ = 2;
    const int first_person_depth_squares_ = 2;

    std::chrono::seconds autosave_interval_;
    std::chrono::milliseconds tick_interval_;
    std::atomic<bool> autosave_stop_{false};
    std::mutex autosave_mutex_;
    std::condition_variable autosave_cv_;
    std::thread autosave_thread_;
};
} // namespace grpcmud::server
