cmake_minimum_required(VERSION 3.16)

project(GEORings
    VERSION 2.0.2
    LANGUAGES CXX
)

add_library(_GEORings SHARED
    GEORingsModule.cpp
    exports.def
)

# SceneHook.h must be byte-identical to the copy every other participant
# builds against.
target_include_directories(_GEORings PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../SceneHook")

set_target_properties(_GEORings PROPERTIES
    PREFIX ""
    OUTPUT_NAME "_GEORings46"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../libs"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../libs"
)

target_compile_features(_GEORings PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(_GEORings PRIVATE /W4 /EHsc)
else()
    target_compile_options(_GEORings PRIVATE -Wall -Wextra -Wpedantic -m32)
    target_link_options(_GEORings PRIVATE -m32 -static -static-libgcc -static-libstdc++)
    target_link_libraries(_GEORings PRIVATE gdi32)
endif()
