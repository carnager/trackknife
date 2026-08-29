# SPDX-License-Identifier: GPL-3.0-only

qt_add_library(
    trackknife_mpd_ui STATIC
    mpd_browser_model.cpp
    mpd_browser_model.hpp
    mpd_probe_controller.cpp
    mpd_probe_controller.hpp
    mpd_output_model.cpp
    mpd_output_model.hpp
    mpd_queue_model.cpp
    mpd_queue_model.hpp
    mpd_search_result_model.cpp
    mpd_search_result_model.hpp
)
add_library(Trackknife::MpdUi ALIAS trackknife_mpd_ui)

target_include_directories(trackknife_mpd_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(
    trackknife_mpd_ui
    PUBLIC Qt6::Core Qt6::Gui Trackknife::Mpd trackknife_project_options
    PRIVATE Qt6::Concurrent trackknife_project_warnings
)
target_compile_features(trackknife_mpd_ui PUBLIC cxx_std_23)
