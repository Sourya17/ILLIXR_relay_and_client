cmake_minimum_required(VERSION 3.16)

set(PLUGIN_NAME plugin.tcp_minimal_backend${ILLIXR_BUILD_SUFFIX})

add_library(${PLUGIN_NAME} SHARED
    plugin.cpp
    plugin.hpp
)

set_target_properties(${PLUGIN_NAME} PROPERTIES OUTPUT_NAME ${PLUGIN_NAME})

target_include_directories(${PLUGIN_NAME} PUBLIC ${ILLIXR_SOURCE_DIR}/include)
target_link_libraries(${PLUGIN_NAME} spdlog::spdlog)
target_compile_features(${PLUGIN_NAME} PRIVATE cxx_std_17)

install(TARGETS ${PLUGIN_NAME} DESTINATION lib)

