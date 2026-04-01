# CrossPad Sampler — ESP-IDF Component CMakeLists.txt
#
# Sampler application module for CrossPad.
# Arduino/PlatformIO builds use library.json instead.

file(GLOB_RECURSE SAMPLER_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

idf_component_register(
    SRCS ${SAMPLER_SRCS}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    REQUIRES
        lvgl
        crosspad-core
        crosspad-gui
)
