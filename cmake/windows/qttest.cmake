# Официальный комплект OBS не содержит Qt Test. Отдельный комплект той же версии
# используется только процессами тестов; DLL плагина получает Qt из OBS SDK.
include_guard(GLOBAL)

set(GOST_QT_TEST_ROOT "${PROJECT_SOURCE_DIR}/.deps/qt-test-kit-6.11.1")
set(_qt_test_archive "${PROJECT_SOURCE_DIR}/.deps/qtbase-msvc-6.11.1.7z")
set(_qt_test_sha256 "7f97edc3937fec7383eb865e010ed5128155bf9c80a563abca450860f3e9bef5")
set(
  _qt_test_url
  "https://ftp.fau.de/qtproject/online/qtsdkrepository/windows_x86/desktop/qt6_6111/qt6_6111_msvc2022_64/qt.qt6.6111.win64_msvc2022_64/6.11.1-0-202605090529qtbase-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z"
)

if(NOT EXISTS "${_qt_test_archive}")
  message(STATUS "Загрузка официального Qt Test 6.11.1 для тестов")
  file(
    DOWNLOAD "${_qt_test_url}" "${_qt_test_archive}"
    EXPECTED_HASH "SHA256=${_qt_test_sha256}"
    TLS_VERIFY ON
    TIMEOUT 180
    SHOW_PROGRESS
  )
endif()
file(SHA256 "${_qt_test_archive}" _qt_test_actual_sha256)
if(NOT _qt_test_actual_sha256 STREQUAL _qt_test_sha256)
  message(FATAL_ERROR "SHA256 архива Qt Test не совпадает с закреплённым значением.")
endif()
if(NOT EXISTS "${GOST_QT_TEST_ROOT}/lib/cmake/Qt6Test/Qt6TestConfig.cmake")
  file(MAKE_DIRECTORY "${GOST_QT_TEST_ROOT}")
  file(ARCHIVE_EXTRACT INPUT "${_qt_test_archive}" DESTINATION "${GOST_QT_TEST_ROOT}")
endif()
set(Qt6Test_DIR "${GOST_QT_TEST_ROOT}/lib/cmake/Qt6Test")
