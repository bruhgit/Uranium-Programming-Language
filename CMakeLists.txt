cmake_minimum_required(VERSION 3.10)
project(Uranium CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(URANIUM_DEBUG_TRACE "Enable VM execution tracing." OFF)
option(URANIUM_DEBUG_PRINT_CODE "Enable bytecode disassembly during compilation." OFF)

set(SOURCES
    src/main.cpp
    src/chunk.cpp
    src/http_native.cpp
    src/godot_native.cpp
    src/gui_native.cpp
    src/heap.cpp
    src/object.cpp
    src/native_jit.cpp
    src/system_native.cpp
    src/value.cpp
    src/vm.cpp
    src/optimizer.cpp
    src/urc.cpp
    src/package_manager.cpp
    src/source_loader.cpp
    src/tooling.cpp
    src/umake.cpp
    src/lexer.cpp
    src/compiler.cpp
    src/type_system.cpp
    src/net_native.cpp
    src/crypto_native.cpp
    src/db_native.cpp
    src/thread_native.cpp
    src/sqlite3.c
)

if(WIN32)
    list(APPEND SOURCES resources.rc)
endif()

add_executable(uranium ${SOURCES})

find_package(Threads REQUIRED)
target_link_libraries(uranium PRIVATE Threads::Threads)

if(WIN32)
    target_link_libraries(uranium PRIVATE Ole32 Shell32 Comdlg32 Comctl32 Winhttp Ws2_32 Advapi32)
endif()

if(APPLE)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        target_link_libraries(uranium PRIVATE CURL::libcurl)
        target_compile_definitions(uranium PRIVATE URANIUM_HAS_CURL)
    endif()
endif()

if(UNIX AND NOT APPLE)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        target_link_libraries(uranium PRIVATE CURL::libcurl)
        target_compile_definitions(uranium PRIVATE URANIUM_HAS_CURL)
    endif()

    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_link_libraries(uranium PRIVATE OpenSSL::Crypto)
        target_compile_definitions(uranium PRIVATE URANIUM_HAS_OPENSSL)
    endif()
endif()

if(URANIUM_DEBUG_TRACE)
    target_compile_definitions(uranium PRIVATE URANIUM_DEBUG_TRACE)
endif()

if(URANIUM_DEBUG_PRINT_CODE)
    target_compile_definitions(uranium PRIVATE URANIUM_DEBUG_PRINT_CODE)
endif()
