cmake_minimum_required(VERSION 3.15)
project(httpserver2 VERSION 2.6.0 LANGUAGES CXX)

# ============================================================
# 全局选项
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 所有可执行文件统一输出到 build/bin，避免根目录被产物淹没
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

option(HS2_BUILD_TESTS       "构建测试可执行文件"                        ON)
option(HS2_ENABLE_HTTP2      "编译 HTTP/2 模块（需要 nghttp2）"           ON)
option(HS2_WITH_TINYDB       "编译 TinyDB 适配器（需要 TinyDB 客户端库）" ON)

# 第三方依赖的公共父目录。原始布局约定为「源码目录的上一级」，
# 例如 <root>/testhttpserver2 + <root>/boost_1_86_0 + <root>/openssl-3.1.0。
set(HS2_DEPS_ROOT "${CMAKE_SOURCE_DIR}/.." CACHE PATH "第三方依赖的公共父目录")

if(MSVC)
    # /utf-8 必须有：源码是 UTF-8，MSVC 默认按 GBK 解码，中文注释里的多字节
    # 序列可能吞掉后续引号，产生与真实代码无关的假语法错误。
    # /bigobj 给 Beast/Asio 这类模板密集的编译单元留足对象文件空间。
    add_compile_options(/W4 /utf-8 /EHsc /bigobj)
    add_compile_definitions(
        _WIN32_WINNT=0x0A00
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
else()
    add_compile_options(-Wall -Wextra)
endif()

# ============================================================
# 第三方依赖
# ============================================================

# ---------- Boost（仅使用头文件） ----------
# 项目用到的 boost/asio、boost/beast、boost/regex、boost/algorithm 在 1.86 中
# 全部 header-only（Boost.System 自 1.69、Boost.Regex 自 1.84 起不再需要
# 单独的编译库），因此只暴露头文件路径，不链接任何 Boost 库。
set(HS2_BOOST_ROOT "" CACHE PATH "Boost 根目录（含 boost/version.hpp）")
if(NOT HS2_BOOST_ROOT)
    foreach(_cand
            "${HS2_DEPS_ROOT}/boost_1_86_0_install"
            "${HS2_DEPS_ROOT}/boost_1_86_0"
            "${HS2_DEPS_ROOT}/boost")
        if(EXISTS "${_cand}/boost/version.hpp")
            set(HS2_BOOST_ROOT "${_cand}")
            break()
        endif()
    endforeach()
endif()

if(HS2_BOOST_ROOT)
    message(STATUS "Boost (header-only): ${HS2_BOOST_ROOT}")
    add_library(hs2_boost INTERFACE)
    target_include_directories(hs2_boost INTERFACE "${HS2_BOOST_ROOT}")
else()
    # 本地目录没有就退回系统安装
    find_package(Boost QUIET)
    if(Boost_FOUND)
        message(STATUS "Boost (system install): ${Boost_INCLUDE_DIRS}")
        add_library(hs2_boost INTERFACE)
        target_include_directories(hs2_boost INTERFACE ${Boost_INCLUDE_DIRS})
    else()
        message(FATAL_ERROR
            "找不到 Boost。请用 -DHS2_BOOST_ROOT=<路径> 指定，"
            "或让 HS2_DEPS_ROOT 指向包含 boost_1_86_0 的目录。")
    endif()
endif()

# ---------- OpenSSL ----------
set(HS2_OPENSSL_ROOT "" CACHE PATH "OpenSSL 根目录（含 include/openssl/ssl.h）")
if(NOT HS2_OPENSSL_ROOT)
    foreach(_cand
            "${HS2_DEPS_ROOT}/openssl-3.3.1-install"
            "${HS2_DEPS_ROOT}/openssl-3.1.0"
            "${HS2_DEPS_ROOT}/openssl")
        if(EXISTS "${_cand}/include/openssl/ssl.h")
            set(HS2_OPENSSL_ROOT "${_cand}")
            break()
        endif()
    endforeach()
endif()
if(HS2_OPENSSL_ROOT)
    set(OPENSSL_ROOT_DIR "${HS2_OPENSSL_ROOT}")
    message(STATUS "OpenSSL root: ${HS2_OPENSSL_ROOT}")
endif()
find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)
message(STATUS "OpenSSL include: ${OPENSSL_INCLUDE_DIR}")

