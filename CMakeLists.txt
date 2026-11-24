cmake_minimum_required(VERSION 3.10)
project(gateway C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# 与硬件无关的部分：可单元测试，可交叉编译复用
add_library(core STATIC
    src/ringbuf.c
    src/frame.c
)
target_include_directories(core PUBLIC include)

# 主程序：硬件层 + 线程编排
add_executable(gateway
    src/main.c
    src/hal_sim.c
    src/hal_uart.c
    src/mqtt.c
)
target_link_libraries(gateway PRIVATE core Threads::Threads)
target_compile_options(gateway PRIVATE -Wall -Wextra)

enable_testing()
add_executable(test_ringbuf tests/test_ringbuf.c)
target_link_libraries(test_ringbuf PRIVATE core)
target_compile_options(test_ringbuf PRIVATE -Wall -Wextra)
add_test(NAME ringbuf COMMAND test_ringbuf)
