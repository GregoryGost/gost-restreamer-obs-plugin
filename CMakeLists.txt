cmake_minimum_required(VERSION 3.28...3.31)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)
project(${_name} VERSION ${_version} LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Поддерживается сборка для Windows x64.")
endif()

option(ENABLE_FRONTEND_API "Использовать OBS Frontend API" ON)
option(ENABLE_QT "Использовать интерфейс Qt Widgets" ON)
if(NOT ENABLE_FRONTEND_API OR NOT ENABLE_QT)
  message(FATAL_ERROR "Для плагина необходимы OBS Frontend API и Qt Widgets.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
include(compilerconfig)
include(defaults)
include(helpers)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 6.11.1 EXACT REQUIRED COMPONENTS Core Widgets Network)

add_library(gost-restreamer-core STATIC)
target_sources(
  gost-restreamer-core
  PRIVATE
    src/core/state-store.cpp
    src/core/state-store.hpp
    src/core/plugin-lifecycle.cpp
    src/core/plugin-lifecycle.hpp
    src/core/settings-store.cpp
    src/core/settings-store.hpp
    src/core/connection-controller.cpp
    src/core/connection-controller.hpp
    src/api/models.hpp
    src/api/json-decoder.cpp
    src/api/json-decoder.hpp
    src/api/api-client.cpp
    src/api/api-client.hpp
    src/api/event-client.cpp
    src/api/event-client.hpp
    src/api/sse-parser.cpp
    src/api/sse-parser.hpp
    src/api/network-origin.hpp
    src/obs/frontend.hpp
    src/obs/obs-controller.cpp
    src/obs/obs-controller.hpp
    src/ui/text-lookup.hpp
    src/ui/connection-text.hpp
    src/ui/restream-dock.cpp
    src/ui/restream-dock.hpp
    src/ui/management-window.cpp
    src/ui/management-window.hpp
)
target_include_directories(gost-restreamer-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(gost-restreamer-core PUBLIC cxx_std_20)
target_link_libraries(gost-restreamer-core PUBLIC Qt6::Core Qt6::Widgets Qt6::Network)
set_target_properties(gost-restreamer-core PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)

add_library(${CMAKE_PROJECT_NAME} MODULE)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.cpp src/obs/obs-frontend.cpp src/obs/obs-frontend.hpp)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)
target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE gost-restreamer-core OBS::libobs OBS::obs-frontend-api Qt6::Network
)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON CXX_EXTENSIONS OFF)
set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

include(CTest)
if(BUILD_TESTING)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/qttest.cmake")
  find_package(Qt6 6.11.1 EXACT REQUIRED COMPONENTS Test)
  add_subdirectory(tests)
endif()
