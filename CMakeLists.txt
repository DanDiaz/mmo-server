cmake_minimum_required(VERSION 3.15)
project(MMOProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(BUILD_CLIENT "Build MMO test client" ON)
option(BUILD_SERVER "Build MMO server" ON)

# Platform-specific settings
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601) # Windows 7+
endif()

# Server target
if(BUILD_SERVER)
    add_executable(mmo-server src/main.cpp)
    if(WIN32)
        target_link_libraries(mmo-server PRIVATE ws2_32)
    endif()
endif()

# Client target
if(BUILD_CLIENT)
    add_executable(test-client src/TestClient.cpp)
    if(WIN32)
        target_link_libraries(test-client PRIVATE ws2_32)
    endif()
endif()
