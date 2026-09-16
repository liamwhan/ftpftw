@echo off
setlocal enabledelayedexpansion
cd /D "%~dp0"

:: --- Usage Notes -------------------------------------------------------
::
:: Central build script for the FTP client. Single unity-build translation
:: unit (src/main.c #includes the whole base layer) - no CMake, no Ninja, no
:: generated project files, no package manager. Locates MSVC itself via
:: vswhere (below) - no need to run from a Developer Command Prompt.
::
:: Usage:
::   build            debug build with MSVC (default)
::   build release    optimized build
::   build clang       build with Clang instead of MSVC
::   build clang release

:: --- Locate & Load MSVC Environment -------------------------------------------
::
:: Needed even for the Clang path: it's what puts the Windows SDK headers
:: (windows.h, winsock2.h, ...) and libs (kernel32, ws2_32, ...) on
:: INCLUDE/LIB, and cl/lib.exe on PATH for the msvc build and for archiving
:: libssh2.lib either way.
if "%VSPATH%"=="" (
  pushd "C:\Program Files (x86)\Microsoft Visual Studio\Installer\"
  for /f "delims=" %%x in ('.\vswhere.exe -latest -property InstallationPath') do set VSPATH=%%x
  popd
)
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x64

:: --- Unpack Arguments ----------------------------------------------------
for %%a in (%*) do set "%%~a=1"
if not "%msvc%"=="1" if not "%clang%"=="1" set msvc=1
if not "%release%"=="1" set debug=1
if "%debug%"=="1"   set release=0 && echo [debug mode]
if "%release%"=="1" set debug=0 && echo [release mode]
if "%msvc%"=="1"    set clang=0 && echo [msvc compile]
if "%clang%"=="1"   set msvc=0 && echo [clang compile]

:: --- Compile/Link Line Definitions ----------------------------------------
set cl_common=  /I..\src\ /nologo /FC /Z7 /Zc:preprocessor /W3
set cl_debug=   call cl /Od /Ob1 /DBUILD_DEBUG=1 %cl_common%
set cl_release= call cl /O2 /DBUILD_DEBUG=0 %cl_common%
set cl_link=    /link /INCREMENTAL:NO /opt:ref
set cl_out=     /out:
set cl_obj_out= /Fo:
set cl_only_compile= /c
set cl_mklib=   call lib -nologo

set clang_common=  -mcx16 -I..\src\ -fdiagnostics-absolute-paths -Wall -Wno-unused-function -gcodeview
set clang_debug=   call clang -g -O0 -DBUILD_DEBUG=1 %clang_common%
set clang_release= call clang -O2 -DBUILD_DEBUG=0 %clang_common%
set clang_link=    -fuse-ld=lld -Xlinker /INCREMENTAL:NO -Xlinker /opt:ref
set clang_out=     -o
set clang_obj_out= -o
set clang_only_compile= -c
set clang_mklib=   call llvm-lib

:: --- Choose Compile/Link Lines ---------------------------------------------
if "%msvc%"=="1"    set compile_debug=%cl_debug%
if "%msvc%"=="1"    set compile_release=%cl_release%
if "%msvc%"=="1"    set compile_link=%cl_link%
if "%msvc%"=="1"    set out=%cl_out%
if "%msvc%"=="1"    set obj_out=%cl_obj_out%
if "%msvc%"=="1"    set only_compile=%cl_only_compile%
if "%msvc%"=="1"    set mklib=%cl_mklib%
if "%clang%"=="1"   set compile_debug=%clang_debug%
if "%clang%"=="1"   set compile_release=%clang_release%
if "%clang%"=="1"   set compile_link=%clang_link%
if "%clang%"=="1"   set out=%clang_out%
if "%clang%"=="1"   set obj_out=%clang_obj_out%
if "%clang%"=="1"   set only_compile=%clang_only_compile%
if "%clang%"=="1"   set mklib=%clang_mklib%
if "%debug%"=="1"   set compile=%compile_debug%
if "%release%"=="1" set compile=%compile_release%

:: --- Prep Directories --------------------------------------------------------
if not exist build mkdir build

:: --- Vendor: libssh2 (SFTP/SSH, WinCNG crypto backend - no OpenSSL) ----------
::
:: Compiled from source, as its own static lib, separately from our unity
:: build (it's third-party C with its own internals - no reason to risk name
:: collisions by pulling it into our translation unit). Cached in build\ -
:: delete build\libssh2.lib to force a rebuild (e.g. after bumping the
:: submodule).
set libssh2_dir=..\third_party\libssh2
set libssh2_flags=-I%libssh2_dir%\include -I%libssh2_dir%\src -DLIBSSH2_WINCNG
set libssh2_sources=agent bcrypt_pbkdf blowfish chacha channel cipher-chachapoly comp crypt global hostkey keepalive kex knownhost mac misc packet pem poly1305 publickey scp session sftp transport userauth userauth_kbd_packet version wincng
set libssh2_libs=libssh2.lib crypt32.lib bcrypt.lib ws2_32.lib
set gui_libs=d3d11.lib dxgi.lib d3dcompiler.lib dwrite.lib user32.lib

pushd build
if not exist libssh2.lib (
  echo [building libssh2]
  for %%f in (%libssh2_sources%) do (
    %compile_release% %libssh2_flags% %only_compile% %libssh2_dir%\src\%%f.c %obj_out%libssh2_%%f.obj || (popd & exit /b 1)
  )
  %mklib% -nologo -out:libssh2.lib libssh2_*.obj || (popd & exit /b 1)
)
popd

:: --- Build --------------------------------------------------------------------
::
:: fp_dwrite.cpp is compiled separately (as C++) and linked in as an extra
:: object - <dwrite.h> can't be included from our usual plain-C unity
:: build (see fp_dwrite.h). Not cached like libssh2.lib - it's our own
:: source, so it always recompiles along with main.c.
pushd build
echo [building font shim (DirectWrite, C++)]
%compile% %only_compile% ..\src\font\fp_dwrite.cpp %obj_out%fp_dwrite.obj
if %errorlevel% neq 0 (popd & exit /b 1)

echo [building lwftpclient]
%compile% -I%libssh2_dir%\include ..\src\main.c fp_dwrite.obj %compile_link% %libssh2_libs% %gui_libs% %out%lwftpclient.exe
if %errorlevel% neq 0 (popd & exit /b 1)
popd

echo [lwftpclient build succeeded]
