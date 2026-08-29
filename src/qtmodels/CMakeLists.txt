# SPDX-License-Identifier: GPL-3.0-only

qt_add_library(
    trackknife_qtmodels STATIC
    src/paged_track_model.cpp
    include/trackknife/qtmodels/paged_track_model.hpp
)
add_library(Trackknife::QtModels ALIAS trackknife_qtmodels)

target_include_directories(
    trackknife_qtmodels
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_qtmodels
    PUBLIC Qt6::Core trackknife_project_options
    PRIVATE Qt6::Concurrent trackknife_project_warnings
)
target_compile_features(trackknife_qtmodels PUBLIC cxx_std_23)
