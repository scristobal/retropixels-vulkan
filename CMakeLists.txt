cmake_minimum_required(VERSION 4.4.4)
project(RetroPixelsVulkan VERSION 1.0 LANGUAGES C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Vulkan 1.4.363 REQUIRED)
find_package(glfw3 3.5.1 REQUIRED)

find_program(RETROPIXELS_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/bin")
find_program(RETROPIXELS_GLSLANG NAMES glslangValidator HINTS "$ENV{VULKAN_SDK}/bin")

set(RETROPIXELS_SHADER_COMPILER AUTO CACHE STRING "Shader compiler: AUTO, GLSLC, or GLSLANG")
set_property(CACHE RETROPIXELS_SHADER_COMPILER PROPERTY STRINGS AUTO GLSLC GLSLANG)

if(RETROPIXELS_SHADER_COMPILER STREQUAL "GLSLANG")
    set(RETROPIXELS_GLSLC "")
elseif(RETROPIXELS_SHADER_COMPILER STREQUAL "GLSLC")
    set(RETROPIXELS_GLSLANG "")
elseif(NOT RETROPIXELS_SHADER_COMPILER STREQUAL "AUTO")
    message(FATAL_ERROR "RETROPIXELS_SHADER_COMPILER must be AUTO, GLSLC, or GLSLANG.")
endif()

if(NOT RETROPIXELS_GLSLC AND NOT RETROPIXELS_GLSLANG)
    message(FATAL_ERROR "Install glslc or glslangValidator to compile the Vulkan shaders.")
endif()

if(RETROPIXELS_GLSLC)
    execute_process(COMMAND "${RETROPIXELS_GLSLC}" --version
        OUTPUT_VARIABLE SHADER_COMPILER_INFO RESULT_VARIABLE SHADER_COMPILER_STATUS)
    string(REGEX MATCH "[0-9]+\\.[0-9]+" SHADER_COMPILER_VERSION "${SHADER_COMPILER_INFO}")

    if(NOT SHADER_COMPILER_STATUS EQUAL 0 OR NOT SHADER_COMPILER_VERSION OR SHADER_COMPILER_VERSION VERSION_LESS 2026.4)
        message(FATAL_ERROR "glslc 2026.4 or newer is required.")
    endif()
else()
    execute_process(COMMAND "${RETROPIXELS_GLSLANG}" --version
        OUTPUT_VARIABLE SHADER_COMPILER_INFO RESULT_VARIABLE SHADER_COMPILER_STATUS)
    string(REGEX MATCH "Glslang Version: [0-9]+:([0-9]+\\.[0-9]+\\.[0-9]+)" SHADER_COMPILER_MATCH "${SHADER_COMPILER_INFO}")
    set(SHADER_COMPILER_VERSION "${CMAKE_MATCH_1}")

    if(NOT SHADER_COMPILER_STATUS EQUAL 0 OR NOT SHADER_COMPILER_VERSION OR SHADER_COMPILER_VERSION VERSION_LESS 16.6.0)
        message(FATAL_ERROR "glslangValidator 16.6.0 or newer is required.")
    endif()
endif()

message(STATUS "Shader compiler version: ${SHADER_COMPILER_VERSION}; target Vulkan 1.4 / SPIR-V 1.6")

set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUTPUT_DIR}")

foreach(STAGE vert frag)
    set(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sprite.${STAGE}")
    set(OUTPUT "${SHADER_OUTPUT_DIR}/sprite.${STAGE}.spv")

    if(RETROPIXELS_GLSLC)
        set(SHADER_COMMAND "${RETROPIXELS_GLSLC}" --target-env=vulkan1.4 --target-spv=spv1.6 "${SOURCE}" -o "${OUTPUT}")
    else()
        set(SHADER_COMMAND "${RETROPIXELS_GLSLANG}" -V --target-env vulkan1.4 --target-env spirv1.6 "${SOURCE}" -o "${OUTPUT}")
    endif()

    add_custom_command(OUTPUT "${OUTPUT}"
        COMMAND ${SHADER_COMMAND}
        DEPENDS "${SOURCE}"
        COMMENT "Compiling sprite.${STAGE} to SPIR-V"
        VERBATIM)
    list(APPEND SHADER_OUTPUTS "${OUTPUT}")
endforeach()

add_custom_target(RetroPixelsShaders DEPENDS ${SHADER_OUTPUTS})

add_executable(RetroPixels src/main.c src/renderer.c src/image.c)
add_dependencies(RetroPixels RetroPixelsShaders)

target_compile_definitions(RetroPixels PRIVATE
    RETROPIXELS_SHADERS_DIR="${SHADER_OUTPUT_DIR}"
    RETROPIXELS_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")

target_include_directories(RetroPixels SYSTEM PRIVATE external/stb)
target_link_libraries(RetroPixels PRIVATE Vulkan::Vulkan glfw)
if(UNIX)
    target_link_libraries(RetroPixels PRIVATE m)
endif()

if(MSVC)
    target_compile_options(RetroPixels PRIVATE /W4)
else()
    target_compile_options(RetroPixels PRIVATE -Wall -Wextra -Wpedantic)
endif()
