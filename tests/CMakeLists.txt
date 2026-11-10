add_executable(gost-restreamer-lifecycle-test unit/plugin-lifecycle-test.cpp fixtures/fake-frontend.hpp)
target_link_libraries(gost-restreamer-lifecycle-test PRIVATE gost-restreamer-core Qt6::Test)
target_include_directories(gost-restreamer-lifecycle-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(gost-restreamer-lifecycle-test PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
add_test(NAME gost-restreamer.lifecycle COMMAND gost-restreamer-lifecycle-test)
set_tests_properties(
  gost-restreamer.lifecycle
  PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${GOST_QT_TEST_ROOT}/plugins/platforms"
    LABELS "offline;gui"
    TIMEOUT 30
)

add_executable(gost-restreamer-backend-readonly-test integration/backend-readonly-test.cpp)
target_compile_features(gost-restreamer-backend-readonly-test PRIVATE cxx_std_20)
target_link_libraries(gost-restreamer-backend-readonly-test PRIVATE gost-restreamer-core Qt6::Network Qt6::Test)
set_target_properties(gost-restreamer-backend-readonly-test PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
add_test(NAME gost-restreamer.backend-readonly COMMAND gost-restreamer-backend-readonly-test)
set_tests_properties(
  gost-restreamer.backend-readonly
  PROPERTIES LABELS "backend;readonly" SKIP_RETURN_CODE 77 TIMEOUT 15
)

set(GOST_TEST_WORKSPACE_DIR "${PROJECT_SOURCE_DIR}/build/test-workspace")
file(MAKE_DIRECTORY "${GOST_TEST_WORKSPACE_DIR}" "${PROJECT_SOURCE_DIR}/build/tmp")

function(gost_add_offline_test name source)
  add_executable(gost-restreamer-${name}-test "${source}")
  target_link_libraries(gost-restreamer-${name}-test PRIVATE gost-restreamer-core Qt6::Test)
  target_include_directories(gost-restreamer-${name}-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  set_target_properties(gost-restreamer-${name}-test PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
  add_test(NAME gost-restreamer.${name} COMMAND gost-restreamer-${name}-test)
  set_tests_properties(gost-restreamer.${name} PROPERTIES LABELS "offline" TIMEOUT 120)
endfunction()

gost_add_offline_test(dto contract/dto-test.cpp)
gost_add_offline_test(sse unit/sse-parser-test.cpp)
gost_add_offline_test(state unit/state-store-test.cpp)
gost_add_offline_test(settings unit/settings-store-test.cpp)
gost_add_offline_test(network integration/api-network-test.cpp)
gost_add_offline_test(connection integration/connection-controller-test.cpp)

foreach(name IN ITEMS lifecycle backend-readonly dto sse state settings network connection)
  target_compile_definitions(
    gost-restreamer-${name}-test
    PRIVATE
      GOST_TEST_WORKSPACE_DIR="${GOST_TEST_WORKSPACE_DIR}"
      GOST_TEST_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
      GOST_TEST_LOCALE_DIR="${PROJECT_SOURCE_DIR}/data/locale"
  )
  set_tests_properties(
    gost-restreamer.${name}
    PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${GOST_QT_TEST_ROOT}/plugins/platforms;TEMP=${PROJECT_SOURCE_DIR}/build/tmp;TMP=${PROJECT_SOURCE_DIR}/build/tmp"
  )
endforeach()

if(NOT IS_DIRECTORY "${GOST_QT_TEST_ROOT}/bin")
  message(FATAL_ERROR "Не задан проверенный runtime Qt Test 6.11.1.")
endif()
set_tests_properties(
  gost-restreamer.lifecycle gost-restreamer.backend-readonly
  gost-restreamer.dto gost-restreamer.sse gost-restreamer.state gost-restreamer.settings
  gost-restreamer.network gost-restreamer.connection
  PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${GOST_QT_TEST_ROOT}/bin"
)