# ---------- zlib ----------
# 不用 FindZLIB：它在 Windows 上会优先命中 zlibstatic.lib（/MT 编译的静态库），
# 与本项目 /MD 的运行库冲突，链接期报 LNK4098。这里自己找，优先动态库
# （zlib.lib 是 import lib，对应 zlib.dll），静态库只在没有动态版时才用。
set(HS2_ZLIB_ROOT "" CACHE PATH "zlib 根目录")
if(NOT HS2_ZLIB_ROOT)
    foreach(_cand "${HS2_DEPS_ROOT}/zlib-install" "${HS2_DEPS_ROOT}/zlib")
        if(EXISTS "${_cand}/include/zlib.h")
            set(HS2_ZLIB_ROOT "${_cand}")
            break()
        endif()
    endforeach()
endif()

set(HS2_ZLIB_LIBRARY "")
if(HS2_ZLIB_ROOT)
    foreach(_cand
            "${HS2_ZLIB_ROOT}/lib/zlib.lib"
            "${HS2_ZLIB_ROOT}/lib/libzlib.so"
            "${HS2_ZLIB_ROOT}/lib/libzlib.dylib"
            "${HS2_ZLIB_ROOT}/lib/libz.a"
            "${HS2_ZLIB_ROOT}/lib/zlibstatic.lib"
            "${HS2_ZLIB_ROOT}/lib/libzlib.a")
        if(EXISTS "${_cand}")
            set(HS2_ZLIB_LIBRARY "${_cand}")
            break()
        endif()
    endforeach()
endif()
if(NOT HS2_ZLIB_LIBRARY)
    # 回退：让 CMake 自己找，但要绕开 FindZLIB 在 Windows 上偏好
    # zlibstatic.lib（/MT 编译，与本项目的 /MD 运行库冲突）的问题。
    find_package(ZLIB REQUIRED)
    if(DEFINED ZLIB_LIBRARY_RELEASE AND EXISTS "${ZLIB_LIBRARY_RELEASE}")
        set(HS2_ZLIB_LIBRARY "${ZLIB_LIBRARY_RELEASE}")
    else()
        set(HS2_ZLIB_LIBRARY "${ZLIB_LIBRARY}")
    endif()
    set(HS2_ZLIB_ROOT "")
    foreach(_cand "${ZLIB_INCLUDE_DIRS}")
        get_filename_component(_zdir "${_cand}" DIRECTORY)
        set(HS2_ZLIB_ROOT "${_zdir}")
        break()
    endforeach()
endif()
message(STATUS "zlib: ${HS2_ZLIB_LIBRARY}")

add_library(hs2_zlib STATIC IMPORTED GLOBAL)
set_target_properties(hs2_zlib PROPERTIES
    IMPORTED_LOCATION "${HS2_ZLIB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${HS2_ZLIB_ROOT}/include"
)

# ---------- SQLite3 ----------
set(HS2_SQLITE_ROOT "" CACHE PATH "SQLite3 根目录")
if(NOT HS2_SQLITE_ROOT)
    foreach(_cand "${HS2_DEPS_ROOT}/sqlite-install" "${HS2_DEPS_ROOT}/sqlite3")
        if(EXISTS "${_cand}/sqlite3.h" OR EXISTS "${_cand}/include/sqlite3.h")
            set(HS2_SQLITE_ROOT "${_cand}")
            break()
        endif()
    endforeach()
