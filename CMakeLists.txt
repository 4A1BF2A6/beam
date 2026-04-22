# CMake最低版本要求
cmake_minimum_required(VERSION 3.16)

# 项目名称，同时声明使用C和C++两种语言
project(ds_beamforming_demo C CXX)

# 指定C和C++的语言标准
set(CMAKE_C_STANDARD   17)
set(CMAKE_CXX_STANDARD 17)

# ── PortAudio 库查找 ────────────────────────────────────────────────────────
# PortAudio 是跨平台音频I/O库，用于从麦克风采集多通道音频数据

# 方式一：尝试通过vcpkg或系统包管理器自动查找
find_package(portaudio CONFIG QUIET)
if(NOT portaudio_FOUND)
    # 方式二：手动搜索库文件（.lib/.so）和头文件（portaudio.h）
    find_library(PA_LIB  NAMES portaudio portaudio_x64)
    find_path   (PA_INC  NAMES portaudio.h)
    if(PA_LIB AND PA_INC)
        # 找到后手动创建一个导入目标，供后续 target_link_libraries 使用
        add_library(portaudio UNKNOWN IMPORTED)
        set_target_properties(portaudio PROPERTIES
            IMPORTED_LOCATION             "${PA_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${PA_INC}")
    else()
        # 两种方式都失败则报错，提示安装方法
        message(FATAL_ERROR
            "PortAudio not found.\n"
            "Install via vcpkg:  vcpkg install portaudio:x64-windows\n"
            "Then re-run cmake with: -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
    endif()
endif()

# ── 可执行文件定义 ──────────────────────────────────────────────────────────

# 将以下三个源文件编译并链接为可执行程序 demo（Windows上生成 demo.exe）
add_executable(demo
    main.cpp                # 主程序：PortAudio录音 + 调用波束成形
    ds_beamforming.c        # 波束成形算法核心（TDOA估计 + 延迟求和）
    speex/fft/fft_wrap.c    # FFT工具库（Cooley-Tukey实数FFT封装）
)

# 将项目根目录加入头文件搜索路径，使 #include "ds_beamforming.h" 等能正常找到
target_include_directories(demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 链接 PortAudio 库
target_link_libraries(demo PRIVATE portaudio)

# Windows平台需要额外链接 winmm（Windows多媒体API），PortAudio底层依赖它
if(WIN32)
    target_link_libraries(demo PRIVATE winmm)
endif()
