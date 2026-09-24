cmake_minimum_required(VERSION 3.24)

project(SlugVulkan VERSION 0.1.0 LANGUAGES C CXX)

include(cmake/SlugUI.cmake)

option(SLUGVK_BUILD_EXAMPLE "Build the kitchen-sink example" ${PROJECT_IS_TOP_LEVEL})
option(SLUGVK_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(SLUGVK_BUILD_BENCHMARKS "Build CPU microbenchmarks" OFF)
option(SLUGVK_ENABLE_VALIDATION "Enable Vulkan validation in debug builds" ON)
option(SLUGVK_STATIC_MSVC_RUNTIME "Link the standalone MSVC build against the static runtime"
       ${PROJECT_IS_TOP_LEVEL})
option(SLUGVK_INSTALL "Install the raw static library and public headers" ${PROJECT_IS_TOP_LEVEL})
option(SLUGVK_ENABLE_GLFW "Build the optional GLFW Window adapter" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC AND SLUGVK_STATIC_MSVC_RUNTIME)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Vulkan REQUIRED)

if(SLUGVK_ENABLE_GLFW)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(external/glfw)
endif()

set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
add_subdirectory(external/freetype)

# SlugVulkan keeps text shaping separate from outline extraction. HarfBuzz owns GSUB/GPOS,
# script/cluster shaping, while FreeType + Slughorn own exact outline decomposition and Slug
# coverage. Build only the HarfBuzz core; raster/vector/GPU/subset utilities are unrelated here.
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_BUILD_RASTER OFF CACHE BOOL "" FORCE)
set(HB_BUILD_VECTOR OFF CACHE BOOL "" FORCE)
set(HB_BUILD_GPU OFF CACHE BOOL "" FORCE)
set(HB_BUILD_GPU_DEMO OFF CACHE STRING "" FORCE)
set(HB_HAVE_FREETYPE OFF CACHE BOOL "" FORCE)
set(HB_HAVE_CAIRO OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GLIB OFF CACHE BOOL "" FORCE)
set(HB_HAVE_ICU OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GOBJECT OFF CACHE BOOL "" FORCE)
add_subdirectory(external/harfbuzz)
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
slugvk_compile_shader(shaders/primitive.vert)
slugvk_compile_shader(shaders/primitive.frag)
set(SLUGVK_EMBEDDED_SHADER_HEADER "${SLUGVK_GENERATED_DIR}/slugvk_embedded_shaders.hpp")
list(GET SLUGVK_SHADER_OUTPUTS 0 SLUGVK_VERTEX_SPV)
list(GET SLUGVK_SHADER_OUTPUTS 1 SLUGVK_FRAGMENT_SPV)
list(GET SLUGVK_SHADER_OUTPUTS 2 SLUGVK_PRIMITIVE_VERTEX_SPV)
list(GET SLUGVK_SHADER_OUTPUTS 3 SLUGVK_PRIMITIVE_FRAGMENT_SPV)
add_custom_command(
  OUTPUT "${SLUGVK_EMBEDDED_SHADER_HEADER}"
  COMMAND "${CMAKE_COMMAND}"
    -DVERTEX_SPV=${SLUGVK_VERTEX_SPV}
    -DFRAGMENT_SPV=${SLUGVK_FRAGMENT_SPV}
    -DPRIMITIVE_VERTEX_SPV=${SLUGVK_PRIMITIVE_VERTEX_SPV}
    -DPRIMITIVE_FRAGMENT_SPV=${SLUGVK_PRIMITIVE_FRAGMENT_SPV}
    -DOUTPUT=${SLUGVK_EMBEDDED_SHADER_HEADER}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
  DEPENDS ${SLUGVK_SHADER_OUTPUTS} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
  VERBATIM
)
add_custom_target(slugvk_shaders DEPENDS "${SLUGVK_EMBEDDED_SHADER_HEADER}")

set(SLUGVK_SOURCES
  src/animation.cpp
  src/draw_list.cpp
  src/input.cpp
  src/render_device.cpp
  src/slugui.cpp
  src/ui.cpp
  src/vector_atlas.cpp
  src/vulkan_renderer.cpp
  "${SLUGVK_EMBEDDED_SHADER_HEADER}"
)
if(SLUGVK_ENABLE_GLFW)
  list(APPEND SLUGVK_SOURCES src/window.cpp)
endif()
if(WIN32)
  list(APPEND SLUGVK_SOURCES src/win32_surface.cpp)
endif()
add_library(slugvk STATIC ${SLUGVK_SOURCES})
add_library(SlugVulkan::slugvk ALIAS slugvk)
add_dependencies(slugvk slugvk_shaders)
target_include_directories(slugvk PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                  PRIVATE "${SLUGVK_GENERATED_DIR}")
target_link_libraries(slugvk PUBLIC slughorn Vulkan::Vulkan PRIVATE harfbuzz)
if(SLUGVK_ENABLE_GLFW)
  target_link_libraries(slugvk PRIVATE glfw)
endif()
if(WIN32)
  target_link_libraries(slugvk PRIVATE user32)
endif()
target_compile_definitions(slugvk PUBLIC SLUGVK_ENABLE_GLFW=$<BOOL:${SLUGVK_ENABLE_GLFW}>)
target_compile_definitions(slugvk PRIVATE
  $<$<BOOL:${SLUGVK_ENABLE_VALIDATION}>:SLUGVK_ENABLE_VALIDATION=1>
)