endif()
if(HS2_SQLITE_ROOT)
    set(SQLite3_ROOT "${HS2_SQLITE_ROOT}")
    message(STATUS "SQLite3 root: ${HS2_SQLITE_ROOT}")
endif()
find_package(SQLite3 REQUIRED)
message(STATUS "SQLite3 include: ${SQLite3_INCLUDE_DIRS}")

# ---------- PipeYAML ----------
# PipeYAML（github.com/zhaoliangcn/PipeYAML）是 YAML 1.2 解析库，替代 yaml-cpp。
# 与 yaml-cpp 相比有三处差异直接影响构建：
#  1. 头文件路径是 include/pipeyaml/pipeyaml.h，不是 include/yaml-cpp/yaml.h；
#  2. 纯静态库且没有导出宏（无 dllimport/dllexport），不需要像 yaml-cpp 那样
#     定义 YAML_CPP_STATIC_DEFINE——那个宏在 yaml-cpp 里是必须的，否则头文件
#     会把符号声明成 dllimport，链接报一堆 __imp_ 未解析；
#  3. MSVC 运行库必须与本项目一致（/MD）。PipeYAML 的 build_win 用的是
#     MultiThreadedDLL（/MD），若本项目用 /MT 链接会报 LNK1169 符号重复。
#     本项目统一用 /MD，这里无需额外处理，但保留了显式校验。
#
# 不用 find_package：PipeYAML 安装后只有 lib/cmake/PipeYAML/PipeYAMLTargets.cmake，
# 且其 IMPORTED_LOCATION 指向 install prefix 下的绝对路径。本项目用的是
# 「已安装布局」和「源码树 + 单独构建目录」两种布局并存，直接探测更可靠。
set(HS2_PIPEYAML_ROOT "" CACHE PATH "PipeYAML 根目录（含 include/pipeyaml/pipeyaml.h）")
if(NOT HS2_PIPEYAML_ROOT)
    foreach(_cand
            "${HS2_DEPS_ROOT}/PipeYAML-install"
            "${HS2_DEPS_ROOT}/PipeYAML"
            "${HS2_DEPS_ROOT}/thirdparty/PipeYAML"
            "${HS2_DEPS_ROOT}/pipeyaml")
        if(EXISTS "${_cand}/include/pipeyaml/pipeyaml.h")
            set(HS2_PIPEYAML_ROOT "${_cand}")
            break()
        endif()
    endforeach()
endif()
if(NOT HS2_PIPEYAML_ROOT)
    message(FATAL_ERROR
        "找不到 PipeYAML。请用 -DHS2_PIPEYAML_ROOT=<路径> 指定。")
endif()

set(HS2_PIPEYAML_INCLUDE "${HS2_PIPEYAML_ROOT}/include")
set(HS2_PIPEYAML_LIB "")
foreach(_cand
        "${HS2_PIPEYAML_ROOT}/lib/pipeyaml.lib"
        "${HS2_PIPEYAML_ROOT}/lib/libpipeyaml.a"
        "${HS2_PIPEYAML_ROOT}/lib/libpipeyaml.so"
        "${HS2_PIPEYAML_ROOT}/lib/libpipeyaml.dylib"
        "${HS2_PIPEYAML_ROOT}/build_win/Release/pipeyaml.lib"
        "${HS2_PIPEYAML_ROOT}/build_win/pipeyaml.lib"
        "${HS2_PIPEYAML_ROOT}/build-msvc/pipeyaml.lib"
        "${HS2_PIPEYAML_ROOT}/build-msvc/Release/pipeyaml.lib"
        "${HS2_PIPEYAML_ROOT}/build/libpipeyaml.a"
        "${HS2_PIPEYAML_ROOT}/build/libpipeyaml.so"
        "${HS2_PIPEYAML_ROOT}/build/Release/pipeyaml.lib")
    if(EXISTS "${_cand}")
        set(HS2_PIPEYAML_LIB "${_cand}")
        break()
    endif()
