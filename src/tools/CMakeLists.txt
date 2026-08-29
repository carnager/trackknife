# SPDX-License-Identifier: GPL-3.0-only

add_executable(trackknife_titleformat_cli titleformat_cli.cpp)
target_link_libraries(
    trackknife_titleformat_cli
    PRIVATE Trackknife::TitleFormat trackknife_project_options trackknife_project_warnings
)
set_target_properties(trackknife_titleformat_cli PROPERTIES OUTPUT_NAME trackknife-titleformat)

add_executable(trackknife_mpd_probe mpd_probe.cpp)
target_link_libraries(
    trackknife_mpd_probe
    PRIVATE Trackknife::Mpd trackknife_project_options trackknife_project_warnings
)
set_target_properties(trackknife_mpd_probe PROPERTIES OUTPUT_NAME trackknife-mpd-probe)
