# veritpath · 镜像分析与注入（以 JNI 动态库形式接进 BOSS App）
#
# 来源：1 号的独立仓库 hgffdkhn-dot/veritpath，按 docs/ANDROID_APP.md 的
#       「方案 A：JNI 动态库」落地（方案 B 打包二进制再 exec 会被 sdcard 的
#       noexec 挂载卡死，Android 10+ 对私有目录执行二进制的限制也越来越严）。
#
# ⚠️ 这份源码是 **vendored（复制进来的）**，不是 submodule：
#      · 好处：离线可编，CI 不用为了编 APK 再去克隆一个仓。
#      · 代价：上游更新不会自动跟过来，会漂移。
#      同步方法见 docs/TASK7-veritpath对接.md 的「上游同步」一节。
cmake_minimum_required(VERSION 3.22.0)
project(veritpath C)

set(CMAKE_C_STANDARD 11)

file(GLOB VP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

# VP_NO_MAIN 必须定义：否则 CLI 的 main() 会被链进 .so。
# 少了它不会报错，只是 .so 里多一个 main 符号——
# 而真正暴露问题是运行时：Java 侧以为在调库，实际行为很怪且查不出来。
add_library(veritpath SHARED ${VP_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/veritpath_jni.c)
target_compile_definitions(veritpath PRIVATE VP_NO_MAIN)
target_include_directories(veritpath PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(veritpath PRIVATE -Wall -Wextra -O2)

# zlib：BOOT 镜像的 ramdisk 段可能是 gzip 压缩的，解包要用。
# Android 平台自带 libz，find_library 找不到就退回直接链 -lz。
find_library(ZLIB zlib)
if(ZLIB)
    target_link_libraries(veritpath ${ZLIB})
else()
    target_link_libraries(veritpath z)
endif()

# ⚠️ 这里**故意不**用 -fvisibility=hidden。
#    bossipc 那个库用了（它不需要对外暴露任何符号），但 veritpath 的 JNI 入口
#    Java_dev_veritpath_Veritpath_* 必须是 default visibility，
#    System.loadLibrary 后 JVM 才能按名字找到它们。
#    隐藏了的表现是 java.lang.UnsatisfiedLinkError: No implementation found，
#    而编译、链接、打包三步全都是绿的——只在运行时炸。