endforeach()
if(NOT HS2_PIPEYAML_LIB)
    message(FATAL_ERROR
        "在 ${HS2_PIPEYAML_ROOT} 下找不到 PipeYAML 库。\n"
        "  MSVC:  cmake -S <PipeYAML> -B build_win -G \"Visual Studio 17 2022\" -A x64 ... && cmake --build build_win --config Release\n"
        "  GCC :  cmake -S <PipeYAML> -B build ... && cmake --build build\n"
        "  或直接安装： cmake --install build_win --prefix <安装目录>")
endif()
message(STATUS "PipeYAML: ${HS2_PIPEYAML_LIB}")

add_library(hs2_pipeyaml STATIC IMPORTED GLOBAL)
set_target_properties(hs2_pipeyaml PROPERTIES
    IMPORTED_LOCATION "${HS2_PIPEYAML_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${HS2_PIPEYAML_INCLUDE}"
)

# ---------- nghttp2 ----------
set(HS2_NGHTTP2_ROOT "" CACHE PATH "nghttp2 根目录")
if(NOT HS2_NGHTTP2_ROOT)
    foreach(_cand
            "${HS2_DEPS_ROOT}/nghttp2-install"
            "${HS2_DEPS_ROOT}/nghttp2"
            "${HS2_DEPS_ROOT}/installed/x64-windows"
            "${HS2_DEPS_ROOT}/installed/x64-linux")
        if(EXISTS "${_cand}/include/nghttp2/nghttp2.h")
            set(HS2_NGHTTP2_ROOT "${_cand}")
            break()
        endif()
    endforeach()
endif()

set(HS2_NGHTTP2_LIB "")
if(HS2_NGHTTP2_ROOT)
    foreach(_cand
            "${HS2_NGHTTP2_ROOT}/lib/nghttp2.lib"
            "${HS2_NGHTTP2_ROOT}/lib/libnghttp2.a"
            "${HS2_NGHTTP2_ROOT}/lib/libnghttp2.so"
            "${HS2_NGHTTP2_ROOT}/lib/libnghttp2.dylib")
        if(EXISTS "${_cand}")
            set(HS2_NGHTTP2_LIB "${_cand}")
            break()
        endif()
    endforeach()
endif()
if(NOT HS2_NGHTTP2_LIB)
    find_package(nghttp2 QUIET CONFIG)
    if(nghttp2_FOUND)
        foreach(_t nghttp2::libnghttp2 nghttp2::nghttp2_shared nghttp2::nghttp2)
            if(TARGET ${_t})
                set(HS2_NGHTTP2_TARGET "${_t}")
                break()
            endif()
        endforeach()
    endif()
endif()

if(HS2_NGHTTP2_LIB)
    message(STATUS "nghttp2: ${HS2_NGHTTP2_LIB}")
    add_library(hs2_nghttp2 STATIC IMPORTED GLOBAL)
    set_target_properties(hs2_nghttp2 PROPERTIES
        IMPORTED_LOCATION "${HS2_NGHTTP2_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${HS2_NGHTTP2_ROOT}/include"
    )
    # nghttp2 只提供静态库时，头文件默认按 dllimport 声明符号，
    # 必须定义 NGHTTP2_STATICLIB 才是正确的声明方式。
    # Windows 上 .lib 既可能是静态库也可能是 import 库，靠有无配套 DLL 区分。
    set(_hs2_nghttp2_static ON)
    if(WIN32)
        get_filename_component(_nh_dir "${HS2_NGHTTP2_LIB}" DIRECTORY)
        get_filename_component(_nh_name "${HS2_NGHTTP2_LIB}" NAME_WE)
        file(GLOB _nh_dll "${_nh_dir}/${_nh_name}*.dll" "${_nh_dir}/../bin/${_nh_name}*.dll")
        if(_nh_dll)
            set(_hs2_nghttp2_static OFF)
        endif()
    endif()
    if(_hs2_nghttp2_static)
        set_property(TARGET hs2_nghttp2 APPEND PROPERTY
            INTERFACE_COMPILE_DEFINITIONS NGHTTP2_STATICLIB)
    endif()
