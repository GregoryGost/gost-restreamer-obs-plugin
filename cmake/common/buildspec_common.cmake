# Common build dependencies module

include_guard(GLOBAL)

# _check_deps_version: Checks for obs-deps VERSION file in prefix paths
function(_check_deps_version version)
  set(found FALSE)

  foreach(path IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${path}/share/obs-deps/VERSION")
      if(dependency STREQUAL qt6 AND NOT EXISTS "${path}/lib/cmake/Qt6/Qt6Config.cmake")
        set(found FALSE)
        continue()
      endif()

      file(READ "${path}/share/obs-deps/VERSION" _check_version)
      string(REPLACE "\n" "" _check_version "${_check_version}")
      string(REPLACE "-" "." _check_version "${_check_version}")
      string(REPLACE "-" "." version "${version}")

      if(_check_version VERSION_EQUAL version)
        set(found TRUE)
        break()
      elseif(_check_version VERSION_LESS version)
        message(
          AUTHOR_WARNING
          "Older ${label} version detected in ${path}: \n"
          "Found ${_check_version}, require ${version}"
        )
        list(REMOVE_ITEM CMAKE_PREFIX_PATH "${path}")
        list(APPEND CMAKE_PREFIX_PATH "${path}")
        set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH})
        continue()
      else()
        message(
          AUTHOR_WARNING
          "Newer ${label} version detected in ${path}: \n"
          "Found ${_check_version}, require ${version}"
        )
        set(found TRUE)
        break()
      endif()
    endif()
  endforeach()

  return(PROPAGATE found CMAKE_PREFIX_PATH)
endfunction()

