# BOSS · App 原生层（任务6）
#
# 只有两个源文件，且刻意保持"薄"：
#   boss_ipc.c     协议客户端（连接 / 握手 / 收输出 / 收退出码）
#   boss_ipc_jni.c JNI 胶水
# 任何特权逻辑都不在这里——App 侧长出特权就是架构红线第 1 条。
#
# 编译期钉死的事：
#   · -fvisibility=hidden：App 的 .so 不该向外暴露符号，这对"低痕迹"是有意义的
#   · 不开 exceptions / rtti：这一层用不上，开了只是白白增大体积
cmake_minimum_required(VERSION 3.22.0)
project(bossipc C)

add_library(bossipc SHARED boss_ipc.c boss_ipc_jni.c)

target_include_directories(bossipc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(bossipc PRIVATE
        -fvisibility=hidden
        -Wall -Wextra -Wno-unused-parameter)

# BOSS_MANAGER_PKG 必须与 daemon 侧编译时的取值一致，否则 App 的 cmdline
# 对不上包名，manager 判定永远失败（表现为"App 里所有功能都显示被拒绝"）。
# 改这里就要改 daemon，两处不一致是最难查的一类问题——它不报错，只是不生效。
target_compile_definitions(bossipc PRIVATE BOSS_MANAGER_PKG="com.boss.manager")

target_link_libraries(bossipc log)

# ---- veritpath：镜像分析与注入（任务7 对接）----
#
# 只 add_subdirectory，**不** target_link_libraries 到 bossipc。
# 两个 .so 完全独立：
#   libbossipc.so   —— 与 bossd 说话（运行时管理）
#   libveritpath.so —— 分析与修补 boot 镜像（装机期/OTA 后）
# 各自被 Java 侧 System.loadLibrary 单独加载。
#
# 为什么不链起来：
#   bossipc 的设计约束是"薄"——它只做 IPC，任何多余依赖都是负担。
#   而且 veritpath 是装机期才用得上的东西，让它常驻在管理链路里没有意义。
#   AGP 会把 CMake 产出的所有 .so 都打进 APK，所以不链接也照样会被打包。
#
# ⚠️ VP_NO_MAIN 等定义在 veritpath/CMakeLists.txt 里，别在这里重复。
add_subdirectory(veritpath)
