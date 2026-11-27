@echo off
setlocal EnableExtensions DisableDelayedExpansion
for %%I in ("%~dp0..\..\..") do set "WOA_ROOT=%%~fI"
wsl.exe --cd "%WOA_ROOT%" --exec python3 src/qcom-npu/ggml/fetch.py --source
if errorlevel 1 exit /b 1
set "NPU_CHAT_BUILD=%WOA_ROOT%\out\qcom-npu\chat-build"
set "NPU_CHAT_CMAKE=%NPU_CHAT_BUILD%\build-tools\cmake\data\bin\cmake.exe"
set "NPU_CHAT_NINJA=%NPU_CHAT_BUILD%\build-tools\bin\ninja.exe"
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat" amd64_arm64 >nul 2>&1
if errorlevel 1 exit /b 1
"%NPU_CHAT_CMAKE%" -S "%NPU_CHAT_BUILD%\llama-src" -B "%NPU_CHAT_BUILD%\native" -G Ninja -DCMAKE_MAKE_PROGRAM="%NPU_CHAT_NINJA%" -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=ARM64 -DGGML_HEXAGON=ON -DGGML_NATIVE=ON -DGGML_MACHINE_SUPPORTS_dotprod=ON -DGGML_MACHINE_SUPPORTS_i8mm=ON -DGGML_MACHINE_SUPPORTS_fma=ON -DGGML_MACHINE_SUPPORTS_sve=OFF -DGGML_MACHINE_SUPPORTS_sme=OFF -DGGML_OPENMP=OFF -DGGML_BACKEND_DL=ON -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_CURL=OFF -DLLAMA_BUILD_SERVER=ON -DQCOM_NPU_SOURCE="%WOA_ROOT%\src\qcom-npu"
if errorlevel 1 exit /b 1
"%NPU_CHAT_CMAKE%" --build "%NPU_CHAT_BUILD%\native" --config Release --target llama-server ggml-hexagon --parallel 8
if errorlevel 1 exit /b 1
set "NPU_GGML_PACKAGE=%NPU_CHAT_BUILD%\package\GGML"
if not exist "%NPU_GGML_PACKAGE%" mkdir "%NPU_GGML_PACKAGE%"
copy /y "%NPU_CHAT_BUILD%\native\bin\ggml-hexagon.dll" "%NPU_GGML_PACKAGE%\qcom_npu_ggml.dll" >nul
if errorlevel 1 exit /b 1
copy /y "%NPU_CHAT_BUILD%\native\bin\ggml-base.dll" "%NPU_GGML_PACKAGE%\ggml-base.dll" >nul
if errorlevel 1 exit /b 1
for %%F in (msvcp140.dll vcruntime140.dll) do (
  copy /y "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Redist\MSVC\14.52.37025\arm64\Microsoft.VC145.CRT\%%F" "%NPU_GGML_PACKAGE%\%%F" >nul
  if errorlevel 1 exit /b 1
)
exit /b 0
