# QApplication работает без нативного окна OBS; проверку загрузки DLL, DPI и реальной темы это не заменяет.
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

# И явные временные каталоги, и TEMP/TMP дочерних тестов остаются внутри рабочего каталога проекта.
set(GOST_TEST_WORKSPACE_DIR "${PROJECT_SOURCE_DIR}/build/test-workspace")
file(MAKE_DIRECTORY "${GOST_TEST_WORKSPACE_DIR}" "${PROJECT_SOURCE_DIR}/build/tmp")

function(gost_add_offline_test name source)
  # name задаёт согласованные имена исполняемого файла и CTest, source — существующий сценарий QTest.
  # Каждый тест запускается отдельным процессом: глобальные прокси и палитра Qt не разделяются между ними.
  # Таймаут ограничивает зависание сценария, но не является порогом производительности продукта.
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
gost_add_offline_test(stats-baseline unit/stats-baseline-test.cpp)
gost_add_offline_test(obs-stop unit/obs-stop-test.cpp)
gost_add_offline_test(operations integration/restream-operations-test.cpp)
gost_add_offline_test(configuration-api contract/configuration-api-test.cpp)
gost_add_offline_test(configuration integration/configuration-controller-test.cpp)
gost_add_offline_test(configuration-ui unit/configuration-ui-test.cpp)
gost_add_offline_test(settings-preferences unit/settings-preferences-test.cpp)
gost_add_offline_test(fallback-api contract/fallback-api-test.cpp)
gost_add_offline_test(fallback integration/fallback-controller-test.cpp)
gost_add_offline_test(fallback-ui unit/fallback-ui-test.cpp)
gost_add_offline_test(diagnostics unit/diagnostics-page-test.cpp)

foreach(name IN ITEMS lifecycle backend-readonly dto sse state settings network connection stats-baseline obs-stop operations configuration-api configuration configuration-ui settings-preferences fallback-api fallback fallback-ui diagnostics)
  # Определения компилятора закрепляют пути фикстур/локализаций и явных временных файлов.
  # ENVIRONMENT задаётся целиком для каждого теста, включая lifecycle: TEMP/TMP не наследуют AppData.
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
# Тестовый Qt используется только процессами CTest; его библиотеки не включаются в поставку плагина.
set_tests_properties(
  gost-restreamer.lifecycle gost-restreamer.backend-readonly
  gost-restreamer.dto gost-restreamer.sse gost-restreamer.state gost-restreamer.settings
  gost-restreamer.network gost-restreamer.connection gost-restreamer.stats-baseline
  gost-restreamer.obs-stop gost-restreamer.operations
  gost-restreamer.configuration-api gost-restreamer.configuration gost-restreamer.configuration-ui
  gost-restreamer.settings-preferences gost-restreamer.fallback-api gost-restreamer.fallback gost-restreamer.fallback-ui
  gost-restreamer.diagnostics
  PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${GOST_QT_TEST_ROOT}/bin"
)
