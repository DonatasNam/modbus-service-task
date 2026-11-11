cmake_minimum_required(VERSION 3.16)
project(modbus-poller VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(MBP_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MBP_VERBOSE_ERRORS "Log step-level error detail (function, line, status)" ON)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MODBUS REQUIRED IMPORTED_TARGET libmodbus>=3.1.6)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson>=1.7.13)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_compile_definitions(_GNU_SOURCE)
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes)
if(MBP_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

enable_testing()
add_subdirectory(modbus-poller)

add_executable(modbus-pollerd daemon/modbus_pollerd.c)
target_link_libraries(modbus-pollerd PRIVATE mbp)

install(TARGETS modbus-pollerd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(FILES config/modbus_config_tcp.json DESTINATION /etc/modbus-poller RENAME config.json)
install(FILES packaging/modbus-poller.service DESTINATION /lib/systemd/system)
install(FILES README.md config/demo_local.json DESTINATION ${CMAKE_INSTALL_DOCDIR})
