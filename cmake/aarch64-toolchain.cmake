# 交叉编译到 aarch64（RK3566 / Ubuntu 24.04）
#
#   cmake -S . -B build-arm64 -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake
#
# sysroot 默认不设：交叉工具链自带的 libc 头就够用。
# 早期版本会自动探测 ~/rk3566-sysroot —— 那个目录一旦存在（哪怕只放了 libevdev），
# CMAKE_SYSROOT 就会被指过去，连带把工具链自带的 libc 头一起换掉，直接编不过。
# 所以改成显式 opt-in：-DUSE_TARGET_SYSROOT=ON 才启用。
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
# 本项目是纯 C；若工程里真有 C++，需先 sudo apt install g++-aarch64-linux-gnu 再补 CMAKE_CXX_COMPILER

option(USE_TARGET_SYSROOT "使用 ~/rk3566-sysroot 作为 CMAKE_SYSROOT" OFF)
if(USE_TARGET_SYSROOT)
  set(CMAKE_SYSROOT "$ENV{HOME}/rk3566-sysroot")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

message(STATUS "交叉编译 ${CMAKE_SYSTEM_PROCESSOR}，sysroot=${CMAKE_SYSROOT}（opt-in=${USE_TARGET_SYSROOT}）")
