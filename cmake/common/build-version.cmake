cmake_minimum_required(VERSION 3.28...3.31)

# Скрипт вызывается целью сборки: GOST_VERSION_SOURCE_DIR задаёт корень исходников,
# GOST_VERSION_BASE — числовую версию project(), GOST_VERSION_HEADER — выходной заголовок.
# Числовая версия проверяется до подстановки в строковый литерал C++.
if(NOT IS_DIRECTORY "${GOST_VERSION_SOURCE_DIR}" OR NOT GOST_VERSION_HEADER OR
   NOT GOST_VERSION_BASE MATCHES "^[0-9]+[.][0-9]+[.][0-9]+([.][0-9]+)?$")
  message(FATAL_ERROR "Не заданы корректные параметры отображаемой версии.")
endif()

# GOST_BUILD_COMMIT содержит проверенный короткий HEAD либо честный признак отсутствия метаданных.
set(GOST_BUILD_COMMIT "unknown")
# Git_FOUND сообщает о доступности установленного Git; GIT_EXECUTABLE задаёт найденную программу.
# Git остаётся необязательным: архив исходников собирается с суффиксом +unknown.
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${GOST_VERSION_SOURCE_DIR}/.git")
  # gost_git_root — корень репозитория, gost_git_root_result — код или причина отказа Git.
  # Отдельный .git в исходниках и совпадение корня исключают случайный HEAD внешнего репозитория.
  # .git может быть каталогом обычного checkout или файлом связанного worktree.
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${GOST_VERSION_SOURCE_DIR}" rev-parse --show-toplevel
    RESULT_VARIABLE gost_git_root_result
    OUTPUT_VARIABLE gost_git_root
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    ENCODING UTF-8
    TIMEOUT 5
  )
  if(gost_git_root_result STREQUAL "0" AND IS_DIRECTORY "${gost_git_root}")
    # gost_source_real и gost_git_root_real сравнивают фактические пути, включая возможные ссылки.
    file(REAL_PATH "${GOST_VERSION_SOURCE_DIR}" gost_source_real)
    file(REAL_PATH "${gost_git_root}" gost_git_root_real)
    if(gost_source_real STREQUAL gost_git_root_real)
      # gost_git_commit — ответ Git, gost_git_commit_result — код или причина отказа чтения HEAD.
      # --short=7 сохраняет минимум семь символов и допускает большую длину для уникальности.
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${GOST_VERSION_SOURCE_DIR}" rev-parse --verify --short=7 HEAD
        RESULT_VARIABLE gost_git_commit_result
        OUTPUT_VARIABLE gost_git_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        ENCODING UTF-8
        TIMEOUT 5
      )
      # gost_git_commit_length ограничивает принятую длину SHA-1/SHA-256 перед подстановкой в C++.
      string(LENGTH "${gost_git_commit}" gost_git_commit_length)
      if(gost_git_commit_result STREQUAL "0" AND gost_git_commit MATCHES "^[0-9a-fA-F]+$" AND
         gost_git_commit_length GREATER_EQUAL 7 AND gost_git_commit_length LESS_EQUAL 64)
        set(GOST_BUILD_COMMIT "${gost_git_commit}")
      endif()
    endif()
  endif()
endif()

# configure_file сохраняет время изменения заголовка, если его содержимое не изменилось.
configure_file("${CMAKE_CURRENT_LIST_DIR}/build-version.hpp.in" "${GOST_VERSION_HEADER}" @ONLY NEWLINE_STYLE LF)
