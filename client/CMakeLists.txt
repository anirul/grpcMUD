add_executable(
  grpcmud_client
  main.cpp
  ClientApp.cpp
  GrpcSession.cpp
  MapRenderer.cpp
  TerminalUi.cpp
)

target_link_libraries(grpcmud_client PRIVATE grpcmud_proto)
target_include_directories(grpcmud_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
