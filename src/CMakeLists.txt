# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

# shared/ holds link_protocol.h, also used by the composite daughter firmware
idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/shared")
