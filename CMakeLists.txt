# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# 分别设置版本号
set(VERSION_MAJOR "1")
set(VERSION_MINOR "0")
set(VERSION_PATCH "0")
set(VERSION_BUILD "0")

# 组合版本号
# set(PROJECT_VER "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
set(PROJECT_VER "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}.${VERSION_BUILD}")

# 增大A2DP流队列大小，避免音频卡顿 (默认20，增大到64)
# 必须在 project() 之前设置，通过 EXTRA_COMPONENT_DIRS 或直接修改源文件
set(EXTRA_CFLAGS "-DA2DP_STREAM_QUEUE_SIZE=64")
add_compile_options(-DA2DP_STREAM_QUEUE_SIZE=64)

include($ENV{ADF_PATH}/CMakeLists.txt)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(HTB-051-${PROJECT_VER})

