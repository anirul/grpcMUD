#include <grpcpp/grpcpp.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

#include "MudServiceImpl.hpp"

ABSL_FLAG(std::string, map_file, "data/world_state.json",
          "World data file path (.json for human-readable, any other extension for binary protobuf).");
ABSL_FLAG(int, autosave_seconds, 60, "Autosave interval in seconds.");
ABSL_FLAG(int, tick_interval_ms, 500, "Server tick interval in milliseconds.");

int main(int argc, char** argv)
{
    absl::ParseCommandLine(argc, argv);

    const std::string map_file = absl::GetFlag(FLAGS_map_file);
    const int autosave_seconds = absl::GetFlag(FLAGS_autosave_seconds);
    const int tick_interval_ms = absl::GetFlag(FLAGS_tick_interval_ms);
    if (autosave_seconds <= 0)
    {
        std::cerr << "Invalid --autosave_seconds value: must be a positive integer." << std::endl;
        return 1;
    }
    if (tick_interval_ms <= 0)
    {
        std::cerr << "Invalid --tick_interval_ms value: must be a positive integer." << std::endl;
        return 1;
    }

    const std::string server_address = "0.0.0.0:50051";
    grpcmud::server::MudServiceImpl service(map_file, std::chrono::seconds(autosave_seconds),
                                            std::chrono::milliseconds(tick_interval_ms));

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server)
    {
        std::cerr << "Failed to start grpcMUD server on " << server_address << std::endl;
        return 1;
    }

    std::cout << "grpcMUD server listening on " << server_address << " using map file "
              << map_file << " (autosave " << autosave_seconds << "s, tick "
              << tick_interval_ms << "ms)" << std::endl;
    server->Wait();
    return 0;
}