# _setup_obs_studio: Create obs-studio build project, then build libobs and obs-frontend-api
function(_setup_obs_studio)
  # Windows-адаптация собирает Development SDK в .deps из закреплённых исходников OBS.
  # Функция использует _obs_destination/_obs_version и шаблоны архивов из вызывающего bootstrap;
  # это внутренний шаг configure, а не установка OBS приложения или плагина на компьютер.
  if(NOT libobs_DIR)
    set(_is_fresh --fresh)
  endif()

  if(OS_WINDOWS)
    # OBS использует собственный кеш архивов даже при заданном CMAKE_PREFIX_PATH.
    # Передаём ему проверенные пакеты, чтобы не загружать тот же SDK повторно.
    set(_obs_dependency_cache "${dependencies_dir}/${_obs_destination}/.deps")
    file(MAKE_DIRECTORY "${_obs_dependency_cache}")
    foreach(_sdk_dependency IN ITEMS prebuilt qt6)
      string(JSON _sdk_version GET "${buildspec}" dependencies ${_sdk_dependency} version)
      string(JSON _sdk_hash GET "${buildspec}" dependencies ${_sdk_dependency} hashes ${platform})
      string(
        JSON _sdk_revision
        ERROR_VARIABLE _sdk_revision_error
        GET "${buildspec}" dependencies ${_sdk_dependency} revision ${platform}
      )
      set(_sdk_archive "${${_sdk_dependency}_filename}")
      string(REPLACE "VERSION" "${_sdk_version}" _sdk_archive "${_sdk_archive}")
      string(REPLACE "ARCH" "${arch}" _sdk_archive "${_sdk_archive}")
      if(_sdk_revision)
        string(REPLACE "-REVISION" "-v${_sdk_revision}" _sdk_archive "${_sdk_archive}")
      else()
        string(REPLACE "-REVISION" "" _sdk_archive "${_sdk_archive}")
      endif()
      file(SHA256 "${dependencies_dir}/${_sdk_archive}" _sdk_actual_hash)
      if(NOT _sdk_actual_hash STREQUAL _sdk_hash)
        message(FATAL_ERROR "SHA256 архива ${_sdk_archive} не совпадает с закреплённым значением.")
      endif()
      file(
        COPY_FILE "${dependencies_dir}/${_sdk_archive}" "${_obs_dependency_cache}/${_sdk_archive}"
        ONLY_IF_DIFFERENT
      )
    endforeach()
    set(_cmake_generator "${CMAKE_GENERATOR}")
    # Дочерний configure повторяет генератор VS, архитектуру и Windows SDK родительского preset.
    # Этот путь использует -A и не является взаимозаменяемой конфигурацией Ninja.
    set(_cmake_arch -A "${arch},version=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
    set(_cmake_extra "-DCMAKE_SYSTEM_VERSION=${CMAKE_SYSTEM_VERSION}")
  elseif(OS_MACOS)
    set(_cmake_generator "Xcode")
    set(_cmake_arch "-DCMAKE_OSX_ARCHITECTURES:STRING='arm64;x86_64'")
    set(_cmake_extra "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  endif()

  message(STATUS "Configure ${label} (${arch})")
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" -S "${dependencies_dir}/${_obs_destination}" -B
      "${dependencies_dir}/${_obs_destination}/build_${arch}" -G "${_cmake_generator}" ${_cmake_arch}
      -DOBS_CMAKE_VERSION:STRING=3.0.0 -UENABLE_PLUGINS -DENABLE_CORE_MODULES:BOOL=OFF -DENABLE_FRONTEND:BOOL=OFF
      -DOBS_VERSION_OVERRIDE:STRING=${_obs_version} "-DCMAKE_PREFIX_PATH:PATH=${CMAKE_PREFIX_PATH}" ${_is_fresh}
      ${_cmake_extra}
    RESULT_VARIABLE _process_result
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_QUIET
  )
  message(STATUS "Configure ${label} (${arch}) - done")

  message(STATUS "Build ${label} (Debug - ${arch})")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build build_${arch} --target obs-frontend-api --config Debug --parallel
    WORKING_DIRECTORY "${dependencies_dir}/${_obs_destination}"
    RESULT_VARIABLE _process_result
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_QUIET
  )
  message(STATUS "Build ${label} (Debug - ${arch}) - done")

  message(STATUS "Build ${label} (Release - ${arch})")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build build_${arch} --target obs-frontend-api --config Release --parallel
    WORKING_DIRECTORY "${dependencies_dir}/${_obs_destination}"
    RESULT_VARIABLE _process_result
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_QUIET
  )
  message(STATUS "Build ${label} (Release - ${arch}) - done")

  message(STATUS "Install ${label} (${arch})")
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" --install build_${arch} --component Development --config Debug --prefix "${dependencies_dir}"
    WORKING_DIRECTORY "${dependencies_dir}/${_obs_destination}"
    RESULT_VARIABLE _process_result
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_QUIET
  )
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" --install build_${arch} --component Development --config Release --prefix "${dependencies_dir}"
    WORKING_DIRECTORY "${dependencies_dir}/${_obs_destination}"
    RESULT_VARIABLE _process_result
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_QUIET
  )
  message(STATUS "Install ${label} (${arch}) - done")
endfunction()

