#include "ClientApp.hpp"

int main(int argc, char** argv)
{
    grpcmud::client::ClientApp app;
    return app.Run(argc, argv);
}

