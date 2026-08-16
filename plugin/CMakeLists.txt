cmake_minimum_required(VERSION 3.16)

project(BubbleRings
    VERSION 0.1.0
    LANGUAGES CXX
)

add_library(BubbleRings SHARED
    BubbleRings.cpp
    PluginInterface.h
    exports.def
)

set_target_properties(BubbleRings PROPERTIES PREFIX "" OUTPUT_NAME BCRings)

target_compile_features(BubbleRings PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(BubbleRings PRIVATE /W4 /EHsc)
else()
    target_compile_options(BubbleRings PRIVATE -Wall -Wextra -Wpedantic -m32)
    target_link_options(BubbleRings PRIVATE -m32 -static -static-libgcc -static-libstdc++)
endif()
