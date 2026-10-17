@echo off
REM Bare configure + build. Expects a VS developer environment (cl.exe on PATH);
REM from a plain terminal it hands off to build-with-deps.bat, which sets one up.
REM The OpenXR loader is provisioned by CMake (pinned FetchContent, see
REM windows/CMakeLists.txt) -- never pass a hardcoded SDK path. Extra arguments
REM are forwarded to the configure step (e.g. -DFETCHCONTENT_SOURCE_DIR_*=...).
setlocal
where cl >nul 2>&1 || goto :with_deps
cd /d "%~dp0\.."
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release %* || goto :error
cmake --build build || goto :error
echo.
echo Run: build\windows\model_viewer_handle_vk_win.exe
exit /b 0
:error
exit /b 1
:with_deps
call "%~dp0build-with-deps.bat" %*
exit /b
