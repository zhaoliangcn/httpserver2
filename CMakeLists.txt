cmake_minimum_required(VERSION 3.15)
project(httpserver2 VERSION 2.0.0 LANGUAGES CXX)

# ============================================================
# C++ 标准与编译选项
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -O2)
endif()

# ============================================================
# 构建类型（默认 Release）
# ============================================================
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ============================================================
# 第三方依赖查找
# ============================================================

# --- Boost ---
# 优先查找系统安装的 Boost，失败则回退到本地目录
find_package(Boost 1.86.0 QUIET COMPONENTS system thread)
if(NOT Boost_FOUND)
    message(STATUS "System Boost not found, trying local installation...")
    set(BOOST_ROOT "${CMAKE_SOURCE_DIR}/../boost_1_86_0_install" CACHE PATH "Boost installation root")
    set(Boost_NO_SYSTEM_PATHS ON)
    find_package(Boost 1.86.0 REQUIRED COMPONENTS system thread)
endif()
include_directories(${Boost_INCLUDE_DIRS})
message(STATUS "Boost include: ${Boost_INCLUDE_DIRS}")
message(STATUS "Boost libraries: ${Boost_LIBRARIES}")

# --- OpenSSL ---
# 查找本地 OpenSSL 3.1.0（非标准布局：库文件在根目录，头文件在 include/）
set(OPENSSL_LOCAL_DIR "${CMAKE_SOURCE_DIR}/../openssl-3.1.0")
if(EXISTS "${OPENSSL_LOCAL_DIR}/include/openssl/ssl.h" AND EXISTS "${OPENSSL_LOCAL_DIR}/libssl.so")
    message(STATUS "Found local OpenSSL at ${OPENSSL_LOCAL_DIR}")
    set(OPENSSL_FOUND TRUE)
    set(OPENSSL_INCLUDE_DIR "${OPENSSL_LOCAL_DIR}/include")
    set(OPENSSL_SSL_LIBRARY "${OPENSSL_LOCAL_DIR}/libssl.so")
    set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_LOCAL_DIR}/libcrypto.so")
    set(OPENSSL_LIBRARIES "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}")
    include_directories(${OPENSSL_INCLUDE_DIR})
    link_directories(${OPENSSL_LOCAL_DIR})
    # 创建导入目标以便 target_link_libraries 使用
    add_library(OpenSSL::SSL SHARED IMPORTED GLOBAL)
    set_target_properties(OpenSSL::SSL PROPERTIES
        IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    )
    add_library(OpenSSL::Crypto SHARED IMPORTED GLOBAL)
    set_target_properties(OpenSSL::Crypto PROPERTIES
        IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    )
else()
    # 尝试系统安装的 OpenSSL 3.x
    find_package(OpenSSL 3.0 QUIET)
    if(NOT OPENSSL_FOUND)
        message(FATAL_ERROR "OpenSSL 3.0+ not found. Install it or set OPENSSL_ROOT_DIR.\n"
                "Local path checked: ${OPENSSL_LOCAL_DIR}")
    endif()
