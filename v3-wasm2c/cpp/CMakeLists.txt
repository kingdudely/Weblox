cmake_minimum_required(VERSION 3.20)
project(weblox_host LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set these to the downloaded engine assets:
#   GEN_INCLUDE  directory containing WC.h
#   WABT_INCLUDE directory containing wasm-rt.h
#   ENGINE_LIB   libweblox_engine.a / .so / .lib
set(GEN_INCLUDE "$ENV{GEN_INCLUDE}")
set(WABT_INCLUDE "$ENV{WABT_INCLUDE}")
set(ENGINE_LIB "$ENV{ENGINE_LIB}")

include_directories(${GEN_INCLUDE} ${WABT_INCLUDE})

# stubs.c is generated from WC.h by tools/ci/gen_stubs.py.
add_library(wc_stubs STATIC stubs.c)
set_target_properties(wc_stubs PROPERTIES LINKER_LANGUAGE C)

find_package(Threads REQUIRED)

add_executable(weblox host.cpp window.cpp platform.cpp net_stub.cpp net_curl.cpp
  gpu_stub.cpp fiber_stub.cpp fiber_ucontext.cpp imports.cpp)
target_link_libraries(weblox PRIVATE wc_stubs "${ENGINE_LIB}" Threads::Threads)
