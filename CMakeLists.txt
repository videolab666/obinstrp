cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(FFmpeg REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale FFmpeg::avformat)

# Event metadata is stored in an embedded SQLite database. Build the official
# amalgamation into the plugin instead of depending on a system SQLite DLL/
# dylib, so Windows/macOS/Linux packages have identical schema/runtime behavior.
include(FetchContent)
FetchContent_Declare(
  sr_sqlite_source
  URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
  URL_HASH SHA3_256=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR
  __sports_replay_no_cmake__
)
FetchContent_MakeAvailable(sr_sqlite_source)

set(SR_SQLITE_DIR "${sr_sqlite_source_SOURCE_DIR}")
if(NOT EXISTS "${SR_SQLITE_DIR}/sqlite3.c")
  set(SR_SQLITE_DIR "${sr_sqlite_source_SOURCE_DIR}/sqlite-amalgamation-3530400")
endif()
if(NOT EXISTS "${SR_SQLITE_DIR}/sqlite3.c" OR NOT EXISTS "${SR_SQLITE_DIR}/sqlite3.h")
  message(FATAL_ERROR "Pinned SQLite amalgamation did not contain sqlite3.c/sqlite3.h")
endif()

add_library(sr_sqlite STATIC "${SR_SQLITE_DIR}/sqlite3.c")
set_target_properties(
  sr_sqlite
  PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden COMPILE_WARNING_AS_ERROR OFF
)
if(MSVC)
  target_compile_options(sr_sqlite PRIVATE /W0)
else()
  target_compile_options(sr_sqlite PRIVATE -w)
endif()
target_include_directories(sr_sqlite PUBLIC "${SR_SQLITE_DIR}")
target_compile_definitions(
  sr_sqlite
  PRIVATE SQLITE_THREADSAFE=1 SQLITE_DEFAULT_FOREIGN_KEYS=1 SQLITE_OMIT_LOAD_EXTENSION=1
)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE sr_sqlite)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/sr-buffer.c
    src/sr-codec.c
    src/sr-frame-cache.c
    src/sr-replay-decode.c
    src/capture-filter.c
    src/playback-source.c
    src/sr-scene-tracker.c
    src/sr-clip.c
    src/sr-save.c
    src/sr-load.c
    src/sr-config.c
    src/sr-session.c
    src/sr-event-db.c
    src/sr-event-controller.c
    src/sr-event-dock.cpp
    src/sr-segment-writer.c
    src/sr-segment-reader.c
    src/sr-segment-catalog.c
    src/sr-disk-decode.c
    src/sr-disk-player.c
    src/sr-thumb.c
    src/sr-dock.cpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
