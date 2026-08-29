if(NOT DEFINED BUILD_DIR OR NOT DEFINED SOURCE_DIR OR NOT DEFINED CONFIG OR
   NOT DEFINED EXECUTABLE_PATH)
  message(FATAL_ERROR "BUILD_DIR, SOURCE_DIR, CONFIG, and EXECUTABLE_PATH are required")
endif()

# A dropped file may carry its original timestamp on Windows/macOS. This is a derived artifact,
# so removing only this exact header guarantees that the AOT compiler consumes the staged source.
file(REMOVE "${BUILD_DIR}/generated/figma_group_31.generated.hpp")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env VSLANG=1033
          "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --config "${CONFIG}"
          --target slugvk_example --parallel
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error
  ENCODING UTF-8
)
file(WRITE "${BUILD_DIR}/slugui_drop_rebuild.log" "${build_output}\n${build_error}")

if(build_result EQUAL 0)
  file(REMOVE "${SOURCE_DIR}/examples/figma_group_31.slugui.drop-backup")
  set(import_result --import-success)
else()
  if(EXISTS "${SOURCE_DIR}/examples/figma_group_31.slugui.drop-backup")
    file(COPY_FILE
      "${SOURCE_DIR}/examples/figma_group_31.slugui.drop-backup"
      "${SOURCE_DIR}/examples/figma_group_31.slugui"
      ONLY_IF_DIFFERENT)
    file(REMOVE "${SOURCE_DIR}/examples/figma_group_31.slugui.drop-backup")
  endif()
  set(import_result --import-failed)
endif()

if(NO_RELAUNCH)
  return()
endif()

if(WIN32)
  execute_process(
    COMMAND cmd.exe /D /C start "" "${EXECUTABLE_PATH}" --figma ${import_result}
  )
elseif(APPLE)
  get_filename_component(macos_directory "${EXECUTABLE_PATH}" DIRECTORY)
  get_filename_component(contents_directory "${macos_directory}" DIRECTORY)
  get_filename_component(bundle_directory "${contents_directory}" DIRECTORY)
  execute_process(
    COMMAND open -n "${bundle_directory}" --args --figma ${import_result}
  )
else()
  message(WARNING "Automatic relaunch is only implemented for Windows and macOS")
endif()