# _check_dependencies: Fetch and extract pre-built OBS build dependencies
function(_check_dependencies)
  # buildspec задаёт версии/хеши, платформенная обёртка — имена архивов и каталоги .deps.
  # При смене хеша извлечённый каталог заменяется; перед распаковкой проверяется сам архив,
  # включая уже существующий. Метка .sha256 помогает выбрать кеш, но не заменяет эту проверку.
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" buildspec)

  string(JSON dependency_data GET ${buildspec} dependencies)

  foreach(dependency IN LISTS dependencies_list)
    string(JSON data GET ${dependency_data} ${dependency})
    string(JSON version GET ${data} version)
    string(JSON hash GET ${data} hashes ${platform})
    string(JSON url GET ${data} baseUrl)
    string(JSON label GET ${data} label)
    string(JSON revision ERROR_VARIABLE error GET ${data} revision ${platform})

    message(STATUS "Setting up ${label} (${arch})")

    set(file "${${dependency}_filename}")
    set(destination "${${dependency}_destination}")
    string(REPLACE "VERSION" "${version}" file "${file}")
    string(REPLACE "VERSION" "${version}" destination "${destination}")
    string(REPLACE "ARCH" "${arch}" file "${file}")
    string(REPLACE "ARCH" "${arch}" destination "${destination}")
    if(revision)
      string(REPLACE "_REVISION" "_v${revision}" file "${file}")
      string(REPLACE "-REVISION" "-v${revision}" file "${file}")
    else()
      string(REPLACE "_REVISION" "" file "${file}")
      string(REPLACE "-REVISION" "" file "${file}")
    endif()

    if(EXISTS "${dependencies_dir}/.dependency_${dependency}_${arch}.sha256")
      file(
        READ
        "${dependencies_dir}/.dependency_${dependency}_${arch}.sha256"
        OBS_DEPENDENCY_${dependency}_${arch}_HASH
      )
    endif()

    set(skip FALSE)
    if(dependency STREQUAL prebuilt OR dependency STREQUAL qt6)
      if(OBS_DEPENDENCY_${dependency}_${arch}_HASH STREQUAL ${hash})
        _check_deps_version(${version})

        if(found)
          set(skip TRUE)
        endif()
      endif()
    endif()

    if(skip)
      if(NOT EXISTS "${dependencies_dir}/${file}")
        set(skip FALSE)
      endif()
    endif()

    if(skip)
      message(STATUS "Setting up ${label} (${arch}) - skipped")
      continue()
    endif()

    if(dependency STREQUAL obs-studio)
      set(url ${url}/${file})
    else()
      set(url ${url}/${version}/${file})
    endif()

    if(NOT EXISTS "${dependencies_dir}/${file}")
      message(STATUS "Downloading ${url}")
      file(DOWNLOAD "${url}" "${dependencies_dir}/${file}" STATUS download_status EXPECTED_HASH SHA256=${hash})

      list(GET download_status 0 error_code)
      list(GET download_status 1 error_message)
      if(error_code GREATER 0)
        message(STATUS "Downloading ${url} - Failure")
        message(FATAL_ERROR "Unable to download ${url}, failed with error: ${error_message}")
        file(REMOVE "${dependencies_dir}/${file}")
      else()
        message(STATUS "Downloading ${url} - done")
      endif()
    endif()

    file(SHA256 "${dependencies_dir}/${file}" actual_archive_hash)
    if(NOT actual_archive_hash STREQUAL hash)
      message(FATAL_ERROR "SHA256 архива ${file} не совпадает с закреплённым значением.")
    endif()

    if(NOT OBS_DEPENDENCY_${dependency}_${arch}_HASH STREQUAL ${hash})
      file(REMOVE_RECURSE "${dependencies_dir}/${destination}")
    endif()

    if(NOT EXISTS "${dependencies_dir}/${destination}")
      file(MAKE_DIRECTORY "${dependencies_dir}/${destination}")
      if(dependency STREQUAL obs-studio)
        file(ARCHIVE_EXTRACT INPUT "${dependencies_dir}/${file}" DESTINATION "${dependencies_dir}")
      else()
        file(ARCHIVE_EXTRACT INPUT "${dependencies_dir}/${file}" DESTINATION "${dependencies_dir}/${destination}")
      endif()
    endif()

    file(WRITE "${dependencies_dir}/.dependency_${dependency}_${arch}.sha256" "${hash}")

    if(dependency STREQUAL prebuilt)
      list(APPEND CMAKE_PREFIX_PATH "${dependencies_dir}/${destination}")
    elseif(dependency STREQUAL qt6)
      list(APPEND CMAKE_PREFIX_PATH "${dependencies_dir}/${destination}")
    elseif(dependency STREQUAL obs-studio)
      set(_obs_version ${version})
      set(_obs_destination "${destination}")
      list(APPEND CMAKE_PREFIX_PATH "${dependencies_dir}")
    endif()

    message(STATUS "Setting up ${label} (${arch}) - done")
  endforeach()

  list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)

  set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} CACHE PATH "CMake prefix search path" FORCE)

  _setup_obs_studio()
endfunction()
