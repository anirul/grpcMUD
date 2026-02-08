add_executable(
  grpcmud_server
  main.cpp
  ClientSession.cpp
  MudServiceImpl.cpp
  WorldState.cpp
)

target_link_libraries(grpcmud_server PRIVATE grpcmud_proto)
target_include_directories(grpcmud_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
