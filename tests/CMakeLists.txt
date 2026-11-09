add_executable(gost-restreamer-lifecycle-test unit/plugin-lifecycle-test.cpp fixtures/fake-frontend.hpp)
target_link_libraries(gost-restreamer-lifecycle-test PRIVATE gost-restreamer-core Qt6::Test)
target_include_directories(gost-restreamer-lifecycle-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(
  gost-restreamer-lifecycle-test
  PRIVATE GOST_TEST_LOCALE_DIR="${PROJECT_SOURCE_DIR}/data/locale"
)
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
target_link_libraries(gost-restreamer-backend-readonly-test PRIVATE Qt6::Core Qt6::Network Qt6::Test)
set_target_properties(gost-restreamer-backend-readonly-test PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
add_test(NAME gost-restreamer.backend-readonly COMMAND gost-restreamer-backend-readonly-test)
set_tests_properties(
  gost-restreamer.backend-readonly
  PROPERTIES LABELS "backend;readonly" SKIP_RETURN_CODE 77 TIMEOUT 15
)

if(NOT IS_DIRECTORY "${GOST_QT_TEST_ROOT}/bin")
  message(FATAL_ERROR "Не задан проверенный runtime Qt Test 6.11.1.")
endif()
set_tests_properties(
  gost-restreamer.lifecycle gost-restreamer.backend-readonly
  PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${GOST_QT_TEST_ROOT}/bin"
)
