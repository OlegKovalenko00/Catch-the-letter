cmake_minimum_required(VERSION 3.16)
project(catch_the_letter CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(USE_SYSTEM_DEPS "Use system-installed header-only deps" OFF)
option(USE_FETCHCONTENT "Fetch header-only deps from GitHub" OFF)

find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

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
    CURL::libcurl
    SQLite::SQLite3
    Threads::Threads
)

if (USE_FETCHCONTENT)
  target_link_libraries(catch_the_letter PRIVATE
    nlohmann_json::nlohmann_json
    httplib::httplib
  )
endif()
