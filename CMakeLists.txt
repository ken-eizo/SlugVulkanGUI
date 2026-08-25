cmake_minimum_required(VERSION 3.24)

project(SlugVulkan VERSION 0.1.0 LANGUAGES C CXX)

option(SLUGVK_BUILD_EXAMPLE "Build the kitchen-sink example" ON)
option(SLUGVK_BUILD_TESTS "Build unit tests" ON)
option(SLUGVK_ENABLE_VALIDATION "Enable Vulkan validation in debug builds" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(Vulkan REQUIRED)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(external/glfw)

set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
add_subdirectory(external/freetype)
if(NOT TARGET Freetype::Freetype)
  add_library(Freetype::Freetype ALIAS freetype-interface)
endif()

# Build only the slughorn core + FreeType bridge that SlugVulkan uses. Keeping this target here
# lets external/slughorn stay an unmodified, reproducible upstream submodule.
add_library(slughorn STATIC external/slughorn/slughorn/slughorn.cpp)
target_include_directories(slughorn SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/external/slughorn")
target_compile_features(slughorn PUBLIC cxx_std_20)
target_compile_definitions(slughorn PRIVATE SLUGHORN_FREETYPE_IMPLEMENTATION PUBLIC SLUGHORN_HAS_FREETYPE)
target_link_libraries(slughorn PUBLIC Freetype::Freetype)
set_target_properties(slughorn PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(slughorn PRIVATE /W4 /wd4100)
else()
  target_compile_options(slughorn PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

set(SLUGVK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SLUGVK_SHADER_DIR "${SLUGVK_GENERATED_DIR}/shaders")
file(MAKE_DIRECTORY "${SLUGVK_SHADER_DIR}")

if(NOT Vulkan_GLSLC_EXECUTABLE)
  find_program(Vulkan_GLSLC_EXECUTABLE NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
endif()
if(NOT Vulkan_GLSLC_EXECUTABLE)
  message(FATAL_ERROR "glslc was not found. Install the Vulkan SDK or set Vulkan_GLSLC_EXECUTABLE.")
endif()

function(slugvk_compile_shader source)
  get_filename_component(name "${source}" NAME)
  set(output "${SLUGVK_SHADER_DIR}/${name}.spv")
  add_custom_command(
    OUTPUT "${output}"
    COMMAND "${Vulkan_GLSLC_EXECUTABLE}" --target-env=vulkan1.1 -O "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -o "${output}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
    VERBATIM
  )
  set(SLUGVK_SHADER_OUTPUTS ${SLUGVK_SHADER_OUTPUTS} "${output}" PARENT_SCOPE)
endfunction()

slugvk_compile_shader(shaders/vector.vert)
slugvk_compile_shader(shaders/vector.frag)
set(SLUGVK_EMBEDDED_SHADER_HEADER "${SLUGVK_GENERATED_DIR}/slugvk_embedded_shaders.hpp")
list(GET SLUGVK_SHADER_OUTPUTS 0 SLUGVK_VERTEX_SPV)
list(GET SLUGVK_SHADER_OUTPUTS 1 SLUGVK_FRAGMENT_SPV)
add_custom_command(
  OUTPUT "${SLUGVK_EMBEDDED_SHADER_HEADER}"
  COMMAND "${CMAKE_COMMAND}"
    -DVERTEX_SPV=${SLUGVK_VERTEX_SPV}
    -DFRAGMENT_SPV=${SLUGVK_FRAGMENT_SPV}
    -DOUTPUT=${SLUGVK_EMBEDDED_SHADER_HEADER}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
  DEPENDS ${SLUGVK_SHADER_OUTPUTS} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
  VERBATIM
)
add_custom_target(slugvk_shaders DEPENDS "${SLUGVK_EMBEDDED_SHADER_HEADER}")

add_library(slugvk STATIC
  src/animation.cpp
  src/draw_list.cpp
  src/input.cpp
  src/ui.cpp
  src/vector_atlas.cpp
  src/window.cpp
  src/vulkan_renderer.cpp
  "${SLUGVK_EMBEDDED_SHADER_HEADER}"
)
add_dependencies(slugvk slugvk_shaders)
target_include_directories(slugvk PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                  PRIVATE "${SLUGVK_GENERATED_DIR}")
target_link_libraries(slugvk PUBLIC slughorn PRIVATE Vulkan::Vulkan glfw)
target_compile_definitions(slugvk PRIVATE
  $<$<BOOL:${SLUGVK_ENABLE_VALIDATION}>:SLUGVK_ENABLE_VALIDATION=1>
)

if(MSVC)
  target_compile_options(slugvk PRIVATE /W4 /permissive- /Zc:__cplusplus)
else()
  target_compile_options(slugvk PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SLUGVK_BUILD_EXAMPLE)
  add_executable(slugvk_example MACOSX_BUNDLE examples/kitchen_sink.cpp)
  target_link_libraries(slugvk_example PRIVATE slugvk)
  add_dependencies(slugvk_example slugvk_shaders)
  set_target_properties(slugvk_example PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "SlugVulkanGUI Example"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.slugvulkan.example"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
endif()

if(SLUGVK_BUILD_TESTS)
  enable_testing()
  add_executable(slugvk_tests tests/core_tests.cpp)
  target_link_libraries(slugvk_tests PRIVATE slugvk)
  add_test(NAME slugvk_core COMMAND slugvk_tests)
endif()

install(TARGETS slugvk ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)
