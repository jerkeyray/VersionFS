cmake_minimum_required(VERSION 3.16)
project(VersionFS VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VERSIONFS_BUILD_FUSE "Build the Linux FUSE daemon" ON)
option(VERSIONFS_BUILD_TESTS "Build unit tests" ON)
option(VERSIONFS_ENABLE_FAULT_INJECTION "Enable test-only crash injection points" OFF)

find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# CMake's FindSQLite3 module has used both names across distributions and
# releases. Prefer imported targets, then retain a variable-based fallback.
if(TARGET SQLite3::SQLite3)
    set(VERSIONFS_SQLITE_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
    set(VERSIONFS_SQLITE_TARGET SQLite::SQLite3)
else()
    add_library(versionfs_sqlite INTERFACE)
    target_include_directories(versionfs_sqlite INTERFACE ${SQLite3_INCLUDE_DIRS})
    target_link_libraries(versionfs_sqlite INTERFACE ${SQLite3_LIBRARIES})
    set(VERSIONFS_SQLITE_TARGET versionfs_sqlite)
endif()

add_library(versionfs_core
    src/control.cpp
    src/repository.cpp
    src/util.cpp
)
target_include_directories(versionfs_core PUBLIC include)
target_link_libraries(versionfs_core PUBLIC ${VERSIONFS_SQLITE_TARGET} OpenSSL::Crypto Threads::Threads)
target_compile_options(versionfs_core PRIVATE -Wall -Wextra -Wpedantic)
if(VERSIONFS_ENABLE_FAULT_INJECTION)
    target_compile_definitions(versionfs_core PUBLIC VERSIONFS_ENABLE_FAULT_INJECTION=1)
endif()

add_executable(versionfsctl src/ctl_main.cpp)
target_link_libraries(versionfsctl PRIVATE versionfs_core)

if(VERSIONFS_BUILD_FUSE)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(WARNING "The FUSE daemon is Linux-only; disabling VERSIONFS_BUILD_FUSE")
        set(VERSIONFS_BUILD_FUSE OFF)
    endif()
endif()

if(VERSIONFS_BUILD_FUSE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)
    add_executable(versionfs src/fuse_main.cpp src/fuse_ops.cpp)
    target_link_libraries(versionfs PRIVATE versionfs_core PkgConfig::FUSE3)
    target_compile_definitions(versionfs PRIVATE FUSE_USE_VERSION=31)
    target_compile_options(versionfs PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(VERSIONFS_BUILD_TESTS)
    enable_testing()
    add_executable(versionfs_unit_tests tests/unit_tests.cpp)
    target_link_libraries(versionfs_unit_tests PRIVATE versionfs_core)
    # The tests are assert-based; keep assertions active in Release builds too.
    target_compile_options(versionfs_unit_tests PRIVATE -UNDEBUG)
    add_test(NAME versionfs_unit_tests COMMAND versionfs_unit_tests)
endif()

install(TARGETS versionfsctl RUNTIME DESTINATION bin)
if(VERSIONFS_BUILD_FUSE)
    install(TARGETS versionfs RUNTIME DESTINATION bin)
endif()
