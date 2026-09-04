cmake_minimum_required(VERSION 3.20)
project(realtek-asio-probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(realtek-asio-probe
    main.cpp
)

target_link_libraries(realtek-asio-probe PRIVATE
    ole32
    oleaut32
    uuid
    mmdevapi
)

target_compile_definitions(realtek-asio-probe PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
