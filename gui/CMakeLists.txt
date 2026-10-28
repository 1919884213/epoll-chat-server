# 聊天室快照面板（LVGL 9.6 on RK3566 / fbdev + evdev）
#
#   cmake -S . -B build-arm64 -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake
#   cmake --build build-arm64 -j8            # 产物 build-arm64/chat_gui
cmake_minimum_required(VERSION 3.16)
project(chat_gui C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

set(LVGL_DIR "$ENV{HOME}/sdk/lvgl-9.6.0" CACHE PATH "LVGL 源码树")
set(APP_TOUCH ON CACHE BOOL "编入 evdev 触摸后端")

if(NOT EXISTS "${LVGL_DIR}/lvgl.h")
  message(FATAL_ERROR "LVGL_DIR 不对：${LVGL_DIR} 里没有 lvgl.h")
endif()

# ---------- LVGL 静态库：自己 glob 源码，不用 LVGL 自带 CMake（它不读 lv_conf.h） ----------
file(GLOB_RECURSE LVGL_SRC CONFIGURE_DEPENDS "${LVGL_DIR}/src/*.c")
list(FILTER LVGL_SRC EXCLUDE REGEX "/src/drivers/")
foreach(_lib freetype libpng libjpeg_turbo libwebp ffmpeg gstreamer nanovg gltf rlottie lz4 frogfs)
  list(FILTER LVGL_SRC EXCLUDE REGEX "/src/libs/${_lib}/")
endforeach()

add_library(lvgl STATIC ${LVGL_SRC})
target_include_directories(lvgl PUBLIC
  "${LVGL_DIR}" "${LVGL_DIR}/include" "${LVGL_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}")            # lv_conf.h 在本工程根
target_link_libraries(lvgl PUBLIC m)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)

# 9.6 的 linux fbdev 驱动在 src/drivers/display/fb/（比头文件多一层 fb/）
target_sources(lvgl PRIVATE "${LVGL_DIR}/src/drivers/display/fb/lv_linux_fbdev.c")

if(APP_TOUCH)
  target_sources(lvgl PRIVATE "${LVGL_DIR}/src/drivers/evdev/lv_evdev.c")
  target_compile_definitions(lvgl PUBLIC LV_PORT_EVDEV_ON=1)
else()
  target_compile_definitions(lvgl PUBLIC LV_PORT_EVDEV_ON=0)
endif()

# ---------- 面板本体 ----------
file(GLOB_RECURSE APP_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")   # 含 src/fonts/*.c
add_executable(chat_gui ${APP_SRC})
target_include_directories(chat_gui PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../kernel/chatsnap")     # chatsnap_uapi.h（与驱动共用）
target_compile_options(chat_gui PRIVATE -Wall -Wextra)
target_link_libraries(chat_gui PRIVATE lvgl)

message(STATUS "chat_gui: touch=${APP_TOUCH}  LVGL=${LVGL_DIR}  build=${CMAKE_BUILD_TYPE}")