elseif(HS2_NGHTTP2_TARGET)
    message(STATUS "nghttp2: ${HS2_NGHTTP2_TARGET}")
else()
    message(FATAL_ERROR
        "找不到 nghttp2。请用 -DHS2_NGHTTP2_ROOT=<路径> 指定，"
        "或关闭 HTTP/2 模块：-DHS2_ENABLE_HTTP2=OFF")
endif()

# ---------- TinyDB 客户端库 ----------
set(HS2_TINYDB_ROOT "" CACHE PATH "TinyDB 根目录")
if(NOT HS2_TINYDB_ROOT)
    foreach(_cand "${HS2_DEPS_ROOT}/TinyDB" "${HS2_DEPS_ROOT}/tinydb")
        if(EXISTS "${_cand}/include/client/tinydb_client.h")
            set(HS2_TINYDB_ROOT "${_cand}")
            break()
        endif()
    endforeach()
endif()

set(HS2_TINYDB_LIB "")
if(HS2_TINYDB_ROOT)
    foreach(_cand
            "${HS2_TINYDB_ROOT}/build_win/Release/tinydb_client.lib"
            "${HS2_TINYDB_ROOT}/build_win/tinydb_client.lib"
            "${HS2_TINYDB_ROOT}/build/libtinydb_client.so"
            "${HS2_TINYDB_ROOT}/build/libtinydb_client.a"
            "${HS2_TINYDB_ROOT}/build/Release/tinydb_client.lib"
            "${HS2_TINYDB_ROOT}/lib/libtinydb_client.so")
        if(EXISTS "${_cand}")
            set(HS2_TINYDB_LIB "${_cand}")
            break()
        endif()
    endforeach()
endif()
if(NOT HS2_TINYDB_LIB)
    message(FATAL_ERROR
        "找不到 TinyDB 客户端库。请用 -DHS2_TINYDB_ROOT=<路径> 指定，"
        "或关闭 TinyDB 适配器：-DHS2_WITH_TINYDB=OFF")
endif()
message(STATUS "TinyDB client: ${HS2_TINYDB_LIB}")

add_library(hs2_tinydb STATIC IMPORTED GLOBAL)
set_target_properties(hs2_tinydb PROPERTIES
    IMPORTED_LOCATION "${HS2_TINYDB_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${HS2_TINYDB_ROOT}/include"
)

# ---------- 平台库 ----------
if(WIN32)
    # ws2_32   : Winsock，Boost.Asio 在 Windows 上必需
    # crypt32  : OpenSSL 证书与随机数
    # advapi32 : 注册表 / 加密服务
    # user32   : 控制台与消息循环
    set(HS2_PLATFORM_LIBS ws2_32 crypt32 advapi32 user32)
else()
    # pthread : std::thread
    # dl      : plugin_manager 的动态加载
    set(HS2_PLATFORM_LIBS pthread dl)
endif()

# ============================================================
# 源文件
# ============================================================
# logger.h 需要恰好一个编译单元定义 LOGGER_IMPLEMENTATION
set(LOGGER_IMPL logger_impl.cpp)

# 所有目标共享的核心实现
set(CORE_SRCS
    ${LOGGER_IMPL}
    httpserver2.cpp
    auth/jwt.cpp
    validation/validator.cpp
    session/memory_store.cpp
    db/sqlite_database.cpp
    ws/websocket.cpp
    cache/memory_cache.cpp
    stream/streaming.cpp
    plugin/plugin_manager.cpp
    middleware/request_id.cpp
    docs/openapi.cpp
)

if(HS2_ENABLE_HTTP2)
    list(APPEND CORE_SRCS http2/http2_handler.cpp)
endif()

