@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 exit /b 1
set "TEST_OUTPUT=%~dp0..\..\..\out\qcom-gpu-local-tests"
if not exist "%TEST_OUTPUT%" mkdir "%TEST_OUTPUT%"
set "TEST_KM=C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\km"
cl /nologo /c /O1 /Gy /GS- /W4 /WX /D_AMD64_ /DA830_NATIVE_DISPLAY /I"%TEST_KM%" "%~dp0..\wddm\adapter-info.c" /Fo"%TEST_OUTPUT%\adapter-info.obj"
if errorlevel 1 exit /b 1
cl /nologo /c /O1 /Gy /GS- /W4 /WX /D_AMD64_ /DA830_NATIVE_DISPLAY /I"%TEST_KM%" "%~dp0..\wddm\display-copy.c" /Fo"%TEST_OUTPUT%\display-copy.obj"
if errorlevel 1 exit /b 1
cl /nologo /O1 /Gy /GS- /W4 /WX /D_AMD64_ /DA830_NATIVE_DISPLAY /I"%TEST_KM%" "%~dp0wddm-memory-test.c" "%TEST_OUTPUT%\adapter-info.obj" "%TEST_OUTPUT%\display-copy.obj" /Fo"%TEST_OUTPUT%\wddm-memory-test.obj" /Fe"%TEST_OUTPUT%\wddm-memory-test.exe" /link /OPT:REF
if errorlevel 1 exit /b 1
"%TEST_OUTPUT%\wddm-memory-test.exe"
if errorlevel 1 exit /b 1
cl /nologo /O1 /Gy /GS- /W4 /WX /D_AMD64_ /DA830_NATIVE_DISPLAY /I"%TEST_KM%" "%~dp0mmu-table-test.c" /Fo"%TEST_OUTPUT%\mmu-table-test.obj" /Fe"%TEST_OUTPUT%\mmu-table-test.exe" /link /OPT:REF
if errorlevel 1 exit /b 1
"%TEST_OUTPUT%\mmu-table-test.exe"
if errorlevel 1 exit /b 1
cl /nologo /O1 /Gy /GS- /W4 /WX /D_AMD64_ /I"%TEST_KM%" "%~dp0mmu-lifetime-test.c" /Fo"%TEST_OUTPUT%\mmu-lifetime-test.obj" /Fe"%TEST_OUTPUT%\mmu-lifetime-test.exe" /link /OPT:REF
if errorlevel 1 exit /b 1
"%TEST_OUTPUT%\mmu-lifetime-test.exe"
if errorlevel 1 exit /b 1
cl /nologo /O1 /Gy /GS- /W4 /WX /D_AMD64_ /I"%TEST_KM%" "%~dp0context-protocol-test.c" /Fo"%TEST_OUTPUT%\context-protocol-test.obj" /Fe"%TEST_OUTPUT%\context-protocol-test.exe" /link /OPT:REF
if errorlevel 1 exit /b 1
"%TEST_OUTPUT%\context-protocol-test.exe"
if errorlevel 1 exit /b 1
cl /nologo /c /O1 /Gy /GS- /W4 /WX /D_AMD64_ /DA830_NATIVE_DISPLAY /I"%TEST_KM%" "%~dp0..\wddm\present.c" /Fo"%TEST_OUTPUT%\present.obj"
if errorlevel 1 exit /b 1
cl /nologo /O1 /Gy /GS- /W4 /WX /D_AMD64_ /DA830_NATIVE_DISPLAY /I"%TEST_KM%" "%~dp0present-test.c" "%TEST_OUTPUT%\present.obj" /Fo"%TEST_OUTPUT%\present-test.obj" /Fe"%TEST_OUTPUT%\present-test.exe" /link /OPT:REF
if errorlevel 1 exit /b 1
"%TEST_OUTPUT%\present-test.exe"
if errorlevel 1 exit /b 1
cl /nologo /c /O1 /Gy /GS- /W4 /WX /D_AMD64_ /DA830_NATIVE_DISPLAY /I"%TEST_KM%" "%~dp0..\wddm\memory.c" /Fo"%TEST_OUTPUT%\memory.obj"
if errorlevel 1 exit /b 1
cl /nologo /O1 /Gy /GS- /W4 /WX /D_AMD64_ /DA830_NATIVE_DISPLAY /I"%TEST_KM%" "%~dp0paging-test.c" "%TEST_OUTPUT%\memory.obj" /Fo"%TEST_OUTPUT%\paging-test.obj" /Fe"%TEST_OUTPUT%\paging-test.exe" /link /OPT:REF
if errorlevel 1 exit /b 1
"%TEST_OUTPUT%\paging-test.exe"
if errorlevel 1 exit /b 1
cl /nologo /c /O1 /Gy /GS- /W4 /WX /D_AMD64_ /DA830_NATIVE_DISPLAY /I"%TEST_KM%" "%~dp0..\wddm\shared-memory.c" /Fo"%TEST_OUTPUT%\shared-memory.obj"
if errorlevel 1 exit /b 1
cl /nologo /O1 /Gy /GS- /W4 /WX /D_AMD64_ /DA830_NATIVE_DISPLAY /I"%TEST_KM%" "%~dp0shared-memory-test.c" "%TEST_OUTPUT%\shared-memory.obj" "%TEST_OUTPUT%\memory.obj" /Fo"%TEST_OUTPUT%\shared-memory-test.obj" /Fe"%TEST_OUTPUT%\shared-memory-test.exe" /link /OPT:REF
if errorlevel 1 exit /b 1
"%TEST_OUTPUT%\shared-memory-test.exe"
if errorlevel 1 exit /b 1
cl /nologo /O1 /W4 /WX /D_AMD64_ /I"%TEST_KM%" "%~dp0perf-table-test.c" /Fo"%TEST_OUTPUT%\perf-table-test.obj" /Fe"%TEST_OUTPUT%\perf-table-test.exe"
if errorlevel 1 exit /b 1
"%TEST_OUTPUT%\perf-table-test.exe"
if errorlevel 1 exit /b 1
cl /nologo /O1 /Gy /GS- /W4 /WX /D_AMD64_ /DA830_NATIVE_DISPLAY /I"%TEST_KM%" "%~dp0context-lifetime-test.c" /Fo"%TEST_OUTPUT%\context-lifetime-test.obj" /Fe"%TEST_OUTPUT%\context-lifetime-test.exe" /link /OPT:REF
if errorlevel 1 exit /b 1
"%TEST_OUTPUT%\context-lifetime-test.exe"
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX "%~dp0deferred-free-test.c" /Fo"%TEST_OUTPUT%\deferred-free-test.obj" /Fe"%TEST_OUTPUT%\deferred-free-test.exe"
if errorlevel 1 exit /b 1
"%TEST_OUTPUT%\deferred-free-test.exe"
if errorlevel 1 exit /b 1
cl /nologo /O1 /Gy /GS- /W4 /WX /D_AMD64_ /DA830_NATIVE_DISPLAY /I"%TEST_KM%" "%~dp0scanout-pool-test.c" /Fo"%TEST_OUTPUT%\scanout-pool-test.obj" /Fe"%TEST_OUTPUT%\scanout-pool-test.exe" /link /OPT:REF
if errorlevel 1 exit /b 1
"%TEST_OUTPUT%\scanout-pool-test.exe"
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX "%~dp0gpu-scanout-test.c" /Fo"%TEST_OUTPUT%\gpu-scanout-test.obj" /Fe"%TEST_OUTPUT%\gpu-scanout-test.exe"
if errorlevel 1 exit /b 1
"%TEST_OUTPUT%\gpu-scanout-test.exe"
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX "%~dp0rpmh-lock-test.c" /Fo"%TEST_OUTPUT%\rpmh-lock-test.obj" /Fe"%TEST_OUTPUT%\rpmh-lock-test.exe"
if errorlevel 1 exit /b 1
"%TEST_OUTPUT%\rpmh-lock-test.exe"
if errorlevel 1 exit /b 1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0make-dpu-transaction-test.ps1" -OutputDirectory "%TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX /I"%TEST_OUTPUT%" "%~dp0dpu-transaction-test.c" /Fo"%TEST_OUTPUT%\dpu-transaction-test.obj" /Fe"%TEST_OUTPUT%\dpu-transaction-test.exe"
if errorlevel 1 exit /b 1
"%TEST_OUTPUT%\dpu-transaction-test.exe"
exit /b %errorlevel%
