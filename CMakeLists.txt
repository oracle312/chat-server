cmake_minimum_required(VERSION 3.10)
project(chat_server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Boost.System (+Asio 헤더온리) 찾기
find_package(Boost 1.71 REQUIRED COMPONENTS system)

add_executable(server src/main.cpp)

# 인클루드 디렉터리
target_include_directories(server PRIVATE ${Boost_INCLUDE_DIRS})

# 링크 라이브러리
target_link_libraries(server PRIVATE Boost::system pthread)