if(HS2_WITH_TINYDB)
    list(APPEND CORE_SRCS db/tinydb_database.cpp)
    add_compile_definitions(HS2_WITH_TINYDB)
endif()

# ============================================================
# 目标
# ============================================================

# 给每个可执行目标套用统一的依赖
function(hs2_configure_target tgt)
    target_include_directories(${tgt} PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(${tgt} PRIVATE
        hs2_boost
        OpenSSL::SSL
        OpenSSL::Crypto
        hs2_zlib
        SQLite::SQLite3
        ${HS2_PLATFORM_LIBS}
    )
    if(HS2_NGHTTP2_LIB)
        target_link_libraries(${tgt} PRIVATE hs2_nghttp2)
    elseif(HS2_NGHTTP2_TARGET)
        target_link_libraries(${tgt} PRIVATE ${HS2_NGHTTP2_TARGET})
    endif()
    if(HS2_WITH_TINYDB)
        target_link_libraries(${tgt} PRIVATE hs2_tinydb)
    endif()
endfunction()

# 主服务器
add_executable(httpserver2 ${CORE_SRCS} main.cpp)
hs2_configure_target(httpserver2)
target_link_libraries(httpserver2 PRIVATE hs2_pipeyaml)

if(HS2_BUILD_TESTS)
    set(HS2_TEST_NAMES
        test_concurrent
        test_proxy
        test_proxy_simple
        test_static_performance
        test_reverse_proxy_simple
        test_unit)

    foreach(_t ${HS2_TEST_NAMES})
        if(EXISTS "${CMAKE_SOURCE_DIR}/${_t}.cpp")
            add_executable(${_t} ${CORE_SRCS} ${_t}.cpp)
            hs2_configure_target(${_t})
        endif()
    endforeach()

    # 压力测试：默认走 Boost.Asio 后端。
    # 另有 hp-async-io 后端（需要 C++20 协程与 clang++），用 USE_HP_ASYNC_IO 开启。
    option(USE_HP_ASYNC_IO "Use hp-async-io backend for test_stress" OFF)
    if(USE_HP_ASYNC_IO)
        set(HP_ASYNC_IO_DIR "${HS2_DEPS_ROOT}/hp-async-io")
        set(HP_ASYNC_IO_LIBRARY "${HP_ASYNC_IO_DIR}/build/libhp_async_io.a")
        if(NOT EXISTS "${HP_ASYNC_IO_LIBRARY}")
            message(FATAL_ERROR "hp-async-io library not found at ${HP_ASYNC_IO_LIBRARY}")
        endif()
        find_program(CLANGXX clang++)
        if(CLANGXX)
            set(CMAKE_CXX_COMPILER "${CLANGXX}")
            message(STATUS "hp-async-io: using clang++ at ${CLANGXX}")
        else()
            message(WARNING "clang++ not found, hp-async-io may not compile with GCC <10")
        endif()

        add_executable(test_stress test_stress.cpp)
        target_compile_definitions(test_stress PRIVATE USE_HP_ASYNC_IO)
        target_include_directories(test_stress PRIVATE
            "${CMAKE_SOURCE_DIR}" "${HP_ASYNC_IO_DIR}/include")
        if(MSVC)
            target_compile_options(test_stress PRIVATE /std:c++20)
        else()
            target_compile_options(test_stress PRIVATE -std=c++20 -fcoroutines-ts)
        endif()
        target_link_libraries(test_stress PRIVATE "${HP_ASYNC_IO_LIBRARY}" pthread)
    else()
        add_executable(test_stress ${LOGGER_IMPL} test_stress.cpp)
        target_include_directories(test_stress PRIVATE "${CMAKE_SOURCE_DIR}")
        target_link_libraries(test_stress PRIVATE
            hs2_boost
            OpenSSL::SSL
            OpenSSL::Crypto
            ${HS2_PLATFORM_LIBS}
        )
        if(HS2_NGHTTP2_LIB)
            target_link_libraries(test_stress PRIVATE hs2_nghttp2)
        endif()
        if(HS2_WITH_TINYDB)
            target_link_libraries(test_stress PRIVATE hs2_tinydb)
        endif()
    endif()
endif()

# ============================================================
# 运行时依赖复制
# ============================================================
# Windows 上 OpenSSL / TinyDB / zlib 都是共享库，运行时 DLL 必须与 exe 同目录，
# 否则程序启动即报「找不到 xxx.dll」。
set(HS2_RUNTIME_DIR "")
if(WIN32)
    file(GLOB _ossl_dll "${HS2_OPENSSL_ROOT}/bin/*.dll")
    file(GLOB _tdb_dll "${HS2_TINYDB_ROOT}/build_win/Release/*.dll")
    file(GLOB _zlib_dll "${HS2_ZLIB_ROOT}/bin/*.dll")
    list(APPEND HS2_RUNTIME_DIR ${_ossl_dll} ${_tdb_dll} ${_zlib_dll})
else()
    file(GLOB _so "${CMAKE_BINARY_DIR}/*.so")
    list(APPEND HS2_RUNTIME_DIR ${_so})
endif()

if(HS2_RUNTIME_DIR)
    add_custom_command(TARGET httpserver2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${HS2_RUNTIME_DIR} "$<TARGET_FILE_DIR:httpserver2>"
        COMMENT "Copying runtime libraries next to the executable..."
    )
endif()

# ============================================================
# RPATH（Linux / macOS）
# ============================================================
set(HS2_ALL_TARGETS httpserver2)
if(HS2_BUILD_TESTS)
    list(APPEND HS2_ALL_TARGETS ${HS2_TEST_NAMES})
    if(TARGET test_stress)
        list(APPEND HS2_ALL_TARGETS test_stress)
    endif()
endif()

if(UNIX)
    foreach(_t ${HS2_ALL_TARGETS})
        set_target_properties(${_t} PROPERTIES
            INSTALL_RPATH "$ORIGIN:$ORIGIN/lib"
            BUILD_RPATH "${CMAKE_BINARY_DIR}"
        )
    endforeach()

    if(APPLE)
        foreach(_t ${HS2_ALL_TARGETS})
            set_target_properties(${_t} PROPERTIES
                INSTALL_NAME_DIR "@rpath"
                BUILD_WITH_INSTALL_RPATH TRUE
            )
        endforeach()
    endif()
endif()

# ============================================================
# 安装规则
# ============================================================
install(TARGETS ${HS2_ALL_TARGETS}
    RUNTIME DESTINATION bin
)

# ============================================================
# 便利目标
# ============================================================
add_custom_target(run
    COMMAND $<TARGET_FILE:httpserver2>
    DEPENDS httpserver2
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Starting HttpServer2..."
)

if(HS2_BUILD_TESTS)
    set(HS2_TEST_TARGETS ${HS2_TEST_NAMES})
    if(TARGET test_stress)
        list(APPEND HS2_TEST_TARGETS test_stress)
    endif()

    add_custom_target(build-tests
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${HS2_TEST_TARGETS}
        COMMENT "Building all test executables..."
    )

    enable_testing()
    foreach(_t ${HS2_TEST_TARGETS})
        add_test(NAME ${_t}
                 COMMAND $<TARGET_FILE:${_t}>
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endforeach()
endif()

# Python 测试套件（可选，缺 python3 时只跳过，不让配置失败）
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(test-py
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/run_tests.py --skip-start --port 8080
        COMMENT "Running Python test suite (server must be started manually)..."
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    add_custom_target(test-py-auto
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/run_tests.py
        COMMENT "Running Python test suite (auto-start server)..."
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
else()
    message(STATUS "python3 not found, skipping Python test targets")
endif()

add_custom_target(clean-all
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target clean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}
    COMMENT "Cleaning all build artifacts..."
)
