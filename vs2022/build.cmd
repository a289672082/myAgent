@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem VS2022 工程统一桥接 CMake：项目无需手工维护 SDK/FTXUI 的编译和链接参数。
rem 用法：build.cmd console|sdk|examples|tests Debug|Release [clean|rebuild]
set "MODULE=%~1"
set "BUILD_CONFIG=%~2"
set "ACTION=%~3"
if "%BUILD_CONFIG%"=="" set "BUILD_CONFIG=Debug"
if /I not "%BUILD_CONFIG%"=="Debug" if /I not "%BUILD_CONFIG%"=="Release" (
  echo [myAgent VS2022] Unsupported configuration: %BUILD_CONFIG%
  exit /b 2
)
set "REPO=%~dp0.."
set "BUILD_DIR=%REPO%\build-vs2022\%MODULE%"
set "SOURCE=%REPO%"
set "TARGET="
set "OPTIONS=-DMYAGENT_BUILD_CONSOLE=OFF -DMYAGENT_BUILD_EXAMPLES=OFF -DMYAGENT_BUILD_TESTS=OFF"
if /I "%MODULE%"=="console" (
  set "TARGET=myagent_console"
  set "OPTIONS=-DMYAGENT_BUILD_CONSOLE=ON -DMYAGENT_BUILD_EXAMPLES=OFF -DMYAGENT_BUILD_TESTS=OFF"
) else if /I "%MODULE%"=="sdk" (
  set "SOURCE=%REPO%\sdk"
) else if /I "%MODULE%"=="examples" (
  set "OPTIONS=-DMYAGENT_BUILD_CONSOLE=OFF -DMYAGENT_BUILD_EXAMPLES=ON -DMYAGENT_BUILD_TESTS=OFF"
) else if /I "%MODULE%"=="tests" (
  set "OPTIONS=-DMYAGENT_BUILD_CONSOLE=OFF -DMYAGENT_BUILD_EXAMPLES=ON -DMYAGENT_BUILD_TESTS=ON"
) else (
  echo [myAgent VS2022] Unknown module: %MODULE%
  exit /b 2
)
rem 清理已生成的目标，不触碰任何 SDK 源码或配置。
if /I "%ACTION%"=="clean" (
  if not exist "%BUILD_DIR%\CMakeCache.txt" exit /b 0
  cmake --build "%BUILD_DIR%" --config "%BUILD_CONFIG%" --target clean
  if errorlevel 1 exit /b 1
  exit /b 0
)
if /I "%MODULE%"=="sdk" (
  cmake -S "%SOURCE%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64
) else (
  cmake -S "%SOURCE%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 %OPTIONS%
)
if errorlevel 1 exit /b 1
if /I "%ACTION%"=="rebuild" (
  cmake --build "%BUILD_DIR%" --config "%BUILD_CONFIG%" --target clean
  if errorlevel 1 exit /b 1
)
if /I "%MODULE%"=="console" (
  cmake --build "%BUILD_DIR%" --config "%BUILD_CONFIG%" --target myagent_console --parallel 2
) else if /I "%MODULE%"=="sdk" (
  cmake --build "%BUILD_DIR%" --config "%BUILD_CONFIG%" --target myagent --parallel 2
) else (
  cmake --build "%BUILD_DIR%" --config "%BUILD_CONFIG%" --parallel 2
)
if errorlevel 1 exit /b 1
if /I "%MODULE%"=="tests" (
  ctest --test-dir "%BUILD_DIR%" -C "%BUILD_CONFIG%" --output-on-failure
  if errorlevel 1 exit /b 1
)
exit /b 0
