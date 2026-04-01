cmake_minimum_required(VERSION 3.16)

file(GLOB_RECURSE SAMPLER_SRCS "src/*.cpp")
add_library(crosspad-sampler ${SAMPLER_SRCS})
target_include_directories(crosspad-sampler PUBLIC include)
target_link_libraries(crosspad-sampler PUBLIC crosspad-core crosspad-gui)
