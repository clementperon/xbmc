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
