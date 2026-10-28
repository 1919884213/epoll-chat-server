# 交叉编译到 RK3566 (aarch64 / Ubuntu 24.04)
# 先 -DAPP_TOUCH=OFF 只做显示；要触摸时把板子上的 libevdev 头/库拷进 sysroot 再开
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(TOOLCHAIN_PREFIX aarch64-linux-gnu)
find_program(CROSS_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc REQUIRED)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)

# sysroot 改为显式 opt-in：自动探测会在 ~/rk3566-sysroot 存在(哪怕只放了 libevdev)时
# 把 CMAKE_SYSROOT 指过去，连带把工具链自带的 libc 头换掉 -> 直接编不过
option(USE_TARGET_SYSROOT "使用 ~/rk3566-sysroot 作为 CMAKE_SYSROOT" OFF)
if(USE_TARGET_SYSROOT)
  set(CMAKE_SYSROOT "$ENV{HOME}/rk3566-sysroot")
endif()

# 这四行少了就会出现"找到宿主的库/头"这种极难查的错
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

message(STATUS "交叉编译目标 ${CMAKE_SYSTEM_PROCESSOR}，sysroot=${CMAKE_SYSROOT}")