endif()
message(STATUS "OpenSSL include: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "OpenSSL libraries: ${OPENSSL_LIBRARIES}")
include_directories(${OPENSSL_INCLUDE_DIR})
message(STATUS "OpenSSL include: ${OPENSSL_INCLUDE_DIR}")

# --- yaml-cpp（始终使用本地版本，避免系统旧版干扰） ---
set(YAML_CPP_DIR "${CMAKE_SOURCE_DIR}/../thirdparty/yaml-cpp")
set(YAML_CPP_INCLUDE_DIR "${YAML_CPP_DIR}/include")
set(YAML_CPP_LIBRARY "${YAML_CPP_DIR}/build/libyaml-cpp.a")
if(NOT EXISTS "${YAML_CPP_LIBRARY}")
    message(FATAL_ERROR "yaml-cpp library not found at ${YAML_CPP_LIBRARY}. Build it first:\n"
            "  cd ${YAML_CPP_DIR} && cmake . && make")
endif()
include_directories(${YAML_CPP_INCLUDE_DIR})
add_library(yaml-cpp STATIC IMPORTED GLOBAL)
set_target_properties(yaml-cpp PROPERTIES
    IMPORTED_LOCATION "${YAML_CPP_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${YAML_CPP_INCLUDE_DIR}"
)
message(STATUS "yaml-cpp include: ${YAML_CPP_INCLUDE_DIR}")
message(STATUS "yaml-cpp lib: ${YAML_CPP_LIBRARY}")

# ============================================================
# 源文件
# ============================================================
# 注意: logger.h 需要恰好一个编译单元定义 LOGGER_IMPLEMENTATION
set(LOGGER_IMPL logger_impl.cpp)

set(SERVER_SRCS
    ${LOGGER_IMPL}
    httpserver2.cpp
    main.cpp
)

set(TEST_CONCURRENT_SRCS
    ${LOGGER_IMPL}
    httpserver2.cpp
    test_concurrent.cpp
)

set(TEST_PROXY_SRCS
    ${LOGGER_IMPL}
    httpserver2.cpp
    test_proxy.cpp
)

set(TEST_STATIC_PERF_SRCS
    ${LOGGER_IMPL}
    httpserver2.cpp
    test_static_performance.cpp
)

set(TEST_REVERSE_PROXY_SRCS
    ${LOGGER_IMPL}
    httpserver2.cpp
    test_reverse_proxy_simple.cpp
)

set(TEST_UNIT_SRCS
    ${LOGGER_IMPL}
    httpserver2.cpp
    test_unit.cpp
)

# ============================================================
# 可执行文件
# ============================================================

# 主服务器
add_executable(httpserver2 ${SERVER_SRCS})
target_link_libraries(httpserver2
    PRIVATE
        Boost::system
        Boost::thread
        OpenSSL::SSL
        OpenSSL::Crypto
        yaml-cpp
        pthread
        z
)

# 并发测试
add_executable(test_concurrent ${TEST_CONCURRENT_SRCS})
target_link_libraries(test_concurrent
    PRIVATE
        Boost::system
        Boost::thread
        OpenSSL::SSL
        OpenSSL::Crypto
        pthread
        z
)

# 代理测试
add_executable(test_proxy ${TEST_PROXY_SRCS})
target_link_libraries(test_proxy
    PRIVATE
        Boost::system
        Boost::thread
        OpenSSL::SSL
        OpenSSL::Crypto
        pthread
        z
)

# 静态文件性能测试
add_executable(test_static_performance ${TEST_STATIC_PERF_SRCS})
target_link_libraries(test_static_performance
    PRIVATE
        Boost::system
        Boost::thread
        OpenSSL::SSL
        OpenSSL::Crypto
        pthread
        z
)

# 反向代理简单测试
add_executable(test_reverse_proxy_simple ${TEST_REVERSE_PROXY_SRCS})
target_link_libraries(test_reverse_proxy_simple
    PRIVATE
        Boost::system
        Boost::thread
        OpenSSL::SSL
        OpenSSL::Crypto
        pthread
        z
)

# 核心组件单元测试
add_executable(test_unit ${TEST_UNIT_SRCS})
target_link_libraries(test_unit
    PRIVATE
        Boost::system
        Boost::thread
        OpenSSL::SSL
        OpenSSL::Crypto
        pthread
        z
)

# ============================================================
# RPATH 设置（Linux / macOS）
# ============================================================
set(ALL_TARGETS httpserver2 test_concurrent test_proxy test_static_performance test_reverse_proxy_simple test_unit)

if(UNIX)
    foreach(target ${ALL_TARGETS})
        set_target_properties(${target} PROPERTIES
            INSTALL_RPATH "$ORIGIN:$ORIGIN/lib"
            BUILD_RPATH "${Boost_LIBRARY_DIRS};${OPENSSL_LOCAL_DIR};${CMAKE_BINARY_DIR}"
        )
    endforeach()

    if(APPLE)
        foreach(target ${ALL_TARGETS})
            set_target_properties(${target} PROPERTIES
                INSTALL_NAME_DIR "@rpath"
                BUILD_WITH_INSTALL_RPATH TRUE
            )
        endforeach()
    endif()
endif()

# ============================================================
# 安装规则
# ============================================================
install(TARGETS ${ALL_TARGETS}
    RUNTIME DESTINATION bin
)

# ============================================================
# 复制依赖库（仅当库文件存在时）
# ============================================================
set(LIB_COPY_COMMANDS "")
if(Boost_LIBRARY_DIRS AND EXISTS "${Boost_LIBRARY_DIRS}/libboost_system.so")
    list(APPEND LIB_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${Boost_LIBRARY_DIRS}/libboost_system.so"        "${CMAKE_BINARY_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${Boost_LIBRARY_DIRS}/libboost_thread.so"        "${CMAKE_BINARY_DIR}/"
    )
endif()
if(OPENSSL_LOCAL_DIR AND EXISTS "${OPENSSL_LOCAL_DIR}/libssl.so")
    list(APPEND LIB_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENSSL_LOCAL_DIR}/libssl.so"                  "${CMAKE_BINARY_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENSSL_LOCAL_DIR}/libcrypto.so"               "${CMAKE_BINARY_DIR}/"
    )
endif()
if(LIB_COPY_COMMANDS)
    add_custom_command(TARGET httpserver2 POST_BUILD
        ${LIB_COPY_COMMANDS}
        COMMENT "Copying shared libraries to build directory..."
    )
endif()

# ============================================================
# 便利目标
# ============================================================
add_custom_target(run
    COMMAND ./httpserver2
    DEPENDS httpserver2
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Starting HttpServer2..."
)

add_custom_target(build-tests
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test_concurrent
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test_proxy
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test_static_performance
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test_reverse_proxy_simple
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test_unit
    COMMENT "Building all test executables..."
)

add_custom_target(test
    COMMAND ${CMAKE_COMMAND} -E echo "========================================"
    COMMAND ${CMAKE_COMMAND} -E echo "  HttpServer2 Python 测试套件"
    COMMAND ${CMAKE_COMMAND} -E echo "========================================"
    COMMAND python3 ${CMAKE_SOURCE_DIR}/run_tests.py --skip-start --port 8080
    COMMENT "Running Python test suite (server must be started manually)..."
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_custom_target(test-auto
    COMMAND python3 ${CMAKE_SOURCE_DIR}/run_tests.py
    COMMENT "Running Python test suite (auto-start server)..."
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_custom_target(clean-all
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target clean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}
    COMMENT "Cleaning all build artifacts..."
)
