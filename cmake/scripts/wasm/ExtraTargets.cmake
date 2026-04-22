# Stage a Tizen web app wrapper around wasm build artifacts.
set(WASM_TIZEN_TEMPLATE_DIR "${CMAKE_SOURCE_DIR}/tools/wasm/tizen")
set(WASM_TIZEN_STAGE_DIR "${CMAKE_BINARY_DIR}/packaging/tizen")

add_custom_target(package_tizen
  COMMAND ${CMAKE_COMMAND} -E make_directory "${WASM_TIZEN_STAGE_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${WASM_TIZEN_TEMPLATE_DIR}/config.xml"
          "${WASM_TIZEN_TEMPLATE_DIR}/index.html"
          "${WASM_TIZEN_TEMPLATE_DIR}/tizen_web_project.yaml"
          "${WASM_TIZEN_STAGE_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_BINARY_DIR}/${APP_NAME_LC}.js"
          "${CMAKE_BINARY_DIR}/${APP_NAME_LC}.wasm"
          "${CMAKE_BINARY_DIR}/${APP_NAME_LC}.data"
          "${WASM_TIZEN_STAGE_DIR}"
  DEPENDS ${APP_NAME_LC}
  COMMENT "Staging Tizen web app in ${WASM_TIZEN_STAGE_DIR}"
  VERBATIM
)

set_target_properties(package_tizen PROPERTIES FOLDER "Build Utilities")

# Resolve Tizen CLI (tz) from environment-driven SDK paths first.
find_program(WASM_TIZEN_TZ_EXECUTABLE
  NAMES tz
  HINTS
    "$ENV{TIZEN_CLI_PATH}"
    "$ENV{TIZEN_TOOLS_PATH}/tizen-core"
    "$ENV{TIZEN_SDK}/tools/tizen-core"
    "$ENV{TIZEN_SDK_ROOT}/tools/tizen-core"
  NO_DEFAULT_PATH
)

# Fallback to PATH lookup if env-based lookup did not resolve tz.
if(NOT WASM_TIZEN_TZ_EXECUTABLE)
  find_program(WASM_TIZEN_TZ_EXECUTABLE NAMES tz)
endif()

if(WASM_TIZEN_TZ_EXECUTABLE)
  set(WASM_TIZEN_INSTALL_TARGET "$ENV{TIZEN_TARGET_NAME}" CACHE STRING
      "Tizen target device name used by install_tizen_wgt (tz --target)")
  set(WASM_TIZEN_INSTALL_SERIAL "$ENV{TIZEN_TARGET_SERIAL}" CACHE STRING
      "Tizen device serial used by install_tizen_wgt (tz --serial)")

  add_custom_target(package_tizen_wgt
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${WASM_TIZEN_STAGE_DIR}/Debug"
    COMMAND "${WASM_TIZEN_TZ_EXECUTABLE}" pack -w "${WASM_TIZEN_STAGE_DIR}" -t wgt
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${WASM_TIZEN_STAGE_DIR}/Debug/projects"
    DEPENDS package_tizen
    COMMENT "Packaging Tizen WGT via ${WASM_TIZEN_TZ_EXECUTABLE}"
    VERBATIM
  )
  set_target_properties(package_tizen_wgt PROPERTIES FOLDER "Build Utilities")

  if(WASM_TIZEN_INSTALL_TARGET)
    add_custom_target(install_tizen_wgt
      COMMAND "${WASM_TIZEN_TZ_EXECUTABLE}" install
              --package-path "${WASM_TIZEN_STAGE_DIR}/Debug/tizen.wgt"
              --target "${WASM_TIZEN_INSTALL_TARGET}"
      DEPENDS package_tizen_wgt
      COMMENT "Installing Tizen WGT to target ${WASM_TIZEN_INSTALL_TARGET}"
      VERBATIM
    )
  elseif(WASM_TIZEN_INSTALL_SERIAL)
    add_custom_target(install_tizen_wgt
      COMMAND "${WASM_TIZEN_TZ_EXECUTABLE}" install
              --package-path "${WASM_TIZEN_STAGE_DIR}/Debug/tizen.wgt"
              --serial "${WASM_TIZEN_INSTALL_SERIAL}"
      DEPENDS package_tizen_wgt
      COMMENT "Installing Tizen WGT to serial ${WASM_TIZEN_INSTALL_SERIAL}"
      VERBATIM
    )
  else()
    add_custom_target(install_tizen_wgt
      COMMAND ${CMAKE_COMMAND} -E echo
              "WASM: install_tizen_wgt requires WASM_TIZEN_INSTALL_TARGET or WASM_TIZEN_INSTALL_SERIAL."
      COMMAND ${CMAKE_COMMAND} -E echo
              "Set cache vars or env vars TIZEN_TARGET_NAME / TIZEN_TARGET_SERIAL, then reconfigure."
      COMMAND ${CMAKE_COMMAND} -E false
      DEPENDS package_tizen_wgt
      COMMENT "Missing install target/serial configuration"
      VERBATIM
    )
  endif()
  set_target_properties(install_tizen_wgt PROPERTIES FOLDER "Build Utilities")
else()
  message(STATUS "WASM: tz CLI not found. Set TIZEN_CLI_PATH/TIZEN_TOOLS_PATH/TIZEN_SDK/TIZEN_SDK_ROOT to enable package_tizen_wgt.")
endif()
