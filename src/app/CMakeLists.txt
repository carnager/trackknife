# SPDX-License-Identifier: GPL-3.0-only

qt_add_executable(trackknife main.cpp)

target_link_libraries(
    trackknife
    PRIVATE
        Trackknife::Ui
        Qt6::Widgets
        trackknife_project_options
        trackknife_project_warnings
)
set_target_properties(trackknife PROPERTIES OUTPUT_NAME trackknife)
