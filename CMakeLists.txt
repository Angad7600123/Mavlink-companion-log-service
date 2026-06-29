cmake_minimum_required(VERSION 3.16)
project(mavlink-companion-log-service VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MCLS_BUILD_TESTS "Build unit tests" ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
    mavlink
    GIT_REPOSITORY https://github.com/mavlink/c_library_v2.git
    GIT_TAG master
)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)

FetchContent_MakeAvailable(mavlink tomlplusplus spdlog json)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SQLITE3 QUIET sqlite3)
endif()
if(NOT SQLITE3_FOUND)
    find_library(SQLITE3_LIBRARIES NAMES sqlite3 REQUIRED)
    find_path(SQLITE3_INCLUDE_DIRS sqlite3.h REQUIRED)
endif()

find_library(SYSTEMD_LIBRARIES NAMES systemd)
if(SYSTEMD_LIBRARIES)
    set(MCLS_HAS_SYSTEMD ON)
    message(STATUS "Found libsystemd: journal logging enabled")
else()
    set(MCLS_HAS_SYSTEMD OFF)
    message(STATUS "libsystemd not found: journal logging disabled")
endif()

add_library(mcls_lib STATIC
    src/Config.cpp
    src/Logger.cpp
    src/Sha256.cpp
    src/Crc32.cpp
    src/ReceivedRanges.cpp
    src/ChunkCoverageTracker.cpp
    src/ArchiveSummary.cpp
    src/FcSampleOffsets.cpp
    src/DataFlashValidator.cpp
    src/StreamDownloadSession.cpp
    src/Database.cpp
    src/Transport.cpp
    src/TcpTransport.cpp
    src/UdpTransport.cpp
    src/MavlinkClient.cpp
    src/FlightMonitor.cpp
    src/StorageManager.cpp
    src/LogDownloader.cpp
    src/DroneLogService.cpp
    src/ServiceSnapshot.cpp
    src/CompanionProtocol.cpp
    src/CompanionDiagnostics.cpp
    src/CompanionUdpServer.cpp
)

target_include_directories(mcls_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${mavlink_SOURCE_DIR}
    ${tomlplusplus_SOURCE_DIR}/include
)
target_include_directories(mcls_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(mcls_lib PUBLIC
    spdlog::spdlog
    ${SQLITE3_LIBRARIES}
    nlohmann_json::nlohmann_json
)

if(MCLS_HAS_SYSTEMD)
    target_compile_definitions(mcls_lib PUBLIC MCLS_HAS_SYSTEMD=1)
    target_link_libraries(mcls_lib PUBLIC ${SYSTEMD_LIBRARIES})
endif()

if(WIN32)
    target_link_libraries(mcls_lib PUBLIC ws2_32)
endif()

add_executable(mcls src/main.cpp)
target_link_libraries(mcls PRIVATE mcls_lib)
set_target_properties(mcls PROPERTIES OUTPUT_NAME mcls)

if(MCLS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS mcls RUNTIME DESTINATION bin)
install(FILES config/config.toml config/sample_master_config.toml DESTINATION etc/mcls)
install(FILES systemd/mavlink-companion-log-service.service DESTINATION lib/systemd/system)
