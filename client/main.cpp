#include "ClientApp.hpp"

#include <exception>
#include <iostream>

#if defined(_WIN32) || defined(_WIN64)
#define WINDOWS_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "frame/logger.h"

namespace
{
int Run(int argc, char** argv)
{
    grpcmud::client::ClientApp app;
    return app.Run(argc, argv);
}
} // namespace

#if defined(_WIN32) || defined(_WIN64)
int WINAPI WinMain(
    _In_ HINSTANCE /*hInstance*/,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_ LPSTR /*lpCmdLine*/,
    _In_ int /*nShowCmd*/)
try
{
    return Run(__argc, __argv);
}
#else
int main(int argc, char** argv)
try
{
    return Run(argc, argv);
}
#endif
catch (const std::exception& ex)
{
    auto& logger = frame::Logger::GetInstance();
    logger->error("Unhandled exception in grpcmud_client: {}", ex.what());
    logger->flush();
#if defined(_WIN32) || defined(_WIN64)
    MessageBoxA(nullptr, ex.what(), "grpcMUD Client Error", MB_OK | MB_ICONERROR);
#else
    std::cerr << ex.what() << std::endl;
    std::cerr.flush();
#endif
    return 1;
}
