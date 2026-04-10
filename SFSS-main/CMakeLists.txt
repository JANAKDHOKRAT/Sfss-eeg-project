cmake_minimum_required(VERSION 3.12)
project(SFSS C CXX)

# ─────────────────────────────────────────────
# ─────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")

# ─────────────────────────────────────────────
# Find EMP-tool
# ─────────────────────────────────────────────
find_path(CMAKE_FOLDER NAMES cmake/emp-tool-config.cmake)
include(${CMAKE_FOLDER}/cmake/emp-base.cmake)

find_package(emp-tool REQUIRED)
include_directories(${EMP-TOOL_INCLUDE_DIRS})

# ─────────────────────────────────────────────
# GMP (Linux default paths usually auto-detected)
# ─────────────────────────────────────────────
find_library(GMP_LIB gmp REQUIRED)
find_library(GMPXX_LIB gmpxx REQUIRED)

# ─────────────────────────────────────────────
# Original SFSS executables (optional)
# ─────────────────────────────────────────────
add_executable(sfss_main sfss_main.cpp)
target_link_libraries(sfss_main ${EMP-TOOL_LIBRARIES} gmp gmpxx)

add_executable(field field.cpp)
target_link_libraries(field ${EMP-TOOL_LIBRARIES} gmp gmpxx)

add_executable(client_runner_main client_runner_main.cpp)
target_link_libraries(client_runner_main ${EMP-TOOL_LIBRARIES} gmp gmpxx)

add_executable(server_runner_main server_runner_main.cpp)
target_link_libraries(server_runner_main ${EMP-TOOL_LIBRARIES} gmp gmpxx)

# ─────────────────────────────────────────────
# 🔥 IMPORTANT: FFI STATIC LIB (Rust needs this)
# ─────────────────────────────────────────────
add_library(sfss_ffi STATIC
    sfss_ffi.cpp
    field.cpp
)

target_link_libraries(sfss_ffi
    ${EMP-TOOL_LIBRARIES}
    gmp
    gmpxx
)

target_include_directories(sfss_ffi PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${EMP-TOOL_INCLUDE_DIRS}
)

install(TARGETS sfss_ffi DESTINATION lib)
install(FILES sfss_ffi.h DESTINATION include)