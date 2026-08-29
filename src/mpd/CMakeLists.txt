# SPDX-License-Identifier: GPL-3.0-only

add_library(
    trackknife_mpd STATIC
    src/client.cpp
    src/model.cpp
    src/music_root.cpp
    src/projection.cpp
    src/session.cpp
)
add_library(Trackknife::Mpd ALIAS trackknife_mpd)

target_include_directories(
    trackknife_mpd
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_mpd
    PUBLIC Trackknife::Core trackknife_project_options
    PRIVATE PkgConfig::MPDCLIENT trackknife_project_warnings
)
target_compile_features(trackknife_mpd PUBLIC cxx_std_23)
