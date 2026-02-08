#include <grpcpp/grpcpp.h>

#include <iostream>
#include <memory>
#include <string>

#include "MudServiceImpl.hpp"

int main()
{
    const std::string server_address = "0.0.0.0:50051";
    grpcmud::server::MudServiceImpl service;

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server)
    {
        std::cerr << "Failed to start grpcMUD server on " << server_address << std::endl;
        return 1;
    }

    std::cout << "grpcMUD server listening on " << server_address << std::endl;
    server->Wait();
    return 0;
}

