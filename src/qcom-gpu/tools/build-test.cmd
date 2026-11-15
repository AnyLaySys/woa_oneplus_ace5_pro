@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 >nul
if errorlevel 1 exit /b 1
set "TEST_OUTPUT=%~dp0..\..\..\out\qcom-gpu-tests"
if not exist "%TEST_OUTPUT%" mkdir "%TEST_OUTPUT%"
cl /nologo /O1 /W4 /WX "%~dp0execution-test.c" /Fo"%TEST_OUTPUT%\execution-test.obj" /Fe"%TEST_OUTPUT%\execution-test.exe" cfgmgr32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX "%~dp0gpu-copy-test.c" /Fo"%TEST_OUTPUT%\gpu-copy-test.obj" /Fe"%TEST_OUTPUT%\gpu-copy-test.exe"
if errorlevel 1 exit /b 1
cl /nologo /O1 /W4 /WX "%~dp0wddm-test.c" /Fo"%TEST_OUTPUT%\wddm-test.obj" /Fe"%TEST_OUTPUT%\wddm-test.exe" gdi32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX "%~dp0async-submit-test.c" /Fo"%TEST_OUTPUT%\async-submit-test.obj" /Fe"%TEST_OUTPUT%\async-submit-test.exe" gdi32.lib
if errorlevel 1 exit /b 1
cl /nologo /O1 /W4 /WX "%~dp0gpu-statistics.c" /Fo"%TEST_OUTPUT%\gpu-statistics.obj" /Fe"%TEST_OUTPUT%\gpu-statistics.exe" gdi32.lib advapi32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX "%~dp0s4-stat.c" /Fo"%TEST_OUTPUT%\s4-stat.obj" /Fe"%TEST_OUTPUT%\s4-stat.exe"
if errorlevel 1 exit /b 1
cl /nologo /O1 /W4 /WX /EHsc "%~dp0d3d-test.cpp" /Fo"%TEST_OUTPUT%\d3d-test.obj" /Fe"%TEST_OUTPUT%\d3d-test.exe" d3d11.lib d3dcompiler.lib dxgi.lib user32.lib gdi32.lib
if errorlevel 1 exit /b 1
cl /nologo /O1 /W4 /WX "%~dp0dwm-stat.c" /Fo"%TEST_OUTPUT%\dwm-stat.obj" /Fe"%TEST_OUTPUT%\dwm-stat.exe" dwmapi.lib user32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX /EHsc /DA830_BENCHMARK "%~dp0d3d-test.cpp" /Fo"%TEST_OUTPUT%\gpu-bench.obj" /Fe"%TEST_OUTPUT%\gpu-bench.exe" d3d11.lib d3dcompiler.lib dxgi.lib user32.lib gdi32.lib
if errorlevel 1 exit /b 1
copy /y "%~dp0gpu-baseline.ps1" "%TEST_OUTPUT%\gpu-baseline.ps1" >nul
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX /EHsc "%~dp0dxgi-probe.cpp" /Fo"%TEST_OUTPUT%\dxgi-probe-arm64.obj" /Fe"%TEST_OUTPUT%\dxgi-probe-arm64.exe" d3d11.lib dxgi.lib
if errorlevel 1 exit /b 1
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX /EHsc "%~dp0dxgi-probe.cpp" /Fo"%TEST_OUTPUT%\dxgi-probe-x64.obj" /Fe"%TEST_OUTPUT%\dxgi-probe-x64.exe" d3d11.lib dxgi.lib
exit /b %errorlevel%
