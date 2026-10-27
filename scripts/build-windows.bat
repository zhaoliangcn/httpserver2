@echo off
rem ============================================================
rem  HttpServer2 - Windows build script (MSVC / C++17)
rem
rem  Usage:
rem    build-windows.bat            build httpserver2.exe and test_unit.exe
rem    build-windows.bat server     build httpserver2.exe only
rem    build-windows.bat test       build test_unit.exe only
rem    build-windows.bat clean      remove build-win/
rem
rem  Why not vcvars64.bat:
rem    vcvars64.bat calls reg.exe to probe VS and the Windows SDK. If reg.exe
rem    is on the host security blacklist (true on this machine), INCLUDE ends
rem    up missing the SDK dirs and the build fails with
rem      fatal error C1083: cannot open include file 'float.h'
rem    So PATH / INCLUDE / LIB are assembled by hand from the installed versions.
rem
rem  Adjust the two sections below to match your machine.
rem ============================================================
setlocal

rem ---------- dependency paths ----------
set BOOST_DIR=D:\boost_1_86_0
set OPENSSL_DIR=D:\openssl-3.3.1-install
set ZLIB_DIR=D:\zlib-install
set PIPEYAML_DIR=D:\PipeYAML-install
set PIPEYAML_LIB_DIR=D:\PipeYAML-install\lib
set SQLITE_DIR=D:\sqlite-install
set TINYDB_DIR=D:\TinyDB
set TINYDB_LIB_DIR=D:\TinyDB\build_win\Release
set NGHTTP2_DIR=D:\nghttp2-install

rem ---------- toolchain ----------
set VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
set MSVCVER=14.44.35207
set SDKROOT=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0

set VCTOOLS=%VSDIR%\VC\Tools\MSVC\%MSVCVER%
set PATH=%VCTOOLS%\bin\Hostx64\x64;%SDKROOT%\bin\%SDKVER%\x64;%PATH%
set INCLUDE=%VCTOOLS%\include;%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\shared;%SDKROOT%\Include\%SDKVER%\um;%SDKROOT%\Include\%SDKVER%\winrt
set LIB=%VCTOOLS%\lib\x64;%SDKROOT%\Lib\%SDKVER%\ucrt\x64;%SDKROOT%\Lib\%SDKVER%\um\x64

cd /d "%~dp0.."

set OUTDIR=build-win

if /I "%1"=="clean" goto :do_clean
if /I "%1"=="server" goto :only_server
if /I "%1"=="test" goto :only_test
goto :build_both

:only_server
set BUILD_SERVER=1
set BUILD_TEST=0
goto :run
:only_test
set BUILD_SERVER=0
set BUILD_TEST=1
goto :run
:build_both
set BUILD_SERVER=1
set BUILD_TEST=1

:run
rem ---------- compile / link flags ----------
set CFLAGS=/nologo /utf-8 /std:c++17 /EHsc /MD /O2 /W3
rem /utf-8 is required: sources are UTF-8. Without it MSVC decodes them as GBK
rem   and multi-byte sequences inside Chinese comments can swallow trailing
rem   quotes, producing confusing bogus syntax errors.
set DEFS=/D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0A00 /DNOMINMAX /D_AMD64_
rem NGHTTP2_STATICLIB: nghttp2 ships a static lib. Without this the headers
rem   declare dllimport and the link fails with many unresolved __imp_nghttp2_*.
rem PipeYAML needs no such macro: it is a plain static lib with no export
rem   macros, so its headers never declare dllimport.
set DEFS=%DEFS% /DNGHTTP2_STATICLIB

set INCS=/I"%BOOST_DIR%" /I"%OPENSSL_DIR%\include" /I"%ZLIB_DIR%\include" /I"%PIPEYAML_DIR%\include" /I"%SQLITE_DIR%" /I"%TINYDB_DIR%\include" /I"%NGHTTP2_DIR%\include" /I"%CD%"

set LIBPATHS=/LIBPATH:"%PIPEYAML_LIB_DIR%" /LIBPATH:"%OPENSSL_DIR%\lib" /LIBPATH:"%ZLIB_DIR%\lib" /LIBPATH:"%SQLITE_DIR%" /LIBPATH:"%TINYDB_LIB_DIR%" /LIBPATH:"%NGHTTP2_DIR%\lib"
set SYSLIBS=pipeyaml.lib libssl.lib libcrypto.lib zlib.lib sqlite3.lib tinydb_client.lib nghttp2.lib ws2_32.lib crypt32.lib advapi32.lib user32.lib

set COMMON_SRCS=logger_impl.cpp httpserver2.cpp auth\jwt.cpp validation\validator.cpp session\memory_store.cpp db\sqlite_database.cpp db\tinydb_database.cpp ws\websocket.cpp cache\memory_cache.cpp stream\streaming.cpp plugin\plugin_manager.cpp http2\http2_handler.cpp middleware\request_id.cpp docs\openapi.cpp

if not exist %OUTDIR% mkdir %OUTDIR%

if "%BUILD_SERVER%"=="1" goto :build_server
goto :after_server

:build_server
echo [1/2] Building httpserver2.exe ...
if not exist %OUTDIR%\srv mkdir %OUTDIR%\srv
cl %CFLAGS% /Fo:%OUTDIR%\srv\ /Fe:%OUTDIR%\httpserver2.exe %DEFS% %INCS% %COMMON_SRCS% main.cpp /link %LIBPATHS% %SYSLIBS%
if errorlevel 1 goto :fail_server

:after_server
if "%BUILD_TEST%"=="1" goto :build_test
goto :after_test

:build_test
echo [2/2] Building test_unit.exe ...
if not exist %OUTDIR%\unit mkdir %OUTDIR%\unit
cl %CFLAGS% /Fo:%OUTDIR%\unit\ /Fe:%OUTDIR%\test_unit.exe %DEFS% %INCS% %COMMON_SRCS% test_unit.cpp /link %LIBPATHS% %SYSLIBS%
if errorlevel 1 goto :fail_test

:after_test
if exist "%OPENSSL_DIR%\bin\libcrypto-3-x64.dll" copy /y "%OPENSSL_DIR%\bin\libcrypto-3-x64.dll" %OUTDIR%\ >nul 2>&1
if exist "%OPENSSL_DIR%\bin\libssl-3-x64.dll"    copy /y "%OPENSSL_DIR%\bin\libssl-3-x64.dll"    %OUTDIR%\ >nul 2>&1
if exist "%ZLIB_DIR%\bin\zlib.dll"               copy /y "%ZLIB_DIR%\bin\zlib.dll"               %OUTDIR%\ >nul 2>&1
if exist "%TINYDB_LIB_DIR%\tinydb_client.dll"    copy /y "%TINYDB_LIB_DIR%\tinydb_client.dll"    %OUTDIR%\ >nul 2>&1

echo.
echo BUILD_OK  -^>  %OUTDIR%\
exit /b 0

:fail_server
echo.
echo BUILD_FAILED: httpserver2.exe
exit /b 1

:fail_test
echo.
echo BUILD_FAILED: test_unit.exe
exit /b 1

:do_clean
if exist %OUTDIR% rmdir /s /q %OUTDIR%
echo Cleaned %OUTDIR%
exit /b 0
