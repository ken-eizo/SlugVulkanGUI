cmake_minimum_required(VERSION 3.24)

project(SlugVulkan VERSION 0.1.0 LANGUAGES C CXX)

include(cmake/SlugUI.cmake)

option(SLUGVK_BUILD_EXAMPLE "Build the kitchen-sink example" ${PROJECT_IS_TOP_LEVEL})
option(SLUGVK_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(SLUGVK_ENABLE_VALIDATION "Enable Vulkan validation in debug builds" ON)
option(SLUGVK_STATIC_MSVC_RUNTIME "Link the standalone MSVC build against the static runtime"
       ${PROJECT_IS_TOP_LEVEL})
option(SLUGVK_INSTALL "Install the raw static library and public headers" ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC AND SLUGVK_STATIC_MSVC_RUNTIME)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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

# Build only the Slughorn core and FreeType bridge used by SlugVulkan.
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
  src/slugui.cpp
  src/ui.cpp
  src/vector_atlas.cpp
  src/window.cpp
  src/vulkan_renderer.cpp
  "${SLUGVK_EMBEDDED_SHADER_HEADER}"
)
add_library(SlugVulkan::slugvk ALIAS slugvk)
add_dependencies(slugvk slugvk_shaders)
target_include_directories(slugvk PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                  PRIVATE "${SLUGVK_GENERATED_DIR}")
target_link_libraries(slugvk PUBLIC slughorn Vulkan::Vulkan PRIVATE glfw)
target_compile_definitions(slugvk PRIVATE
  $<$<BOOL:${SLUGVK_ENABLE_VALIDATION}>:SLUGVK_ENABLE_VALIDATION=1>
)

if(MSVC)
  target_compile_options(slugvk PRIVATE /W4 /permissive- /Zc:__cplusplus)
else()
  target_compile_options(slugvk PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SLUGVK_BUILD_EXAMPLE)
  set(SLUGVK_GEIST_FONT "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Geist[wght].ttf")
  set(SLUGVK_GEIST_HEADER "${SLUGVK_GENERATED_DIR}/geist_font.generated.hpp")
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_custom_command(
    OUTPUT "${SLUGVK_GEIST_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_binary.py"
      "${SLUGVK_GEIST_FONT}" "${SLUGVK_GEIST_HEADER}"
      --namespace slugvk::example::assets --symbol geistFont
    DEPENDS "${SLUGVK_GEIST_FONT}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_binary.py"
    VERBATIM)
  add_custom_target(slugvk_geist_font DEPENDS "${SLUGVK_GEIST_HEADER}")
  set(SLUGVK_SLUGUI_DEMO_HEADER "${SLUGVK_GENERATED_DIR}/slugui_demo.generated.hpp")
  set(SLUGVK_FIGMA_DEMO_HEADER "${SLUGVK_GENERATED_DIR}/figma_group_31.generated.hpp")
  slugvk_compile_slugui(
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/examples/slugui_demo.slugui"
    OUTPUT "${SLUGVK_SLUGUI_DEMO_HEADER}"
    NAMESPACE slugvk::example)
  slugvk_compile_slugui(
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/examples/figma_group_31.slugui"
    OUTPUT "${SLUGVK_FIGMA_DEMO_HEADER}"
    NAMESPACE slugvk::example
    CLASS_NAME Group_31Generated)
  add_custom_target(slugvk_slugui_example_codegen DEPENDS
    "${SLUGVK_SLUGUI_DEMO_HEADER}"
    "${SLUGVK_FIGMA_DEMO_HEADER}")
  add_executable(slugvk_example MACOSX_BUNDLE
    examples/kitchen_sink.cpp
    "${SLUGVK_GEIST_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/OFL-Geist.txt")
  target_link_libraries(slugvk_example PRIVATE SlugVulkan::slugvk)
  target_include_directories(slugvk_example PRIVATE "${SLUGVK_GENERATED_DIR}")
  target_compile_definitions(slugvk_example PRIVATE
    SLUGVK_DEVELOPMENT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    SLUGVK_DEVELOPMENT_BUILD_DIR="${CMAKE_BINARY_DIR}"
    SLUGVK_DEVELOPMENT_CONFIG="$<CONFIG>"
    SLUGVK_CMAKE_COMMAND="${CMAKE_COMMAND}"
    SLUGVK_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
  )
  add_dependencies(slugvk_example slugvk_shaders slugvk_slugui_example_codegen slugvk_geist_font)
  set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/OFL-Geist.txt"
    PROPERTIES HEADER_FILE_ONLY TRUE MACOSX_PACKAGE_LOCATION Resources)
  set_target_properties(slugvk_example PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "SlugVulkanGUI Example"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.slugvulkan.example"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
endif()

if(SLUGVK_BUILD_TESTS)
  enable_testing()
  add_executable(slugvk_tests tests/core_tests.cpp)
  target_link_libraries(slugvk_tests PRIVATE SlugVulkan::slugvk)
  add_test(NAME slugvk_core COMMAND slugvk_tests)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME slugui_compiler
    COMMAND "${Python3_EXECUTABLE}" -m unittest
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/slugui_compiler_test.py" -v)
  set_tests_properties(slugui_compiler PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  find_program(SLUGVK_NODE_EXECUTABLE NAMES node)
  if(SLUGVK_NODE_EXECUTABLE)
    add_test(NAME slugui_figma_exporter
      COMMAND "${SLUGVK_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/figma_plugin_test.js")
    set_tests_properties(slugui_figma_exporter PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      ENVIRONMENT "SLUGUI_PYTHON=${Python3_EXECUTABLE}")
  else()
    message(WARNING "Node.js was not found; the Figma exporter integration test is disabled.")
  endif()
endif()

if(SLUGVK_INSTALL)
  install(TARGETS slugvk ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
  install(DIRECTORY include/ DESTINATION include)
  install(PROGRAMS tools/slugui_compiler.py DESTINATION bin)
  install(FILES tools/svg_path.py DESTINATION bin)
  install(FILES cmake/SlugUI.cmake DESTINATION share/SlugVulkan/cmake)
  install(DIRECTORY tools/figma-slugui DESTINATION share/SlugVulkan)
endif()
