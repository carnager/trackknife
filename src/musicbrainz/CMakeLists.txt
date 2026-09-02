# SPDX-License-Identifier: GPL-3.0-only

qt_add_library(
    trackknife_musicbrainz STATIC
    src/client.cpp
    src/matching.cpp
    src/proposal_bridge.cpp
    src/web_service.cpp
    include/trackknife/musicbrainz/client.hpp
    include/trackknife/musicbrainz/matching.hpp
    include/trackknife/musicbrainz/proposal_bridge.hpp
    include/trackknife/musicbrainz/web_service.hpp
)
add_library(Trackknife::MusicBrainz ALIAS trackknife_musicbrainz)

target_include_directories(
    trackknife_musicbrainz
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_musicbrainz
    PUBLIC Trackknife::Core Trackknife::Metadata Qt6::Core trackknife_project_options
    PRIVATE Qt6::Network trackknife_project_warnings
)
target_compile_features(trackknife_musicbrainz PUBLIC cxx_std_23)
