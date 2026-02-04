cmake_minimum_required(VERSION 3.16)
project(catch_the_letter CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(USE_SYSTEM_DEPS "Use system-installed header-only deps" OFF)

find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)

if (USE_SYSTEM_DEPS)
  find_package(nlohmann_json REQUIRED)
  find_package(httplib CONFIG REQUIRED)
else()
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
endif()

add_executable(catch_the_letter
  src/main.cpp
  src/app/App.cpp
  src/app/Config.cpp
  src/domain/RuleEngine.cpp
  src/infra/MailClientMock.cpp
  src/infra/MailClientImap.cpp
  src/infra/TelegramNotifierMock.cpp
  src/infra/TelegramNotifier.cpp
  src/infra/TwilioNotifier.cpp
  src/infra/SqliteStorage.cpp
  src/infra/HttpServer.cpp
)

target_include_directories(catch_the_letter PRIVATE src)

target_link_libraries(catch_the_letter
  PRIVATE
    CURL::libcurl
    SQLite::SQLite3
    nlohmann_json::nlohmann_json
    httplib::httplib
    Threads::Threads
)