if(MSVC)
  # /FS serializes PDB writes inside cl.exe. The example and generated Figma bridge are compiled
  # concurrently during low-latency imports, so without it MSBuild can fail with C1041.
  target_compile_options(slugvk PRIVATE /W4 /permissive- /Zc:__cplusplus /FS)
else()
  target_compile_options(slugvk PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SLUGVK_BUILD_EXAMPLE AND NOT SLUGVK_ENABLE_GLFW)
  message(FATAL_ERROR "SLUGVK_BUILD_EXAMPLE requires SLUGVK_ENABLE_GLFW=ON")
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
  set(SLUGVK_SLUGUI_DEMO_STAMP "${SLUGVK_SLUGUI_DEMO_HEADER}.stamp")
  set(SLUGVK_FIGMA_DEMO_STAMP "${SLUGVK_FIGMA_DEMO_HEADER}.stamp")
  slugvk_compile_slugui(
    STAMPED
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/examples/slugui_demo.slugui"
    OUTPUT "${SLUGVK_SLUGUI_DEMO_HEADER}"
    NAMESPACE slugvk::example)
  slugvk_compile_slugui(
    STAMPED
    STRIP_SOURCE_METADATA
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/examples/figma_group_31.slugui"
    OUTPUT "${SLUGVK_FIGMA_DEMO_HEADER}"
    NAMESPACE slugvk::example
    CLASS_NAME Group_31Generated)
  add_custom_target(slugvk_slugui_example_codegen DEPENDS
    "${SLUGVK_SLUGUI_DEMO_STAMP}"
    "${SLUGVK_FIGMA_DEMO_STAMP}")
  add_executable(slugvk_example MACOSX_BUNDLE
    examples/kitchen_sink.cpp
    examples/figma_import_bridge.cpp
    "${SLUGVK_GEIST_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/OFL-Geist.txt")
  # Figma AOT output can contain thousands of construction statements. Keep that generated
  # code isolated in one tiny translation unit and compile that bridge without optimization.
  # The generator scopes every child subtree so moved temporaries do not accumulate in one huge
  # /Od stack frame; this gives fast iterative builds without the former stack-overflow hazard.
  if(MSVC)
    target_compile_options(slugvk_example PRIVATE /FS)
    set_source_files_properties(examples/figma_import_bridge.cpp
      PROPERTIES COMPILE_OPTIONS "/Od;/Ob0;/bigobj")
  else()
    set_source_files_properties(examples/figma_import_bridge.cpp
      PROPERTIES COMPILE_OPTIONS "-O0")
  endif()
  target_link_libraries(slugvk_example PRIVATE SlugVulkan::slugvk)
  target_include_directories(slugvk_example PRIVATE "${SLUGVK_GENERATED_DIR}")
  # The generated Figma header is a BYPRODUCT of the stamp command. Make the bridge object's
  # dependency explicit so Visual Studio cannot reuse an object built against stale generated UI.
  set_property(SOURCE examples/figma_import_bridge.cpp APPEND PROPERTY
    OBJECT_DEPENDS "${SLUGVK_FIGMA_DEMO_HEADER}")
  target_compile_definitions(slugvk_example PRIVATE
    SLUGVK_DEVELOPMENT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    SLUGVK_DEVELOPMENT_BUILD_DIR="${CMAKE_BINARY_DIR}"
    SLUGVK_DEVELOPMENT_CONFIG="$<CONFIG>"
    SLUGVK_CMAKE_COMMAND="${CMAKE_COMMAND}"
    SLUGVK_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
    SLUGVK_FIGMA_GENERATED_HEADER="${SLUGVK_FIGMA_DEMO_HEADER}"
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
  add_executable(slugvk_stress_tests tests/stress_tests.cpp)
  target_link_libraries(slugvk_stress_tests PRIVATE SlugVulkan::slugvk)
  add_test(NAME slugui_stress COMMAND slugvk_stress_tests)
  add_executable(slugvk_fuzz_tests tests/fuzz_tests.cpp)
  target_link_libraries(slugvk_fuzz_tests PRIVATE SlugVulkan::slugvk)
  add_test(NAME slugvk_fuzz COMMAND slugvk_fuzz_tests)
  add_executable(slugvk_golden_tests tests/golden_tests.cpp)
  target_link_libraries(slugvk_golden_tests PRIVATE SlugVulkan::slugvk)
  add_test(NAME slugui_golden COMMAND slugvk_golden_tests)
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

if(SLUGVK_BUILD_BENCHMARKS)
  add_executable(slugvk_cpu_benchmarks benchmarks/cpu_benchmarks.cpp)
  target_link_libraries(slugvk_cpu_benchmarks PRIVATE SlugVulkan::slugvk)
endif()

if(SLUGVK_INSTALL)
  # slugvk is a static archive, so its static implementation dependencies must travel with a raw
  # install as archives too. FreeType and HarfBuzz install themselves from their subdirectories;
  # Slughorn and the optional GLFW adapter are owned/configured here.
  install(TARGETS slugvk slughorn
    ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
  if(SLUGVK_ENABLE_GLFW)
    install(TARGETS glfw
      ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
  endif()
  install(DIRECTORY include/ DESTINATION include)
  install(DIRECTORY external/slughorn/slughorn/ DESTINATION include/slughorn
    FILES_MATCHING PATTERN "*.hpp")
  install(PROGRAMS tools/slugui_compiler.py DESTINATION bin)
  install(FILES tools/svg_path.py DESTINATION bin)
  install(FILES cmake/SlugUI.cmake DESTINATION share/SlugVulkan/cmake)
  install(DIRECTORY tools/figma-slugui DESTINATION share/SlugVulkan)
endif()
