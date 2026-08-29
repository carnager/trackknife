# SPDX-License-Identifier: GPL-3.0-only

add_library(
    trackknife_titleformat STATIC
    src/compiler.cpp
    src/evaluator.cpp
    src/function_registry.cpp
    src/parser.cpp
)
add_library(Trackknife::TitleFormat ALIAS trackknife_titleformat)

target_include_directories(
    trackknife_titleformat
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_titleformat
    PUBLIC Trackknife::Core trackknife_project_options
    PRIVATE trackknife_project_warnings
)
target_compile_features(trackknife_titleformat PUBLIC cxx_std_23)
