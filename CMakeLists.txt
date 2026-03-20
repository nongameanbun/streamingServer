cmake_minimum_required(VERSION 3.16)

# Allow older CMake policies for compatibility with dependencies
if(POLICY CMP0077)
    cmake_policy(SET CMP0077 NEW)
endif()
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum CMake policy version" FORCE)

project(UltraLowLatencyStreamer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows specific settings
if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00)
endif()

# Find required packages
find_package(PkgConfig QUIET)

# FFmpeg
if(NOT FFMPEG_ROOT)
    set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg" CACHE PATH "FFmpeg root directory")
endif()

if(PkgConfig_FOUND AND NOT WIN32)
    pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale)
else()
    message(STATUS "Using FFMPEG_ROOT: ${FFMPEG_ROOT}")
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_ROOT}/include")
    set(FFMPEG_LIBRARY_DIRS "${FFMPEG_ROOT}/lib")
    set(FFMPEG_LIBRARIES avcodec avformat avutil swscale)
    
    if(NOT EXISTS "${FFMPEG_INCLUDE_DIRS}/libavcodec/avcodec.h")
        message(WARNING "FFmpeg headers not found at ${FFMPEG_INCLUDE_DIRS}")
    endif()
endif()

# libdatachannel for WebRTC
find_package(LibDataChannel QUIET)
if(NOT LibDataChannel_FOUND)
    # Use FetchContent to download libdatachannel
    include(FetchContent)
    FetchContent_Declare(
        libdatachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG v0.20.2
    )
    set(NO_MEDIA OFF CACHE BOOL "" FORCE)
    set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
    set(NO_ICE_TCP OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(libdatachannel)
endif()

# nlohmann_json is included with libdatachannel, no need to fetch separately

# Source files
set(SOURCES
    src/main.cpp
    src/ScreenCapture.cpp
    src/HardwareEncoder.cpp
    src/WebRTCManager.cpp
    src/SignalingClient.cpp
    src/StreamingPipeline.cpp
)

set(HEADERS
    include/ScreenCapture.h
    include/HardwareEncoder.h
    include/WebRTCManager.h
    include/SignalingClient.h
    include/StreamingPipeline.h
    include/StreamCommon.h
)

# Executable
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${FFMPEG_INCLUDE_DIRS}
)

target_link_directories(${PROJECT_NAME} PRIVATE
    ${FFMPEG_LIBRARY_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${FFMPEG_LIBRARIES}
    LibDataChannel::LibDataChannel
    nlohmann_json::nlohmann_json
    # Windows libraries
    d3d11
    dxgi
    ws2_32
)

# Copy required DLLs to output directory (if on Windows)
if(WIN32)
    # FFmpeg DLLs
    if(EXISTS "${FFMPEG_ROOT}/bin")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FFMPEG_ROOT}/bin"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        )
    endif()

    # libdatachannel DLL
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:LibDataChannel::LibDataChannel>"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        # Also copy other dependencies of libdatachannel if they are DLLs
    )

    # OpenSSL DLLs
    if(OPENSSL_FOUND OR OPENSSL_ROOT_DIR)
        set(OPENSSL_BIN_DIR "${OPENSSL_ROOT_DIR}/bin")
        if(EXISTS "${OPENSSL_BIN_DIR}")
            file(GLOB OPENSSL_DLLS "${OPENSSL_BIN_DIR}/*.dll")
            foreach(DLL ${OPENSSL_DLLS})
                add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DLL}"
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
                )
            endforeach()
        endif()
    endif()
endif()

# Installation
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
