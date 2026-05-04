cmake_minimum_required(VERSION 3.16)
project(catch_the_letter CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(USE_SYSTEM_DEPS "Use system-installed header-only deps" OFF)
option(USE_FETCHCONTENT "Fetch header-only deps from GitHub" OFF)

set(CURL_USE_STATIC_LIBS OFF)
find_package(CURL QUIET)
find_package(SQLite3 QUIET)
find_package(Threads REQUIRED)

if (CURL_FOUND AND TARGET CURL::libcurl)
  set(CATCH_THE_LETTER_CURL_TARGET CURL::libcurl)
  get_target_property(_curl_location CURL::libcurl IMPORTED_LOCATION_RELEASE)
  if (NOT _curl_location)
    get_target_property(_curl_location CURL::libcurl IMPORTED_LOCATION)
  endif()
  if (_curl_location MATCHES "\\.a$")
    find_library(CURL_SHARED_LIBRARY
      NAMES curl libcurl
      PATHS /usr/lib /usr/lib/x86_64-linux-gnu /lib /lib/x86_64-linux-gnu
      NO_DEFAULT_PATH
    )
    if (NOT CURL_SHARED_LIBRARY)
      message(FATAL_ERROR "CMake selected static libcurl (${_curl_location}) and no shared libcurl.so was found. Install shared libcurl development package or remove the static libcurl from the search path.")
    endif()
    add_library(curl_shared UNKNOWN IMPORTED)
    set_target_properties(curl_shared PROPERTIES
      IMPORTED_LOCATION "${CURL_SHARED_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${CURL_INCLUDE_DIRS}"
    )
    set(CATCH_THE_LETTER_CURL_TARGET curl_shared)
    message(STATUS "Using shared libcurl: ${CURL_SHARED_LIBRARY}")
  endif()
else()
  if (NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/curl/curl.h")
    message(FATAL_ERROR "libcurl development package was not found and third_party/curl/curl.h is missing. Install libcurl development headers, configure with a valid CURL package, or vendor curl headers under third_party/curl.")
  endif()
  find_library(CURL_SHARED_RUNTIME
    NAMES libcurl.so.4 curl libcurl
    PATHS /usr/lib /usr/lib/x86_64-linux-gnu /lib /lib/x86_64-linux-gnu
    NO_DEFAULT_PATH
  )
  if (NOT CURL_SHARED_RUNTIME)
    message(FATAL_ERROR "libcurl development package was not found and no shared runtime libcurl.so.4 was found. Install libcurl4/libcurl runtime or the libcurl development package.")
  endif()
  add_library(curl_shared UNKNOWN IMPORTED)
  set_target_properties(curl_shared PROPERTIES
    IMPORTED_LOCATION "${CURL_SHARED_RUNTIME}"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/third_party"
  )
  set(CATCH_THE_LETTER_CURL_TARGET curl_shared)
  message(STATUS "Using vendored curl headers with shared runtime libcurl: ${CURL_SHARED_RUNTIME}")
endif()

if (SQLite3_FOUND)
  set(CATCH_THE_LETTER_SQLITE_TARGET SQLite::SQLite3)
else()
  if (NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/sqlite3.h")
    message(FATAL_ERROR "SQLite3 development package was not found and third_party/sqlite3.h is missing. Install SQLite3 development headers or vendor sqlite3.h.")
  endif()
  find_library(SQLITE3_SHARED_RUNTIME
    NAMES libsqlite3.so.0 sqlite3 libsqlite3
    PATHS /usr/lib /usr/lib/x86_64-linux-gnu /lib /lib/x86_64-linux-gnu
    NO_DEFAULT_PATH
  )
  if (NOT SQLITE3_SHARED_RUNTIME)
    message(FATAL_ERROR "SQLite3 development package was not found and no shared runtime libsqlite3.so.0 was found. Install libsqlite3 runtime or the SQLite3 development package.")
  endif()
  add_library(sqlite3_shared UNKNOWN IMPORTED)
  set_target_properties(sqlite3_shared PROPERTIES
    IMPORTED_LOCATION "${SQLITE3_SHARED_RUNTIME}"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/third_party"
  )
  set(CATCH_THE_LETTER_SQLITE_TARGET sqlite3_shared)
  message(STATUS "Using vendored sqlite3.h with shared runtime SQLite3: ${SQLITE3_SHARED_RUNTIME}")
endif()

if (USE_FETCHCONTENT)
  include(FetchContent)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
  )
  FetchContent_MakeAvailable(nlohmann_json)

  FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
  )
  FetchContent_MakeAvailable(httplib)
elseif (USE_SYSTEM_DEPS)
  find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)
  find_path(HTTPLIB_INCLUDE_DIR httplib.h REQUIRED)
else()
  if (NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/nlohmann/json.hpp")
    message(FATAL_ERROR "Missing third_party/nlohmann/json.hpp. Vendor nlohmann/json headers or configure with -DUSE_SYSTEM_DEPS=ON.")
  endif()
  if (NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/httplib/httplib.h")
    message(FATAL_ERROR "Missing third_party/httplib/httplib.h. Vendor cpp-httplib header or configure with -DUSE_SYSTEM_DEPS=ON.")
  endif()
  if (NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/curl/curl.h")
    message(FATAL_ERROR "Missing third_party/curl/curl.h and no system libcurl headers were found. Vendor curl headers or install libcurl development headers.")
  endif()
  if (NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/sqlite3.h")
    message(FATAL_ERROR "Missing third_party/sqlite3.h and no system SQLite3 headers were found. Vendor sqlite3.h or install SQLite3 development headers.")
  endif()
endif()

add_executable(catch_the_letter
  src/main.cpp
  src/app/App.cpp
  src/app/Config.cpp
  src/app/EmailClassifier.cpp
  src/app/TelegramDialogManager.cpp
  src/app/UserProfileLoader.cpp
  src/app/WorkflowEngine.cpp
  src/domain/RuleEngine.cpp
  src/infra/MailClientMock.cpp
  src/infra/MailClientImap.cpp
  src/infra/BrowserWorkerClient.cpp
  src/infra/NoopLlmClient.cpp
  src/infra/OllamaClient.cpp
  src/infra/TelegramBot.cpp
  src/infra/TelegramNotifierMock.cpp
  src/infra/TelegramNotifier.cpp
  src/infra/TwilioNotifier.cpp
  src/infra/SqliteStorage.cpp
  src/infra/HttpServer.cpp
  src/infra/UrlPolicy.cpp
)

target_include_directories(catch_the_letter PRIVATE
  src
  third_party
  third_party/nlohmann
  third_party/httplib
)

if (USE_SYSTEM_DEPS)
  target_include_directories(catch_the_letter PRIVATE
    ${NLOHMANN_JSON_INCLUDE_DIR}
    ${HTTPLIB_INCLUDE_DIR}
  )
endif()

target_link_libraries(catch_the_letter
  PRIVATE
    ${CATCH_THE_LETTER_CURL_TARGET}
    ${CATCH_THE_LETTER_SQLITE_TARGET}
    Threads::Threads
)

if (USE_FETCHCONTENT)
  target_link_libraries(catch_the_letter PRIVATE
    nlohmann_json::nlohmann_json
    httplib::httplib
  )
endif()
