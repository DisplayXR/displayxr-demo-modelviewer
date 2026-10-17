@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: VS 2022 not found
    exit /b 1
)
set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe;%PATH%"
cd /d "%~dp0\.."

REM --- Vulkan SDK ----------------------------------------------------------------
REM Use the VULKAN_SDK env var the LunarG installer sets (any version), instead of
REM a hardcoded C:/VulkanSDK/<ver> path. find_package(Vulkan) picks it up via the
REM VULKAN_SDK environment, so no -DCMAKE_PREFIX_PATH is needed.
if "%VULKAN_SDK%"=="" (
    echo ERROR: VULKAN_SDK is not set. Install the Vulkan SDK from https://vulkan.lunarg.com
    echo        and open a fresh terminal so VULKAN_SDK is exported, then re-run.
    exit /b 1
)
REM Put the SDK's Bin on PATH so CMake's find_program(glslangValidator) resolves
REM (find_package(Vulkan) locates it via VULKAN_SDK, but the bare find_program only
REM searches PATH — and some installs set VULKAN_SDK without adding Bin to PATH).
set "PATH=%VULKAN_SDK%\Bin;%PATH%"

REM --- OpenXR loader -------------------------------------------------------------
REM Provisioned by CMake itself (windows/CMakeLists.txt: FetchContent of the pinned
REM prebuilt Khronos loader, == XR_CURRENT_API_VERSION of openxr_includes/ and the
REM CI pin in .github/workflows/build-windows.yml). Nothing to stage here, so this
REM script and the bare build_windows.bat configure identically.
REM
REM Extra arguments are forwarded to the configure step, e.g. for an offline build:
REM   scripts\build-with-deps.bat -DFETCHCONTENT_SOURCE_DIR_OPENXR_LOADER=C:/deps/openxr_loader_windows-1.1.51
REM                               -DFETCHCONTENT_SOURCE_DIR_TINYUSDZ=C:/deps/tinyusdz
REM (all on one line).
REM (see CLAUDE.md / README.md "Offline / flaky-network builds").

echo === Configuring ===
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release %* || exit /b 1
echo === Building ===
cmake --build build || exit /b 1
echo === DONE ===
