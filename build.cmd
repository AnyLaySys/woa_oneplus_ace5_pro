@echo off
setlocal EnableExtensions DisableDelayedExpansion
for %%I in ("%~dp0.") do set "ROOT=%%~fI"
if /i "%~1"=="--help" goto help
if /i "%~1"=="-h" goto help
if /i "%~1"=="/?" goto help
if /i "%~1"=="install-fw" goto install_fw
if not "%~2"=="" goto invalid
if /i "%~1"=="all" goto build_all
if /i "%~1"=="ollama" goto build_ollama
if /i "%~1"=="sdcpp" goto build_sdcpp
set "NAME=%~1"
set "TARGET="
if /i "%NAME%"=="syna-touch" (set "NAME=syna-touch"& set "TARGET=syna_touch")
if /i "%NAME%"=="qcom-audio" (set "NAME=qcom-audio"& set "TARGET=sia9177")
if /i "%NAME%"=="qcom-bt" (set "NAME=qcom-bt"& set "TARGET=qcom_bt")
if /i "%NAME%"=="qcom-wlan" (set "NAME=qcom-wlan"& set "TARGET=qcom_wlan")
if /i "%NAME%"=="qcom-charge" (set "NAME=qcom-audio"& set "TARGET=sia9177")
if /i "%NAME%"=="ti-batt" (set "NAME=ti-batt"& set "TARGET=ti_batt")
if /i "%NAME%"=="qcom-cpupwr" (set "NAME=qcom-cpupwr"& set "TARGET=qcom_cpupwr")
if /i "%NAME%"=="qcom-keys" (set "NAME=qcom-keys"& set "TARGET=qcom_keys")
if /i "%NAME%"=="qcom-sensors" (set "NAME=qcom-sensors"& set "TARGET=qcom_sensors")
if /i "%NAME%"=="nxp-nfc" (set "NAME=nxp-nfc"& set "TARGET=nxp_nfc")
if /i "%NAME%"=="jiiov-fp" (set "NAME=jiiov-fp"& set "TARGET=jiiov_fp")
if /i "%NAME%"=="qcom-ir" (set "NAME=qcom-ir"& set "TARGET=qcom_ir")
if /i "%NAME%"=="ir-remote" (set "NAME=ir-remote"& set "TARGET=ir_remote")
if /i "%NAME%"=="qcom-cam" (set "NAME=qcom-cam"& set "TARGET=qcom_cam")
if /i "%NAME%"=="qcom-gpu" (set "NAME=qcom-gpu"& set "TARGET=qcom_gpu")
if /i "%NAME%"=="qcom-npu" (set "NAME=qcom-npu"& set "TARGET=qcom_npu")
if not defined TARGET goto invalid
set "APP_PACKAGE="
if /i "%NAME%"=="ir-remote" set "APP_PACKAGE=1"
set "DRIVER_EXT=sys"
if /i "%NAME%"=="nxp-nfc" set "DRIVER_EXT=dll"
if defined APP_PACKAGE set "DRIVER_EXT=exe"
set "CREDENTIAL="
if /i "%NAME%"=="qcom-audio" (
  set "CREDENTIAL=%ROOT%\fw\charge\credential.bin"
  for %%F in ("%ROOT%\fw\charge\credential.bin") do if not "%%~zF"=="16" (
    echo ERROR fw\charge\credential.bin is missing or not exactly 16 bytes.
    exit /b 1
  )
)
set "SRC=%ROOT%\src\%NAME%"
if /i "%NAME%"=="ir-remote" set "SRC=%ROOT%\src\qcom-ir\app"
if /i "%NAME%"=="qcom-gpu" set "SRC=%ROOT%\src\qcom-gpu\wddm"
set "GPU_PACKAGE="
if /i "%NAME%"=="qcom-gpu" set "GPU_PACKAGE=1"
set "PROJECT="
set "PROJECT_COUNT=0"
for /r "%SRC%" %%P in (*.vcxproj) do if /i "%%~nxP"=="%TARGET%.vcxproj" (
  set "PROJECT=%%~fP"
  set /a PROJECT_COUNT+=1 >nul
)
if not "%PROJECT_COUNT%"=="1" (
  echo ERROR expected one %TARGET%.vcxproj under %SRC%
  exit /b 1
)
set "DST=%ROOT%\out\drv\%NAME%"
if defined APP_PACKAGE set "DST=%ROOT%\out\app\%NAME%"
if /i "%NAME%"=="qcom-npu" set "DST=%ROOT%\out\qcom-npu\driver\package"
set "WORK=%ROOT%\out\%NAME%\ARM64\Release"
if /i "%NAME%"=="qcom-npu" set "WORK=%ROOT%\out\qcom-npu\driver\build\ARM64\Release"
set "BIN=%WORK%\bin"
set "VS=C:\Program Files\Microsoft Visual Studio\18\Insiders"
set "MSB=%VS%\MSBuild\Current\Bin\MSBuild.exe"
set "KITBIN=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0"
set "SIGNTOOL=%KITBIN%\x64\signtool.exe"
set "INF2CAT=%KITBIN%\x86\Inf2Cat.exe"
set "CERT=FF7908A360512CF12E08C0BD99625830C1EA4E6D"
if defined WOA_SIGN_CERT set "CERT=%WOA_SIGN_CERT%"
if not exist "%MSB%" exit /b 1
if not exist "%DST%" mkdir "%DST%"
if not exist "%WORK%\obj" mkdir "%WORK%\obj"
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 exit /b 1
if /i "%NAME%"=="qcom-npu" (
  call :run_npu_build prepare
  if errorlevel 1 exit /b 1
  call :build_npu_runtime
  if errorlevel 1 exit /b 1
  call :build_npu_ggml
  if errorlevel 1 exit /b 1
  call :build_npu_ep
  if errorlevel 1 exit /b 1
)
set "GPU_RENDER_PACKAGE=%ROOT%\out\mesa\umd-package"
if defined GPU_PACKAGE call :build_gpu_renderer
if errorlevel 1 exit /b 1
if defined GPU_PACKAGE (
  if not exist "%BIN%" mkdir "%BIN%"
  for %%F in (a830_umd.dll a830_umd_arm64.dll a830_umd_x64.dll a830_encoder.dll a830_encoder_arm64.dll a830_encoder_x64.dll a830_decoder.dll a830_decoder_arm64.dll a830_decoder_x64.dll a830_gl.dll a830_gl_x64.dll vulkan_freedreno.dll vulkan_freedreno_x64.dll freedreno_icd.json freedreno_icd_x64.json msvcp140.dll vcruntime140.dll) do (
    if not exist "%GPU_RENDER_PACKAGE%\%%F" (
      echo ERROR GPU renderer output is missing: %GPU_RENDER_PACKAGE%\%%F
      exit /b 1
    )
    copy /y "%GPU_RENDER_PACKAGE%\%%F" "%BIN%\%%F" >nul
    if errorlevel 1 exit /b 1
  )
  copy /y "%ROOT%\fw\gpu\gen80000_zap.mbn" "%BIN%\gen80000_zap.mbn" >nul
  if errorlevel 1 exit /b 1
  copy /y "%ROOT%\fw\gpu\vpu35_4v.mbn" "%BIN%\vpu35_4v.mbn" >nul
  if errorlevel 1 exit /b 1
)
"%MSB%" "%PROJECT%" /t:Rebuild /p:Configuration=Release /p:Platform=ARM64 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:OutDir="%BIN%/" /p:IntDir="%WORK%\obj/" /p:SkipPackageVerification=true /p:SignMode=Off /v:minimal
if errorlevel 1 exit /b 1
if defined GPU_PACKAGE for %%E in (sys inf cat) do if exist "%DST%\*.%%E" del /q "%DST%\*.%%E"
if defined GPU_PACKAGE if exist "%DST%\deployment-manifest.json" del /q "%DST%\deployment-manifest.json"
copy /y "%BIN%\%TARGET%.%DRIVER_EXT%" "%DST%\%TARGET%.%DRIVER_EXT%" >nul
if errorlevel 1 exit /b 1
if not defined APP_PACKAGE (
  copy /y "%BIN%\%TARGET%.inf" "%DST%\%TARGET%.inf" >nul
  if errorlevel 1 exit /b 1
)
if defined GPU_PACKAGE (
  for %%F in (a830_umd.dll a830_umd_arm64.dll a830_umd_x64.dll a830_encoder.dll a830_encoder_arm64.dll a830_encoder_x64.dll a830_decoder.dll a830_decoder_arm64.dll a830_decoder_x64.dll a830_gl.dll a830_gl_x64.dll vulkan_freedreno.dll vulkan_freedreno_x64.dll) do (
    copy /y "%BIN%\%%F" "%DST%\%%F" >nul
    if errorlevel 1 exit /b 1
    "%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\%%F"
    if errorlevel 1 exit /b 1
  )
  for %%F in (freedreno_icd.json freedreno_icd_x64.json msvcp140.dll vcruntime140.dll gen80000_zap.mbn vpu35_4v.mbn) do (
    copy /y "%BIN%\%%F" "%DST%\%%F" >nul
    if errorlevel 1 exit /b 1
  )
)
"%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\%TARGET%.%DRIVER_EXT%"
if errorlevel 1 exit /b 1
if defined APP_PACKAGE goto build_done
if /i "%NAME%"=="qcom-cam" (
  for %%F in (qcom_cam_video.sys qcom_cam_video.inf) do (
    copy /y "%BIN%\%%F" "%DST%\%%F" >nul
    if errorlevel 1 exit /b 1
  )
  "%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\qcom_cam_video.sys"
  if errorlevel 1 exit /b 1
  copy /y "%BIN%\qcom_cam_check.exe" "%DST%\qcom_cam_check.exe" >nul
  if errorlevel 1 exit /b 1
  "%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\qcom_cam_check.exe"
  if errorlevel 1 exit /b 1
  copy /y "%BIN%\qcom_cam_capture.exe" "%DST%\qcom_cam_capture.exe" >nul
  if errorlevel 1 exit /b 1
  "%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\qcom_cam_capture.exe"
  if errorlevel 1 exit /b 1
)

if /i "%NAME%"=="qcom-sensors" (
  for %%F in (qcom_sensors_native.dll qcom_sensors_native.inf) do (
    copy /y "%BIN%\%%F" "%DST%\%%F" >nul
    if errorlevel 1 exit /b 1
  )
  "%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\qcom_sensors_native.dll"
  if errorlevel 1 exit /b 1
  copy /y "%BIN%\qcom_sensors.exe" "%DST%\qcom_sensors.exe" >nul
  if errorlevel 1 exit /b 1
  "%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\qcom_sensors.exe"
  if errorlevel 1 exit /b 1
)
if /i "%NAME%"=="qcom-audio" (
  if exist "%DST%\charge.exe" del /q "%DST%\charge.exe"
  copy /y "%CREDENTIAL%" "%DST%\credential.bin" >nul
  if errorlevel 1 exit /b 1
  if not exist "%DST%\ADSP" mkdir "%DST%\ADSP"
  copy /y "%ROOT%\fw\adsp\adsp.*" "%DST%\ADSP\" >nul
  if errorlevel 1 exit /b 1
  copy /y "%ROOT%\fw\adsp\adsp_dtb.*" "%DST%\ADSP\" >nul
  if errorlevel 1 exit /b 1
)
if /i "%NAME%"=="qcom-npu" (
  call :build_npu_mcdm
  if errorlevel 1 exit /b 1
  for %%F in (qcom_npu_mcdm.sys qcom_npu_mcdm.inf) do (
    copy /y "%WORK%\mcdm\%%F" "%DST%\%%F" >nul
    if errorlevel 1 exit /b 1
  )
  for %%F in (qcom_npu_ep.dll qcom_npu_rpc.dll) do (
    copy /y "%ROOT%\out\qcom-npu\driver\build\inference\runner\%%F" "%DST%\%%F" >nul
    if errorlevel 1 exit /b 1
  )
  for %%F in (qcom_npu_mcdm.sys qcom_npu_ep.dll qcom_npu_rpc.dll) do (
    "%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\%%F"
    if errorlevel 1 exit /b 1
  )
  for %%F in (QNN-LICENSE.pdf FastRPC-LICENSE.txt ONNX-Runtime-LICENSE.txt) do (
    copy /y "%ROOT%\out\qcom-npu\driver\build\inference\runner\%%F" "%DST%\%%F" >nul
    if errorlevel 1 exit /b 1
  )
  if not exist "%DST%\GGML" mkdir "%DST%\GGML"
  for %%F in (qcom_npu_ggml.dll ggml-base.dll msvcp140.dll vcruntime140.dll) do (
    copy /y "%ROOT%\out\qcom-npu\ggml\package\%%F" "%DST%\GGML\%%F" >nul
    if errorlevel 1 exit /b 1
  )
  for %%F in (qcom_npu_ggml.dll ggml-base.dll) do (
    "%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\GGML\%%F"
    if errorlevel 1 exit /b 1
  )
  if not exist "%DST%\DSP250" mkdir "%DST%\DSP250"
  for %%F in (fastrpc_shell_unsigned_3 libc++.so.1 libc++abi.so.1) do (
    copy /y "%ROOT%\fw\cdsp\user\%%F" "%DST%\DSP250\%%F" >nul
    if errorlevel 1 exit /b 1
  )
  for %%F in (libQnnHtpV79Skel.so libQnnHtpV79.so) do (
    copy /y "%ROOT%\out\qcom-npu\driver\build\inference\sdk\lib\hexagon-v79\unsigned\%%F" "%DST%\DSP250\%%F" >nul
    if errorlevel 1 exit /b 1
  )
  copy /y "%ROOT%\out\qcom-npu\ggml\runtime\libggml-htp-v79.so" "%DST%\DSP250\libggml-htp-v79.so" >nul
  if errorlevel 1 exit /b 1
  copy /y "%ROOT%\out\qcom-npu\ggml\runtime\GGML-LICENSE.txt" "%DST%\GGML-LICENSE.txt" >nul
  if errorlevel 1 exit /b 1
  if not exist "%DST%\CDSP" mkdir "%DST%\CDSP"
  copy /y "%ROOT%\fw\cdsp\cdsp.*" "%DST%\CDSP\" >nul
  if errorlevel 1 exit /b 1
  copy /y "%ROOT%\fw\cdsp\cdsp_dtb.*" "%DST%\CDSP\" >nul
  if errorlevel 1 exit /b 1
)
"%INF2CAT%" /driver:"%DST%" /os:10_CO_ARM64 /uselocaltime
if errorlevel 1 exit /b 1
"%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\%TARGET%.cat"
if errorlevel 1 exit /b 1
if /i "%NAME%"=="qcom-npu" (
  "%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\qcom_npu_mcdm.cat"
  if errorlevel 1 exit /b 1
  call :run_npu_build winml
  if errorlevel 1 exit /b 1
)
if /i "%NAME%"=="qcom-sensors" (
  "%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\qcom_sensors_native.cat"
  if errorlevel 1 exit /b 1
)
if /i "%NAME%"=="qcom-cam" (
  "%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\qcom_cam_video.cat"
  if errorlevel 1 exit /b 1
)
echo.
:build_done
if defined APP_PACKAGE (echo APP=%DST%) else echo DRIVER=%DST%
dir "%DST%" /b
exit /b 0

:build_ollama
call :run_npu_build ggml
if errorlevel 1 exit /b 1
call :build_npu_ggml server
if errorlevel 1 exit /b 1
call :run_npu_build ollama
if errorlevel 1 exit /b 1
set "OLLAMA_DST=%ROOT%\out\qcom-npu\ollama\package"
set "OLLAMA_SIGNTOOL=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
set "OLLAMA_SIGN_CERT=FF7908A360512CF12E08C0BD99625830C1EA4E6D"
if defined WOA_SIGN_CERT set "OLLAMA_SIGN_CERT=%WOA_SIGN_CERT%"
for %%F in (ollama.exe "ollama app.exe") do (
  "%OLLAMA_SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %OLLAMA_SIGN_CERT% "%OLLAMA_DST%\%%~F"
  if errorlevel 1 exit /b 1
)
for %%F in (llama-server.exe llama-server-impl.dll llama-common.dll llama.dll mtmd.dll ggml.dll ggml-base.dll ggml-cpu.dll) do (
  "%OLLAMA_SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %OLLAMA_SIGN_CERT% "%OLLAMA_DST%\lib\ollama\%%F"
  if errorlevel 1 exit /b 1
)
echo APP=%OLLAMA_DST%
exit /b 0

:build_sdcpp
call :run_npu_build sdcpp
if errorlevel 1 exit /b 1
set "SDCPP_DST=%ROOT%\out\qcom-npu\sdcpp\package"
set "SDCPP_SIGNTOOL=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
set "SDCPP_SIGN_CERT=FF7908A360512CF12E08C0BD99625830C1EA4E6D"
if defined WOA_SIGN_CERT set "SDCPP_SIGN_CERT=%WOA_SIGN_CERT%"
for %%F in (sd-cli.exe sd-server.exe ggml.dll ggml-base.dll ggml-cpu.dll ggml-hexagon.dll) do (
  "%SDCPP_SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %SDCPP_SIGN_CERT% "%SDCPP_DST%\%%F"
  if errorlevel 1 exit /b 1
)
echo APP=%SDCPP_DST%
exit /b 0

:build_all
for %%D in (syna-touch qcom-audio qcom-bt qcom-wlan ti-batt qcom-cpupwr qcom-keys qcom-sensors nxp-nfc jiiov-fp qcom-ir qcom-cam qcom-gpu qcom-npu) do (
  echo.
  echo Building %%D...
  call "%~f0" %%D
  if errorlevel 1 (
    echo ERROR build failed: %%D
    exit /b 1
  )
)
echo.
echo All drivers built successfully.
exit /b 0

:build_gpu_renderer
where.exe wsl.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR WSL is required to build the ARM64 GPU renderer.
  exit /b 1
)
if not defined WOA_WSL_DISTRO set "WOA_WSL_DISTRO=Ubuntu-26.10"
echo Building the ARM64 GPU renderer with %WOA_WSL_DISTRO%...
set "WOA_GPU_BUILD_ROOT=%ROOT%"
set "WOA_BUILD_ENTRY=%~f0"
powershell.exe -NoProfile -Command "$text=[IO.File]::ReadAllText($env:WOA_BUILD_ENTRY); $match=[regex]::Match($text,'(?ms)^#<GPU-BUILD>\r?\n(.*?)^#</GPU-BUILD>\r?$'); if(-not $match.Success){throw 'Missing GPU build body'}; $output=Join-Path $env:WOA_GPU_BUILD_ROOT 'out\mesa'; [IO.Directory]::CreateDirectory($output)|Out-Null; $utf8=[Text.UTF8Encoding]::new($false); [IO.File]::WriteAllText((Join-Path $output 'build.py'),$match.Groups[1].Value,$utf8); $tools=@{vc=$env:VCToolsInstallDir;sdk=$env:WindowsSdkDir;sdk_version=$env:WindowsSDKVersion;redist=$env:VCToolsRedistDir}; [IO.File]::WriteAllText((Join-Path $output 'toolchain.json'),($tools|ConvertTo-Json),$utf8); & wsl.exe --distribution $env:WOA_WSL_DISTRO --cd $env:WOA_GPU_BUILD_ROOT --exec python3 -B out/mesa/build.py; if($LASTEXITCODE -ne 0){exit 1}"
exit /b %errorlevel%

:run_npu_build
set "WOA_NPU_ROOT=%ROOT%"
set "WOA_BUILD_ENTRY=%~f0"
set "WOA_NPU_ACTION=%~1"
powershell.exe -NoProfile -Command "$text=[IO.File]::ReadAllText($env:WOA_BUILD_ENTRY); $match=[regex]::Match($text,'(?ms)^#<NPU-BUILD>\r?\n(.*?)^#</NPU-BUILD>\r?$'); if(-not $match.Success){throw 'Missing NPU build body'}; $output=Join-Path $env:WOA_NPU_ROOT 'out\qcom-npu'; [IO.Directory]::CreateDirectory($output)|Out-Null; [IO.File]::WriteAllText((Join-Path $output 'build.py'),$match.Groups[1].Value,[Text.UTF8Encoding]::new($false)); & wsl.exe --cd $env:WOA_NPU_ROOT --exec python3 -B out/qcom-npu/build.py $env:WOA_NPU_ACTION; if($LASTEXITCODE -ne 0){exit 1}"
exit /b %errorlevel%

:build_npu_runtime
setlocal EnableExtensions DisableDelayedExpansion
"%MSB%" "%ROOT%\src\qcom-npu\runtime\qcom_npu_rpc.vcxproj" /t:Build /p:Configuration=Release /p:Platform=ARM64 /p:OutDir="%ROOT%\out\qcom-npu\driver\build\inference\runner/" /p:IntDir="%ROOT%\out\qcom-npu\driver\build\inference\rpc-obj/" /v:minimal
if errorlevel 1 exit /b 1
exit /b 0

:build_npu_ggml
setlocal EnableExtensions DisableDelayedExpansion
set "NPU_GGML_BUILD=%ROOT%\out\qcom-npu\ggml\build"
set "NPU_GGML_CMAKE=%NPU_GGML_BUILD%\build-tools\cmake\data\bin\cmake.exe"
set "NPU_GGML_NINJA=%NPU_GGML_BUILD%\build-tools\bin\ninja.exe"
set "NPU_GGML_NATIVE=%NPU_GGML_BUILD%\native"
set "NPU_GGML_TARGETS=ggml-hexagon"
if /i "%~1"=="server" set "NPU_GGML_TARGETS=llama-server ggml-hexagon"
if /i "%~1"=="server" set "NPU_GGML_NATIVE=%ROOT%\out\qcom-npu\ollama\build\native"
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat" amd64_arm64 >nul 2>&1
if errorlevel 1 exit /b 1
"%NPU_GGML_CMAKE%" -S "%LOCALAPPDATA%\WoA\SourceCache\qcom-npu\llama" -B "%NPU_GGML_NATIVE%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NPU_GGML_NINJA%" -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=ARM64 -DGGML_HEXAGON=ON -DGGML_NATIVE=ON -DGGML_MACHINE_SUPPORTS_dotprod=ON -DGGML_MACHINE_SUPPORTS_i8mm=ON -DGGML_MACHINE_SUPPORTS_fma=ON -DGGML_MACHINE_SUPPORTS_sve=OFF -DGGML_MACHINE_SUPPORTS_sme=OFF -DGGML_OPENMP=OFF -DGGML_CCACHE=OFF -DGGML_BACKEND_DL=ON -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_CURL=OFF -DLLAMA_BUILD_SERVER=ON -DLLAMA_BUILD_APP=OFF -DLLAMA_BUILD_UI=OFF -DLLAMA_USE_PREBUILT_UI=OFF -DQCOM_NPU_SOURCE="%ROOT%\src\qcom-npu"
if errorlevel 1 exit /b 1
"%NPU_GGML_CMAKE%" --build "%NPU_GGML_NATIVE%" --config Release --target %NPU_GGML_TARGETS% --parallel 8
if errorlevel 1 exit /b 1
set "NPU_GGML_PACKAGE=%ROOT%\out\qcom-npu\ggml\package"
if not exist "%NPU_GGML_PACKAGE%" mkdir "%NPU_GGML_PACKAGE%"
copy /y "%NPU_GGML_NATIVE%\bin\ggml-hexagon.dll" "%NPU_GGML_PACKAGE%\qcom_npu_ggml.dll" >nul
if errorlevel 1 exit /b 1
copy /y "%NPU_GGML_NATIVE%\bin\ggml-base.dll" "%NPU_GGML_PACKAGE%\ggml-base.dll" >nul
if errorlevel 1 exit /b 1
for %%F in (msvcp140.dll vcruntime140.dll) do (
  copy /y "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Redist\MSVC\14.52.37025\arm64\Microsoft.VC145.CRT\%%F" "%NPU_GGML_PACKAGE%\%%F" >nul
  if errorlevel 1 exit /b 1
)
exit /b 0

:build_npu_ep
setlocal EnableExtensions DisableDelayedExpansion
set "NPU_BUILD_ROOT=%ROOT%\out\qcom-npu\driver\build\inference"
"%MSB%" "%ROOT%\src\qcom-npu\ep\provider.vcxproj" /t:Build /p:Configuration=Release /p:Platform=ARM64 /p:OutDir="%NPU_BUILD_ROOT%\runner/" /p:IntDir="%NPU_BUILD_ROOT%\ep-obj/" /v:minimal
if errorlevel 1 exit /b 1
copy /y "%ROOT%\out\qcom-npu\driver\build\inference\sdk\LICENSE.pdf" "%NPU_BUILD_ROOT%\runner\QNN-LICENSE.pdf" >nul
if errorlevel 1 exit /b 1
copy /y "%ROOT%\src\qcom-npu\runtime\vendor\LICENSE.txt" "%NPU_BUILD_ROOT%\runner\FastRPC-LICENSE.txt" >nul
if errorlevel 1 exit /b 1
copy /y "%ROOT%\src\qcom-npu\ep\include\LICENSE" "%NPU_BUILD_ROOT%\runner\ONNX-Runtime-LICENSE.txt" >nul
exit /b 0

:build_npu_mcdm
setlocal EnableExtensions DisableDelayedExpansion
"%MSB%" "%ROOT%\src\qcom-npu\mcdm\qcom_npu_mcdm.vcxproj" /t:Build /p:Configuration=Release /p:Platform=ARM64 /p:OutDir="%ROOT%\out\qcom-npu\driver\build\ARM64\Release\mcdm/" /p:IntDir="%ROOT%\out\qcom-npu\driver\build\ARM64\Release\mcdm-obj/" /p:SkipPackageVerification=true /p:SignMode=Off /v:minimal
if errorlevel 1 exit /b 1
exit /b 0

:install_fw
set "WOA_BUILD_ENTRY=%~f0"
set "WOA_ROOT=%ROOT%"
set "WOA_WINDOWS="
set "WOA_BLUETOOTH_ADDRESS="
set "WOA_FORCE=0"
set "WOA_WHATIF=0"
shift
:install_args
if "%~1"=="" goto install_run
if /i "%~1"=="--help" goto help
if /i "%~1"=="-WindowsDirectory" (
  if defined WOA_WINDOWS goto invalid
  if "%~2"=="" goto invalid
  set "WOA_WINDOWS=%~2"
  shift
  shift
  goto install_args
)
if /i "%~1"=="-BluetoothAddress" (
  if defined WOA_BLUETOOTH_ADDRESS goto invalid
  if "%~2"=="" goto invalid
  set "WOA_BLUETOOTH_ADDRESS=%~2"
  shift
  shift
  goto install_args
)
if /i "%~1"=="-Force" (
  set "WOA_FORCE=1"
  shift
  goto install_args
)
if /i "%~1"=="-WhatIf" (
  set "WOA_WHATIF=1"
  shift
  goto install_args
)
goto invalid
:install_run
if not defined WOA_WINDOWS goto invalid
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$text=[IO.File]::ReadAllText($env:WOA_BUILD_ENTRY); $marker=[regex]::Match($text,'(?m)^#<FW-INSTALL>\r?$'); if(-not $marker.Success){throw 'Missing firmware installer body'}; $code=$text.Substring($marker.Index+$marker.Length); & ([scriptblock]::Create($code)) -WindowsDirectory $env:WOA_WINDOWS -BluetoothAddress $env:WOA_BLUETOOTH_ADDRESS -Force:($env:WOA_FORCE -eq '1') -WhatIf:($env:WOA_WHATIF -eq '1')"
exit /b %errorlevel%

:invalid
echo ERROR invalid target or arguments
call :help
exit /b 2

:help
echo Usage: build.cmd all
echo        build.cmd syna-touch
echo        build.cmd qcom-audio
echo        build.cmd qcom-bt
echo        build.cmd qcom-wlan
echo        build.cmd qcom-charge
echo        build.cmd ti-batt
echo        build.cmd qcom-cpupwr
echo        build.cmd qcom-keys
echo        build.cmd qcom-sensors
echo        build.cmd nxp-nfc
echo        build.cmd jiiov-fp
echo        build.cmd qcom-ir
echo        build.cmd ir-remote
echo        build.cmd qcom-cam
echo        build.cmd qcom-gpu
echo        build.cmd qcom-npu
echo        build.cmd ollama
echo        build.cmd sdcpp
echo        build.cmd install-fw -WindowsDirectory "W:\Windows" [-BluetoothAddress "XX:XX:XX:XX:XX:XX"] [-WhatIf] [-Force]
echo all builds every driver.
echo qcom-charge builds qcom-audio with autonomous kernel charging.
echo qcom-gpu builds the Qualcomm Adreno 830 desktop driver, renderer and signed package.
echo GPU builds use WSL Ubuntu-26.10 by default. Set WOA_WSL_DISTRO to override it.
echo Firmware copy requires an explicit WindowsDirectory. Existing files require -Force.
echo Windows targets require a valid existing Bluetooth address.bin or -BluetoothAddress.
exit /b 0

#<NPU-BUILD>
from pathlib import Path
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
NPU = ROOT / 'src/qcom-npu'
CRT = Path('/mnt/c/Program Files/Microsoft Visual Studio/18/Insiders/VC/Redist/MSVC/14.52.37025/arm64/Microsoft.VC145.CRT')

def source_cache():
    local = subprocess.check_output(['/mnt/c/Windows/System32/cmd.exe', '/d', '/c', 'echo', '%LOCALAPPDATA%'], text=True).strip()
    return Path(subprocess.check_output(['wslpath', '-u', local], text=True).strip()) / 'WoA/SourceCache/qcom-npu'


def build_source(source, destination):
    shutil.copytree(source, destination, dirs_exist_ok=True, ignore=shutil.ignore_patterns('node_modules', 'dist', '__pycache__'))
    return destination


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def download(url, path, expected=None):
    path = Path(path)
    if path.is_file() and (not expected or digest(path) == expected): return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.download')
    subprocess.run(['curl', '--fail', '--location', '--retry', '3', '--max-time', '1800', '--silent', '--show-error', url, '--output', str(temporary)], check=True)
    if expected and digest(temporary) != expected: raise RuntimeError('Checksum mismatch: ' + path.name)
    temporary.replace(path)


def extract(archive, destination, prefix=''):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as source:
            for member in source.infolist():
                path = Path(member.filename)
                if path.is_absolute() or '..' in path.parts: raise RuntimeError('Unsafe archive path')
            source.extractall(destination)
        return
    with tarfile.open(archive, 'r:*') as source:
        if not prefix:
            source.extractall(destination, filter='data')
            return
        for member in source:
            if member.isdir() and member.name == prefix.rstrip('/'): continue
            if not member.name.startswith(prefix) or member.issym() or member.islnk(): raise RuntimeError('Unexpected archive member')
            path = Path(member.name[len(prefix):])
            if path.is_absolute() or '..' in path.parts: raise RuntimeError('Unsafe archive path')
            source.extract(member.replace(name=str(path)), destination, filter='data')


def windows(path):
    return subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip()

class RemoteZip(io.RawIOBase):
 def __init__(self, version):
  self.url=f"https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/{version}/v{version}.zip"
  r=urllib.request.urlopen(urllib.request.Request(self.url,headers={"Range":"bytes=0-15"}),timeout=30)
  self.size=int(r.headers["Content-Range"].split("/")[1]); r.close(); self.pos=0; self.cache={}
 def readable(self):return True
 def seekable(self):return True
 def tell(self):return self.pos
 def seek(self,n,whence=0):
  self.pos=n if whence==0 else self.pos+n if whence==1 else self.size+n
  return self.pos
 def read(self,n=-1):
  n=min(n if n>=0 else self.size-self.pos,self.size-self.pos);data=[]
  while n>0:
   block=self.pos//262144;off=self.pos%262144
   if block not in self.cache:
    a=block*262144;b=min(a+262143,self.size-1)
    r=urllib.request.urlopen(urllib.request.Request(self.url,headers={"Range":f"bytes={a}-{b}"}),timeout=30)
    if r.status!=206 or r.headers.get("Content-Range")!=f"bytes {a}-{b}/{self.size}":raise RuntimeError("Invalid range response")
    self.cache[block]=r.read();r.close()
   v=self.cache[block][off:off+n]
   if not v:raise RuntimeError("Short range response")
   data.append(v);self.pos+=len(v);n-=len(v)
  return b"".join(data)

def fetch_qairt():
    sdk = ROOT / "out/qcom-npu/driver/build/inference/sdk"
    version = "2.50.0.260828"
    files = {
     "lib/hexagon-v79/unsigned/libQnnHtpV79Skel.so": "238675526b2932efe83a66784937e310ce238e76577e16e9c4358e81711e5333",
     "lib/hexagon-v79/unsigned/libQnnHtpV79.so": "1691ce54f93d3624d374111e5264617e214ab7d05aee7ba1b4ea721ce127a1df",
    }
    missing = [name for name, sha in files.items() if not (sdk/name).is_file() or hashlib.sha256((sdk/name).read_bytes()).hexdigest() != sha]
    if not missing and (sdk/"LICENSE.pdf").is_file():
     print("V79 runtime files verified")
     return
    z = zipfile.ZipFile(RemoteZip(version))
    for name in missing + (["LICENSE.pdf"] if not (sdk/"LICENSE.pdf").is_file() else []):
     data = z.read("qairt/" + version + "/" + name)
     if name in files and hashlib.sha256(data).hexdigest() != files[name]: raise RuntimeError("Runtime hash mismatch: " + name)
     out = sdk/name; out.parent.mkdir(parents=True, exist_ok=True)
     temporary = out.with_name(out.name + ".new")
     temporary.write_bytes(data); temporary.replace(out)
     print("Downloaded", name)

def prepare_ggml(source):
    npu = NPU
    backend = source / "ggml/src/ggml-hexagon"
    shutil.copyfile(npu / "ggml/CMakeLists.txt", backend / "CMakeLists.txt")

    path = backend / "htp-drv.cpp"
    original = path.with_suffix(path.suffix + ".upstream")
    if not original.exists(): shutil.copyfile(path, original)
    text = original.read_text()
    text = '#include "installed.h"\n' + text
    text = text.replace("#include <domain.h>", "")
    text = text.replace("typedef int (*remote_system_request_pfn_t)(system_req_payload * req);", "")
    text = text.replace("remote_system_request_pfn_t   remote_system_request_pfn   = nullptr;", "")
    start = text.index("HTPDRV_API int remote_system_request(")
    end = text.index("\n#ifdef _WIN32", start)
    text = text[:start] + text[end:]
    start = text.index("static std::string get_driver_path()")
    end = text.index("\n#endif", start)
    text = text[:start] + '''static std::string get_driver_path() {
        try { return wstr_to_str(qcom_npu::InstalledDirectory().wstring()); }
        catch (...) { return {}; }
    }
    ''' + text[end:]
    text = text.replace('"libcdsprpc.dll"', '"qcom_npu_rpc.dll"')
    text = "\n".join(line for line in text.split("\n") if "dlsym(handle.get(), remote_system_request_pfn_t" not in line)
    start = text.index("domain * htpdrv_get_domain(")
    end = text.index("int htpdrv_get_arch(", start)
    text = text[:start] + text[end:]
    start = text.index("std::string htp_lib_dir()")
    end = text.index("int htpdrv_set_dsp_search_path(", start)
    text = text[:start] + 'std::string htp_lib_dir() { return get_driver_path() + "\\\\DSP250\\\\"; }\n\n' + text[end:]
    start = text.index("    fastrpc_manage_dsp_lib_path p {};")
    end = text.index("\nvoid htpdrv_set_dsp_env_path(", start)
    text = text[:start] + '    htpdrv_set_dsp_env_path(path, dev_name);\n    (void)domain_id;\n    return AEE_SUCCESS;\n}\n' + text[end:]
    path.write_text(text)

    path = backend / "htp-drv.h"
    original = path.with_suffix(path.suffix + ".upstream")
    if not original.exists(): shutil.copyfile(path, original)
    text = original.read_text().replace("HTPDRV_API domain * htpdrv_get_domain(int domain_id);", "")
    text = text.replace("HTPDRV_API int remote_system_request(system_req_payload * req);", "")
    text = text.replace("__declspec(dllexport) extern", "extern").replace("__declspec(dllimport) extern", "extern")
    path.write_text(text)

    path = backend / "ggml-hexagon.cpp"
    original = path.with_suffix(path.suffix + ".upstream")
    if not original.exists(): shutil.copyfile(path, original)
    text = '#include "memory-status.h"\n' + original.read_text()
    # Memory planning can optimize graphs before their buffers are allocated.
    text = text.replace("    ggml_hexagon_graph_repack(gf, sess);\n", "")
    text = text.replace("int    opt_power_mode = HTP_POWER_MODE_BURST;", "int    opt_power_mode = HTP_POWER_MODE_SUSTAINED_HIGH_PERFORMANCE;")
    text = text.replace('{ "default",                    HTP_POWER_MODE_BURST }', '{ "default",                    HTP_POWER_MODE_SUSTAINED_HIGH_PERFORMANCE }')
    text = text.replace("falling back to 'burst'", "falling back to 'sustained_high_performance'")
    text = text.replace("bool   opt_hostbuf    = true;", "bool   opt_hostbuf    = false;")
    text = text.replace("FASTRPC_MAP_FD_DELAYED_EXTENDED", "FASTRPC_MAP_FD_DELAYED")
    text = text.replace("size_t opt_mbuf_dyn    = 512ul * 1024 * 1024;", "size_t opt_mbuf_dyn    = 64ul * 1024 * 1024;")
    text = text.replace("size_t opt_mbuf_static = 1ul * 1024 * 1024 * 1024;", "size_t opt_mbuf_static = 256ul * 1024 * 1024;")
    text = text.replace("n_bufs_max = HTP_OP_MAX_BUFS;", "n_bufs_max = std::min<unsigned int>(HTP_OP_MAX_BUFS, QCOM_NPU_MODEL_BUFFERS);")
    # Repack in cached host memory, then copy once into the noncached DSP buffer.
    text = text.replace("    htp_repack_tensor_tiled(tensor, raw.data(), raw.size());", '''    std::vector<uint8_t> packed(ggml_backend_buft_get_alloc_size(tensor->buffer->buft, tensor));
        ggml_tensor target = *tensor;
        target.data = packed.data();
        htp_repack_tensor_tiled(&target, raw.data(), raw.size());
        memcpy(tensor->data, packed.data(), packed.size());''')
    start = text.index("static void ggml_backend_hexagon_device_get_memory(")
    end = text.index("static enum ggml_backend_dev_type", start)
    text = text[:start] + '''static void ggml_backend_hexagon_device_get_memory(ggml_backend_dev_t, size_t * free, size_t * total) {
        QCOM_NPU_MEMORY_STATUS state{};
        *free = *total = 0;
        if (!qcom_npu::MemoryStatus(state)) return;
        *total = opt_mbuf_total ? std::min<uint64_t>(state.Budget, opt_mbuf_total) : state.Budget;
        *free = std::min<uint64_t>(state.Available, *total);
        MEMORYSTATUSEX system{}; system.dwLength = sizeof(system);
        if (GlobalMemoryStatusEx(&system)) {
            const uint64_t reserve = 256ul * 1024 * 1024;
            *free = std::min<uint64_t>(*free, system.ullAvailPhys > reserve ? system.ullAvailPhys - reserve : 0);
        }
    }

    ''' + text[end:]
    text = text.replace('return "Hexagon";', 'return "Qualcomm Hexagon V79 NPU";')
    # A tensor and its guard page must fit one transport allocation.
    start = text.index("static bool ggml_backend_hexagon_device_supports_op(")
    end = text.index("static bool ggml_backend_hexagon_device_supports_buft(", start)
    text = text[:start] + text[start:end].replace("sess->max_vmem", "std::min<size_t>(sess->max_vmem, QCOM_NPU_MAX_SHARED - 4096)") + text[end:]
    text = text.replace("    this->op_batch = new ggml_hexagon_opbatch", '''    QCOM_NPU_MEMORY_STATUS memory{};
        if (qcom_npu::MemoryStatus(memory) && memory.WindowBytes) {
            this->max_vmem = std::min<size_t>(this->max_vmem, memory.WindowBytes - 64ul * 1024 * 1024);
        }
        this->op_batch = new ggml_hexagon_opbatch''')
    start = text.index("static void ggml_hexagon_resolve_device_domain(")
    end = text.index("static void ggml_hexagon_init(", start)
    text = text[:start] + '''static void ggml_hexagon_discover_devices() {
        for (size_t i = 0; i < opt_ndev; ++i) {
            auto& cfg = opt_device_configs[i];
            cfg.domain_id = cfg.physical_idx == 0 && cfg.virtual_idx == 0 ? CDSP_DOMAIN_ID : -1;
            cfg.domain_name = cfg.domain_id == CDSP_DOMAIN_ID ? CDSP_DOMAIN_NAME : "";
        }
    }

    ''' + text[end:]
    path.write_text(text)

    path = source / "src/llama-model.cpp"
    original = path.with_suffix(path.suffix + ".upstream")
    if not original.exists(): shutil.copyfile(path, original)
    text = original.read_text().replace("    pimpl->dev_input = { cpu_dev, &pimpl->cpu_buft_list };", '''    pimpl->dev_input = { cpu_dev, &pimpl->cpu_buft_list };
        const char * strict = getenv("GGML_NPU_REQUIRE_FULL_OFFLOAD");
        if (strict && strcmp(strict, "1") == 0 && act_gpu_layers > 0) {
            auto input = get_layer_buft_list(0);
            if (strncmp(ggml_backend_dev_name(input.dev), "HTP", 3) == 0) pimpl->dev_input = input;
        }''')
    path.write_text(text)

    path = source / "ggml/src/ggml-backend.cpp"
    original = path.with_suffix(path.suffix + ".upstream")
    if not original.exists(): shutil.copyfile(path, original)
    text = original.read_text()
    text = text.replace("    return ggml_backend_sched_compute_splits(sched);", '''    const char * strict = getenv("GGML_NPU_REQUIRE_FULL_OFFLOAD");
        if (strict && strcmp(strict, "1") == 0) {
            for (int i = 0; i < ggml_graph_n_nodes(graph); i++) {
                ggml_tensor * node = ggml_graph_node(graph, i);
                if (ggml_op_is_empty(node->op) || ggml_is_empty(node)) continue;
                ggml_backend_t backend = ggml_backend_sched_get_tensor_backend(sched, node);
                if (!backend || strncmp(ggml_backend_dev_name(ggml_backend_get_device(backend)), "HTP", 3) != 0) {
                    GGML_LOG_ERROR("NPU cannot execute %s (%s); CPU fallback is disabled\\n", node->name, ggml_op_desc(node));
                    return GGML_STATUS_FAILED;
                }
            }
        }
        return ggml_backend_sched_compute_splits(sched);''')
    path.write_text(text)

    # Host layout calculations need the size of FP16 storage, not FP16 arithmetic.
    for name in ["flash-attn-ops.h", "gated-delta-net-ops.h", "matmul-ops.h", "unary-ops.h", "cpy-ops.h", "upscale-ops.h"]:
        path = backend / "htp" / name
        original = path.with_suffix(path.suffix + ".upstream")
        if not original.exists(): shutil.copyfile(path, original)
        text = original.read_text().replace("sizeof(__fp16)", "sizeof(uint16_t)").replace("sizeof(_Float16)", "sizeof(uint16_t)")
        text = text.replace("__builtin_ctz(", "NpuCountTrailingZeros(")
        text = text.replace("struct __attribute__((aligned(128)))", "struct alignas(128)")
        path.write_text(text)

    # An exported source tree lives inside the WoA repository; pin its own identity.
    (source / "cmake/build-info.cmake").write_text('''set(BUILD_NUMBER 12359)
    set(BUILD_COMMIT "b1b770d")
    set(BUILD_COMPILER "${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
    set(BUILD_TARGET "${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
    ''')

GGML_RELEASE = 'b12359'
GGML_COMMIT = 'b1b770d90d94cad9e9ea5b993bdbc383a522fb28'
GGML_RUNTIME_HASH = 'b9967897ab8ca0cad3b465929f88266aa589df01495982d6c0a914146fb9e18b'
GGML_ARCHIVE_HASH = '8183b26d575a74fbc3ea75214669b622eb01b309013b0349814f7bad4dfe01e1'

def ggml_runtime():
    target = ROOT / 'out/qcom-npu/ggml/runtime'
    library = target / 'libggml-htp-v79.so'
    if library.is_file() and digest(library) == GGML_RUNTIME_HASH and (target / 'GGML-LICENSE.txt').is_file():
        return
    archive = target / f'llama-{GGML_RELEASE}-bin-android-arm64-snapdragon.tar.gz'
    download(f'https://github.com/ggml-org/llama.cpp/releases/download/{GGML_RELEASE}/{archive.name}', archive, GGML_ARCHIVE_HASH)
    with tarfile.open(archive, 'r:gz') as tar:
        for member, output in [(f'llama-{GGML_RELEASE}/lib/libggml-htp-v79.so', library), (f'llama-{GGML_RELEASE}/LICENSE', target / 'GGML-LICENSE.txt')]:
            with tar.extractfile(member) as stream:
                output.write_bytes(stream.read())
    if digest(library) != GGML_RUNTIME_HASH:
        raise RuntimeError('V79 GGML runtime hash mismatch')

def ggml_source():
    build = ROOT / 'out/qcom-npu/ggml/build'
    cache = source_cache()
    tree = cache / 'llama'
    stamp = tree / '.npu-source'
    if not stamp.is_file() or stamp.read_text().strip() != GGML_COMMIT:
        archive = cache / f'llama-{GGML_COMMIT}.tar.gz'
        download(f'https://codeload.github.com/ggml-org/llama.cpp/tar.gz/{GGML_COMMIT}', archive)
        extract(archive, tree, 'llama.cpp-' + GGML_COMMIT + '/')
        stamp.write_text(GGML_COMMIT + '\n')
    packages = [('cmake', '4.4.5', 'cmake-4.4.5-py3-none-win_amd64.whl', 'd5521b4a9e864505c550abae99ff593d501bd08225583e69b3a9a4ca7f36a03f'), ('ninja', '1.13.2', 'ninja-1.13.2-py3-none-win_amd64.whl', '1293f4078278b70d0ee4b6cc8f3a9e030656c9b2f59909970343c4fe76070118')]
    for name, version, filename, sha in packages:
        wheel = build / 'downloads' / filename
        if not wheel.is_file() or digest(wheel) != sha:
            metadata = build / 'downloads' / (name + '.json')
            download(f'https://pypi.org/pypi/{name}/{version}/json', metadata)
            item = next((item for item in json.loads(metadata.read_text())['urls'] if item['filename'] == filename))
            download(item['url'], wheel, sha)
        executable = build / 'build-tools' / ('cmake/data/bin/cmake.exe' if name == 'cmake' else 'ninja-1.13.2.data/scripts/ninja.exe')
        if not executable.is_file():
            extract(wheel, build / 'build-tools')
    binary = build / 'build-tools/bin/ninja.exe'
    binary.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(build / 'build-tools/ninja-1.13.2.data/scripts/ninja.exe', binary)
    prepare_ggml(tree)
    return tree

def fetch_ggml():
    ggml_runtime()
    return ggml_source()

def build_ollama():
    HERE = NPU / 'ollama'
    CACHE = Path.home() / '.cache/qcom-npu/ollama'
    BUILD = ROOT / 'out/qcom-npu/ollama/build'
    PACKAGE = ROOT / 'out/qcom-npu/ollama/package'
    VERSION = '0.40.18'
    SOURCE_SHA = '205298799e63978c78b18d9bef2f3e0625e02972364956ce5aefc3c95b36bf29'

    def toolchain(name, url, sha, directory):
        archive = CACHE / name
        download(url, archive, sha)
        target = CACHE / 'toolchains' / directory
        stamp = target.with_name(target.name + '.stamp')
        if not target.is_dir() or not stamp.is_file() or stamp.read_text().strip() != sha:
            target.mkdir(parents=True, exist_ok=True)
            extract(archive, target.parent)
            stamp.write_text(sha)
        return target

    def source():
        archive = CACHE / ('v' + VERSION + '.tar.gz')
        download(f'https://codeload.github.com/ollama/ollama/tar.gz/refs/tags/v{VERSION}', archive, SOURCE_SHA)
        tree = source_cache() / 'ollama'
        stamp = tree / '.npu-source'
        patch = HERE / 'backend.patch'
        identity = SOURCE_SHA + ':' + digest(patch)
        if not stamp.is_file() or stamp.read_text().strip() != identity:
            extract(archive, tree, f'ollama-{VERSION}/')
            subprocess.run(['patch', '--batch', '-p1', '-i', str(patch)], cwd=tree, check=True)
            stamp.write_text(identity)
        for name in ('qcom_npu_windows.go', 'qcom_npu_other.go'):
            shutil.copyfile(HERE / name, tree / 'ml' / name)
        return tree
    tree = build_source(source(), BUILD / 'source')
    go = toolchain('go1.26.10.linux-amd64.tar.gz', 'https://go.dev/dl/go1.26.10.linux-amd64.tar.gz', 'ccfe7ae822d2c2f41f4fe7c956199967a4dac22d0ee0d2b2bfdb440aab3b20c5', 'go')
    name = 'llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64'
    llvm = toolchain(name + '.tar.xz', f'https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/{name}.tar.xz', 'e7a15a49a2ea95e9f3d8a9d0cfabe5052a4b10fb4777cc54de5a8c27b64bb701', name)
    env = dict(os.environ, GOOS='windows', GOARCH='arm64', CGO_ENABLED='1', CC=str(llvm / 'bin/aarch64-w64-mingw32-clang'), CXX=str(llvm / 'bin/aarch64-w64-mingw32-clang++'), GOMODCACHE=str(CACHE / 'go-modules'), GOCACHE=str(BUILD / 'go-cache'), GOTMPDIR=str(BUILD / 'temp'))
    (BUILD / 'temp').mkdir(parents=True, exist_ok=True)
    headers = BUILD / 'headers'
    headers.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(llvm / 'generic-w64-mingw32/include/eventtoken.h', headers / 'EventToken.h')
    env['CGO_CXXFLAGS'] = env.get('CGO_CXXFLAGS', '') + ' -I' + str(headers)
    ui = tree / 'app/ui/app'
    if not (ui / 'dist/index.html').is_file():
        subprocess.run(['npm', 'ci', '--no-audit', '--no-fund'], cwd=ui, check=True)
        subprocess.run(['npm', 'run', 'build'], cwd=ui, check=True)
    PACKAGE.mkdir(parents=True, exist_ok=True)
    exe = go / 'bin/go'
    subprocess.run([str(exe), 'build', '-trimpath', '-ldflags', f'-s -w -X github.com/ollama/ollama/version.Version={VERSION}+qcom.npu.10', '-o', str(PACKAGE / 'ollama.exe'), '.'], cwd=tree, env=env, check=True)
    subprocess.run([str(exe), 'build', '-trimpath', '-ldflags', f'-s -w -H windowsgui -X github.com/ollama/ollama/app/version.Version={VERSION}', '-o', str(PACKAGE / 'ollama app.exe'), './app/cmd/app'], cwd=tree, env=env, check=True)
    native = BUILD / 'native/bin'
    libraries = PACKAGE / 'lib/ollama'
    libraries.mkdir(parents=True, exist_ok=True)
    for name in ('llama-server.exe', 'llama-server-impl.dll', 'llama-common.dll', 'llama.dll', 'mtmd.dll', 'ggml.dll', 'ggml-base.dll', 'ggml-cpu.dll'):
        shutil.copyfile(native / name, libraries / name)
    for name in ('msvcp140.dll', 'vcruntime140.dll'):
        shutil.copyfile(CRT / name, libraries / name)
    shutil.copyfile(tree / 'LICENSE', PACKAGE / 'Ollama-LICENSE.txt')
    shutil.copyfile(ROOT / 'out/qcom-npu/ggml/runtime/GGML-LICENSE.txt', PACKAGE / 'GGML-LICENSE.txt')
    shutil.copyfile(HERE / 'install.ps1', PACKAGE / 'install.ps1')
    print('Ollama package:', PACKAGE)

def build_sdcpp():
    HERE = NPU / 'sdcpp'
    BUILD = ROOT / 'out/qcom-npu/sdcpp/build'
    CACHE = Path.home() / '.cache/qcom-npu/image'
    PACKAGE = ROOT / 'out/qcom-npu/sdcpp/package'
    COMMIT = 'e637c7bccd35aa8841d67fb942ba72918176826c'
    UI_COMMIT = 'dd77dc8255bfd2371f733b894f7f773d05b92aa0'
    MODEL_REV = '3144d898d61492f8382ffcabec055733fc5b2a0e'
    MODEL_URL = f'https://huggingface.co/concedo/sdxs-512-tinySDdistilled-GGUF/resolve/{MODEL_REV}'

    def source(name, repo, commit, sha, patch=None):
        cache = source_cache()
        archive = cache / (name + '.tar.gz')
        download(f'https://codeload.github.com/leejet/{repo}/tar.gz/{commit}', archive, sha)
        tree = cache / name
        stamp = tree / '.npu-source'
        identity = sha + (':' + digest(patch) if patch else '')
        if not stamp.is_file() or stamp.read_text().strip() != identity:
            extract(archive, tree, f'{repo}-{commit}/')
            if patch:
                subprocess.run(['patch', '--batch', '-p1', '-i', str(patch)], cwd=tree, check=True)
            stamp.write_text(identity)
        return tree
    fetch_ggml()
    tree = build_source(source('sdcpp', 'stable-diffusion.cpp', COMMIT, 'b4ee260e48667e96cf6065dc1e116007c63dcebcccb8ecc97e6b9c77edd94979', HERE / 'backend.patch'), BUILD / 'source')
    ui = build_source(source('sdcpp-webui', 'sdcpp-webui', UI_COMMIT, 'a4c8a1459f6b9b9ae7f5740d828521123ee070c3444c19f78a58f038d6de30cf'), BUILD / 'frontend')
    header = ui / 'dist/gen_index_html.h'
    if not header.is_file():
        for args in [('install', '--frozen-lockfile'), ('run', 'build'), ('run', 'build:header')]:
            subprocess.run(['npx', '--yes', 'pnpm@10.15.1', *args], cwd=ui, check=True)
    target = tree / 'examples/server/frontend/dist/gen_index_html.h'
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(header, target)
    ggml = ROOT / 'out/qcom-npu/ggml/build'
    cmake = ggml / 'build-tools/cmake/data/bin/cmake.exe'
    options = {'CMAKE_MAKE_PROGRAM': windows(ggml / 'build-tools/bin/ninja.exe'), 'CMAKE_BUILD_TYPE': 'Release', 'CMAKE_SYSTEM_NAME': 'Windows', 'CMAKE_SYSTEM_PROCESSOR': 'ARM64', 'CMAKE_PROJECT_INCLUDE': '', 'GIT_EXE:FILEPATH': 'OFF', 'SDCPP_BUILD_COMMIT': COMMIT[:7], 'SDCPP_BUILD_VERSION': 'master-1141-e637c7b', 'SD_GGML_SOURCE_DIR': windows(source_cache() / 'llama/ggml'), 'SD_USE_SYSTEM_GGML': 'OFF', 'SD_USE_UPSTREAM_GGML': 'ON', 'SD_BUILD_SHARED_GGML_LIB': 'ON', 'BUILD_SHARED_LIBS': 'ON', 'GGML_HEXAGON': 'ON', 'GGML_BACKEND_DL': 'ON', 'GGML_OPENMP': 'OFF', 'GGML_CCACHE': 'OFF', 'GGML_NATIVE': 'ON', 'GGML_MACHINE_SUPPORTS_dotprod': 'ON', 'GGML_MACHINE_SUPPORTS_i8mm': 'ON', 'GGML_MACHINE_SUPPORTS_fma': 'ON', 'GGML_MACHINE_SUPPORTS_sve': 'OFF', 'GGML_MACHINE_SUPPORTS_sme': 'OFF', 'QCOM_NPU_SOURCE': windows(HERE.parent), 'SD_WEBP': 'OFF', 'SD_WEBM': 'OFF', 'SD_SERVER_BUILD_FRONTEND': 'ON', 'PNPM_EXECUTABLE:FILEPATH': 'OFF'}
    flags = ' '.join((f'-D{key}="{value}"' for key, value in options.items()))
    script = BUILD / 'build.cmd'
    script.write_text(f'''@echo off\nsetlocal\ncall "C:\\Program Files\\Microsoft Visual Studio\\18\\Insiders\\VC\\Auxiliary\\Build\\vcvarsall.bat" amd64_arm64 >nul 2>&1\nif errorlevel 1 exit /b 1\n"{windows(cmake)}" -S "{windows(tree)}" -B "{windows(BUILD / 'native')}" -G Ninja {flags}\nif errorlevel 1 exit /b 1\n"{windows(cmake)}" --build "{windows(BUILD / 'native')}" --target sd-cli sd-server --parallel 8\n''')
    subprocess.run(['/mnt/c/Windows/System32/cmd.exe', '/d', '/c', windows(script)], check=True)
    PACKAGE.mkdir(parents=True, exist_ok=True)
    for name in ('sd-cli.exe', 'sd-server.exe', 'ggml.dll', 'ggml-base.dll', 'ggml-cpu.dll', 'ggml-hexagon.dll'):
        shutil.copyfile(BUILD / 'native/bin' / name, PACKAGE / name)
    for name in ('msvcp140.dll', 'vcruntime140.dll'):
        shutil.copyfile(CRT / name, PACKAGE / name)
    for src, name in ((tree / 'LICENSE', 'StableDiffusionCpp-LICENSE.txt'), (ui / 'LICENSE', 'WebUI-LICENSE.txt'), (ROOT / 'out/qcom-npu/ggml/runtime/GGML-LICENSE.txt', 'GGML-LICENSE.txt')):
        shutil.copyfile(src, PACKAGE / name)
    for name in ('start-server.local.bat', 'open.ps1', 'install.ps1', 'README.md'):
        shutil.copyfile(HERE / name, PACKAGE / name)
    shutil.copyfile(tree / 'start-server.bat', PACKAGE / 'start-server.bat')
    model = CACHE / 'models/sdxs-512-q8.gguf'
    download(MODEL_URL + '/sdxs-512-tinySDdistilled_Q8_0.gguf', model, '409ab23582ee074c6b9d5395784fc0741b0599fb9d138686c69087c71678eb6a')
    models = PACKAGE / 'models/checkpoints'
    models.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(model, models / 'SDXS-512-Q8_0.gguf')
    card = CACHE / 'models/SDXS-model-card.md'
    download(MODEL_URL + '/README.md', card)
    shutil.copyfile(card, PACKAGE / 'SDXS-model-card.md')
    license = CACHE / 'models/SDXS-LICENSE.txt'
    download('https://huggingface.co/stabilityai/stable-diffusion-xl-base-1.0/resolve/462165984030d82259a11f4367a4eed129e94a7b/LICENSE.md', license, '19b6998b569b53ac1fc2158a8a3202c8699a9a4605b47075715d9c96be7fb6d0')
    shutil.copyfile(license, PACKAGE / 'SDXS-LICENSE.txt')
    print('Stable Diffusion package:', PACKAGE)

def package_winml():
    HERE = NPU / 'ep'
    KIT = Path('/mnt/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64')
    package = ROOT / 'out/qcom-npu/driver/package'
    stage = ROOT / 'out/qcom-npu/driver/build/winml'
    (stage / 'EP').mkdir(parents=True, exist_ok=True)
    version = re.search(r'DriverVer=[^,]+,([\d.]+)', (HERE.parent / 'kmdf/qcom_npu.inx').read_text()).group(1)
    (stage / 'AppxManifest.xml').write_text((HERE / 'AppxManifest.xml').read_text().replace('@VERSION@', version))
    shutil.copyfile(package / 'qcom_npu_ep.dll', stage / 'EP/qcom_npu_ep.dll')
    shutil.copyfile(HERE / 'logo.png', stage / 'logo.png')
    output = package / 'qcom_npu_ep.msix'
    subprocess.run([str(KIT / 'MakeAppx.exe'), 'pack', '/o', '/d', windows(stage), '/p', windows(output)], check=True)
    subprocess.run([str(KIT / 'signtool.exe'), 'sign', '/q', '/fd', 'sha256', '/s', 'My', '/sha1',
        os.environ.get('WOA_SIGN_CERT', 'FF7908A360512CF12E08C0BD99625830C1EA4E6D'), windows(output)], check=True)

def prepare_driver():
    fetch_qairt()
    fetch_ggml()

{'prepare': prepare_driver, 'ggml': fetch_ggml, 'ollama': build_ollama,
 'sdcpp': build_sdcpp, 'winml': package_winml}[sys.argv[1]]()
#</NPU-BUILD>

#<GPU-BUILD>
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
import tarfile
import urllib.request

root = Path(__file__).resolve().parents[2]
local = root / 'src/mesa'
output = root / 'out/mesa'
source = output / 'source'
build = output / 'arm64'
package = output / 'umd-package'
mesa_version = '26.2.4'
mesa_commit = '96cb43121031992b85767f9c1be8f3f48e22b1d2'
mesa_sha256 = 'bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9'
vkd3d_version = '1.14'
vkd3d_sha256 = 'c94015d59ebe8ae72c4d802af5ca1517d9744548842a4e776534612a8d905485'


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def write(path, text):
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def copy(original, destination):
    if destination.exists():
        if original.read_bytes() == destination.read_bytes():
            return
        destination.chmod(0o644)
    shutil.copyfile(original, destination)


def download(url, path, checksum):
    if not path.exists():
        print('Downloading ' + url, flush=True)
        temporary = path.with_name(path.name + '.part')
        with urllib.request.urlopen(url, timeout=60) as response, temporary.open('wb') as target:
            shutil.copyfileobj(response, target)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != checksum:
            raise ValueError('Unexpected source archive: ' + str(temporary))
        temporary.replace(path)
    elif hashlib.sha256(path.read_bytes()).hexdigest() != checksum:
        raise ValueError('Unexpected source archive: ' + str(path))


if not source.exists():
    archive = output / ('mesa-' + mesa_version + '.tar.xz')
    download('https://archive.mesa3d.org/' + archive.name, archive, mesa_sha256)
    print('Extracting ' + archive.name, flush=True)
    with tarfile.open(archive) as archive_file:
        archive_file.extractall(output, filter='data')
    (output / ('mesa-' + mesa_version)).rename(source)
    (source / '.woa-upstream').write_text(mesa_sha256)
    run('git', '-C', source, 'init', '--quiet')
marker = source / '.woa-upstream'
if marker.exists():
    if marker.read_text().strip() != mesa_sha256:
        raise ValueError('Unexpected Mesa source archive')
elif subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip() != mesa_commit:
    raise ValueError('Unexpected Mesa revision')

patch = local / 'mesa.patch'
if subprocess.run(['git', '-C', str(source), 'apply', '--ignore-whitespace', '--reverse', '--check', str(patch)], capture_output=True).returncode:
    run('git', '-C', source, 'apply', '--ignore-whitespace', '--check', patch)
    run('git', '-C', source, 'apply', '--ignore-whitespace', patch)
vulkan = source / 'src/freedreno/vulkan'
for name in ['tu_knl_windows.cc', 'tu_wsi_windows.cc', 'tu_wsi_windows.h', 'a830_composition.h']:
    copy(local / name, vulkan / name)
abi = (root / 'src/qcom-gpu/wddm/abi.h').read_text().replace('#include "../uapi.h"', '#include "qcom_gpu_uapi.h"')
for backend in [vulkan, source / 'src/freedreno/drm']:
    for name in ['a830_windows.h', 'a830_host.h', 'a830_completion.h']:
        copy(local / name, backend / name)
    copy(root / 'src/qcom-gpu/uapi.h', backend / 'qcom_gpu_uapi.h')
    write(backend / 'a830_wddm_abi.h', abi)
copy(local / 'fd_windows.c', source / 'src/freedreno/drm/fd_windows.c')

toolchain = json.loads((output / 'toolchain.json').read_text())
def linux_path(path):
    return Path(subprocess.check_output(['wslpath', '-u', path], text=True).strip())

vc = linux_path(toolchain['vc'])
sdk = linux_path(toolchain['sdk'])
sdk_version = toolchain['sdk_version'].rstrip('\\/')
headers = [vc / 'include'] + [sdk / 'Include' / sdk_version / name for name in ['ucrt', 'shared', 'um', 'winrt']]
compiler = ['clang-cl', '--target=aarch64-pc-windows-msvc', '-fuse-ld=lld']
for header in headers:
    compiler += ['/imsvc', str(header)]
link = ['/LIBPATH:' + str(path) for path in [
    vc / 'lib/arm64', sdk / 'Lib' / sdk_version / 'um/arm64', sdk / 'Lib' / sdk_version / 'ucrt/arm64']]
resource_compiler = ['llvm-rc']
for header in headers:
    resource_compiler += ['/I', str(header)]
cross = output / 'cross.ini'
write(cross, f"""[binaries]
c = {compiler!r}
cpp = {compiler!r}
ar = 'llvm-lib'
strip = 'llvm-strip'
windres = {resource_compiler!r}
[host_machine]
system = 'windows'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = []
cpp_args = []
c_link_args = {link!r}
cpp_link_args = {link!r}
""")
options = [
    '--buildtype=release', '--wrap-mode=nofallback', '--force-fallback-for=zlib,DirectX-Headers',
    '-Dplatforms=windows', '-Dvulkan-drivers=freedreno', '-Dfreedreno-kmds=windows',
    '-Dgallium-drivers=freedreno', '-Dgallium-wgl-dll-name=a830_gl', '-Dllvm=disabled', '-Dglx=disabled', '-Degl=disabled',
    '-Dgbm=disabled', '-Dgles1=disabled', '-Dgles2=disabled', '-Dopengl=true',
    '-Dshader-cache=disabled', '-Dexpat=disabled', '-Dzlib=enabled', '-Dzstd=disabled',
    '-Dbuild-tests=false', '-Dspirv-tools=disabled', '-Dtools=[]', '-Ddefault_library=static',
    '-Dtu-build-id=' + hashlib.sha256(mesa_commit.encode() + patch.read_bytes()).hexdigest()]
def build_mesa(directory, cross_file, config, *targets):
    setup = ['--reconfigure'] if (directory / 'build.ninja').exists() else []
    cached = directory / 'meson-private/cmd_line.txt'
    if setup and (not cached.exists() or str(cross_file) not in cached.read_text() or cross_file.stat().st_mtime > (directory / 'meson-private/coredata.dat').stat().st_mtime):
        setup = ['--wipe']
    run('meson', 'setup', *setup, directory, source, '--cross-file', cross_file, *config)
    run('ninja', '-C', directory, '-j', min(os.cpu_count() or 1, 8), *targets)


build_mesa(build, cross, options, 'src/freedreno/vulkan/vulkan_freedreno.dll', 'src/gallium/targets/wgl/a830_gl.dll')
package.mkdir(parents=True, exist_ok=True)
turnip = build / 'src/freedreno/vulkan/vulkan_freedreno.dll'
copy(turnip, package / turnip.name)
opengl = build / 'src/gallium/targets/wgl/a830_gl.dll'
copy(opengl, package / opengl.name)
crt = linux_path(toolchain['redist']) / 'arm64/Microsoft.VC145.CRT'
queue, seen = [turnip, opengl], set()
while queue:
    imports = subprocess.check_output(['llvm-readobj', '--coff-imports', str(queue.pop())], text=True)
    for name in re.findall(r'^  Name: (.+)$', imports, re.M):
        name = name.lower()
        if name in seen:
            continue
        seen.add(name)
        dependency = crt / name
        if dependency.is_file():
            copy(dependency, package / name)
            queue.append(dependency)
write(package / 'freedreno_icd.json', json.dumps({
    'file_format_version': '1.0.0', 'ICD': {'library_path': '.\\vulkan_freedreno.dll', 'api_version': '1.4.0'}}, indent=2) + '\n')
copy(source / 'docs/license.rst', package / 'LICENSE.Mesa')

shader_source = output / ('vkd3d-' + vkd3d_version)
archive = output / ('vkd3d-' + vkd3d_version + '.tar.xz')
download('https://dl.winehq.org/vkd3d/source/' + archive.name, archive, vkd3d_sha256)
if not shader_source.exists():
    with tarfile.open(archive) as archive_file:
        archive_file.extractall(output, filter='data')
umd = local / 'umd'
def build_forwarder(directory, target, definitions):
    directory.mkdir(parents=True, exist_ok=True)
    objects = []
    for arch, triple, machine in [('arm64', 'aarch64', 'arm64'), ('x64', 'arm64ec', 'x64')]:
        definition = directory / (arch + '.def')
        write(definition, definitions[arch])
        obj = directory / (arch + '.obj')
        run(*[arg.replace('aarch64-pc-windows-msvc', triple + '-pc-windows-msvc') for arg in compiler],
            '/c', '/Zl', '/GS-', '/Fo' + str(obj), local / 'd3d12_forward.c')
        library = directory / (arch + '.lib')
        run('lld-link', '/lib', '/machine:' + machine, '/def:' + str(definition), '/out:' + str(library))
        objects += [obj, library]
    run('lld-link', '/dll', '/noentry', '/nodefaultlib', '/machine:arm64x',
        '/defarm64native:' + str(directory / 'arm64.def'), '/def:' + str(directory / 'x64.def'),
        *objects, '/out:' + str(target), '/implib:' + str(directory / 'forward.lib'))


def build_umd(arch, compiler, link):
    umd_build = output / ('umd-' + arch)
    machine = 'X64' if arch == 'x64' else 'ARM64'
    umd_build.mkdir(parents=True, exist_ok=True)
    write(umd_build / 'config.h', f"""#define PACKAGE_VERSION "{vkd3d_version}"
    #define PACKAGE_STRING "vkd3d {vkd3d_version}"
    #define HAVE_BUILTIN_CLZ 1
    #define HAVE_BUILTIN_POPCOUNT 1
    #define HAVE_BUILTIN_ADD_OVERFLOW 1
    #define HAVE_ATOMIC_EXCHANGE_N 1
    #define HAVE_SYNC_ADD_AND_FETCH 1
    #define HAVE_SYNC_BOOL_COMPARE_AND_SWAP 1
    #define HAVE_SPIRV_UNIFIED1_SPIRV_H 1
    #define HAVE_SPIRV_UNIFIED1_GLSL_STD_450_H 1
    #define HAVE_STDINT_H 1
    #define HAVE_INTTYPES_H 1
    """)
    write(umd_build / 'unistd.h', '#pragma once\n#include <io.h>\n#include <process.h>\n#define isatty _isatty\n#define fileno _fileno\n#define getpid _getpid\n')
    write(umd_build / 'vkd3d_version.h', '#define VKD3D_VCS_ID "' + vkd3d_version + '"\n')
    spirv = umd_build / 'spirv/unified1'
    spirv.mkdir(parents=True, exist_ok=True)
    for name in ['spirv.h', 'GLSL.std.450.h']:
        copy(source / 'src/compiler/spirv' / name, spirv / name)
    shader = shader_source / 'libs/vkd3d-shader'
    for name in ['hlsl', 'preproc']:
        if not (umd_build / (name + '.tab.c')).exists():
            run('bison', '-d', '-o', umd_build / (name + '.tab.c'), shader / (name + '.y'))
            run('flex', '-o', umd_build / (name + '.yy.c'), shader / (name + '.l'))
    includes = [umd_build, shader_source / 'include', shader_source / 'include/private', shader,
                source / 'include', local, root / 'src/qcom-gpu/wddm', umd]
    args = ['/nologo', '/O2', '/MT' if arch == 'x64' else '/MD', '/DNDEBUG', '/D_CRT_SECURE_NO_WARNINGS', '/D_USE_MATH_DEFINES',
            '/DLIBVKD3D_SHADER_SOURCE', '/DVKD3D_NO_TRACE_MESSAGES', '/DVKD3D_NO_DEBUG_MESSAGES', '/DUSE_GCC_ATOMIC_BUILTINS', '/w']
    args += ['/I' + str(path) for path in includes]
    files = list(shader.glob('*.c'))
    files += [umd_build / (name + suffix) for name in ['hlsl', 'preproc'] for suffix in ['.tab.c', '.yy.c']]
    files += [shader_source / 'libs/vkd3d-common' / (name + '.c') for name in ['debug', 'memory', 'error', 'utf8']]


    def compile_file(path):
        obj = umd_build / (path.stem + '.obj')
        cpp = path.suffix == '.cpp'
        if cpp or not obj.exists() or obj.stat().st_mtime < path.stat().st_mtime:
            flags = ['/clang:-ferror-limit=8', '/std:c++20', '/EHa'] if cpp else ['/std:c11']
            options = [arg for arg in args if arg != '/w'] + ['/W4', '/WX', '/clang:-Wno-unused-parameter', '/clang:-Wno-missing-field-initializers'] if cpp else args
            result = subprocess.run(compiler + options + flags + ['/c', str(path), '/Fo' + str(obj)], capture_output=True, text=True)
            if result.returncode:
                print(result.stdout + result.stderr, flush=True)
                raise RuntimeError(str(path))
        return obj


    with ThreadPoolExecutor(max_workers=6) as pool:
        objects = list(pool.map(compile_file, files))
        objects += list(pool.map(compile_file, [
            umd / (name + '.cpp') for name in ['core', 'd3d11', 'driver', 'interop', 'pipeline', 'query', 'resource', 'shader', 'state', 'video']]))
    definition = umd_build / 'a830_umd.def'
    write(definition, 'LIBRARY a830_umd\nEXPORTS\nOpenAdapter10\nOpenAdapter10_2\n')
    run('lld-link', '/DLL', '/MACHINE:' + machine, '/OUT:' + str(umd_build / ('a830_umd_' + arch + '.dll')),
        '/DEF:' + str(definition), *link,
        *objects, 'kernel32.lib', 'user32.lib', 'gdi32.lib', 'advapi32.lib', 'oldnames.lib', 'uuid.lib')
    copy(umd_build / ('a830_umd_' + arch + '.dll'), package / ('a830_umd_' + arch + '.dll'))


def build_codec(name, arch, compiler, link):
    directory = output / (name + '-' + arch)
    directory.mkdir(parents=True, exist_ok=True)
    obj = directory / (name + '.obj')
    run(*compiler, '/nologo', '/O2', '/MT' if arch == 'x64' else '/MD', '/EHsc', '/std:c++17', '/W4', '/WX',
        '/DNDEBUG', '/DUNICODE', '/D_UNICODE', '/c', local / (name + '.cpp'), '/Fo' + str(obj))
    definition = directory / (name + '.def')
    write(definition, 'EXPORTS\nDllGetClassObject\nDllCanUnloadNow\n')
    dll = package / ('a830_' + name + '_' + arch + '.dll')
    run('lld-link', '/DLL', '/MACHINE:' + ('X64' if arch == 'x64' else 'ARM64'), '/OUT:' + str(dll),
        '/DEF:' + str(definition), *link, obj, 'kernel32.lib', 'gdi32.lib', 'advapi32.lib', 'ole32.lib',
        'oleaut32.lib', 'mfuuid.lib', 'mfplat.lib', 'mf.lib', 'uuid.lib', 'dxgi.lib')


build_umd('arm64', compiler, link)
build_codec('encoder', 'arm64', compiler, link)
build_codec('decoder', 'arm64', compiler, link)
copy(shader_source / 'COPYING', package / 'COPYING.vkd3d')
cross_x64 = output / 'cross-x64.ini'
write(cross_x64, cross.read_text().replace('aarch64', 'x86_64').replace("'arm64'", "'x86_64'").replace('/arm64', '/x64'))
options_x64 = [option.replace('a830_gl', 'a830_gl_x64') for option in options] + ['-Db_vscrt=mt']
build_x64 = output / 'x64'
build_mesa(build_x64, cross_x64, options_x64, 'src/freedreno/vulkan/vulkan_freedreno.dll', 'src/gallium/targets/wgl/a830_gl_x64.dll', 'src/gallium/targets/libgl-gdi/opengl32.dll')
build_umd('x64', [arg.replace('aarch64', 'x86_64') for arg in compiler],
             [arg.replace('/arm64', '/x64') for arg in link])
for codec in ['encoder', 'decoder']:
    build_codec(codec, 'x64', [arg.replace('aarch64', 'x86_64') for arg in compiler],
                [arg.replace('/arm64', '/x64') for arg in link])
build_forwarder(output / 'umd-forwarder', package / 'a830_umd.dll', {
    arch: 'EXPORTS\n' + ''.join(name + '=a830_umd_' + arch + '.' + name + '\n'
        for name in ['OpenAdapter10', 'OpenAdapter10_2']) for arch in ['arm64', 'x64']})
for codec in ['encoder', 'decoder']:
    build_forwarder(output / (codec + '-forwarder'), package / ('a830_' + codec + '.dll'), {
        arch: 'EXPORTS\n' + ''.join(name + '=a830_' + codec + '_' + arch + '.' + name + '\n'
            for name in ['DllGetClassObject', 'DllCanUnloadNow']) for arch in ['arm64', 'x64']})
copy(build_x64 / 'src/gallium/targets/wgl/a830_gl_x64.dll', package / 'a830_gl_x64.dll')
copy(build_x64 / 'src/freedreno/vulkan/vulkan_freedreno.dll', package / 'vulkan_freedreno_x64.dll')
write(package / 'freedreno_icd_x64.json', (package / 'freedreno_icd.json').read_text().replace('vulkan_freedreno.dll', 'vulkan_freedreno_x64.dll'))
def patched_source(name, repository, tag, revision, patch):
    directory = output / name
    if not directory.exists():
        run('git', 'clone', '--depth', '1', '--branch', tag, '--recurse-submodules', '--shallow-submodules', repository, directory)
    if subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip() != revision:
        raise ValueError('Unexpected ' + name + ' revision')
    patch = local / patch
    if subprocess.run(['git', '-C', str(directory), 'apply', '--reverse', '--check', str(patch)], capture_output=True).returncode:
        run('git', '-C', directory, 'apply', '--check', patch)
        run('git', '-C', directory, 'apply', patch)
    return directory


d3d12_source = patched_source('vkd3d-proton-3.1', 'https://github.com/HansKristian-Work/vkd3d-proton.git',
    'v3.1', '4522cc1a8b1347f1ab1b0f7b14a23ed9444d95d1', 'vkd3d.patch')
copy(local / 'd3d12_dxgi.h', d3d12_source / 'libs/d3d12/d3d12_dxgi.h')
dxgi_source = patched_source('dxvk-2.7.1', 'https://github.com/doitsujin/dxvk.git',
    'v2.7.1', 'c3dd74be6baec53786d4e064a572185b70347a17', 'dxgi.patch')
copy(local / 'qcom_vulkan.h', d3d12_source / 'libs/d3d12core/qcom_vulkan.h')
copy(local / 'qcom_vulkan.h', dxgi_source / 'src/vulkan/qcom_vulkan.h')
for name in ['a830_composition.h', 'dxgi_composition.h']:
    copy(local / name, dxgi_source / 'src/dxgi' / name)
d3d12_cross = output / 'd3d12.ini'
write(d3d12_cross, """[binaries]
widl = ['x86_64-w64-mingw32-widl', '-I/usr/share/mingw-w64/include']
[built-in options]
c_args = ['/DNOMINMAX', '/DVKD3D_NO_DEBUG_MESSAGES', '/clang:-Wno-incompatible-pointer-types']
cpp_args = ['/EHsc', '/DVKD3D_NO_DEBUG_MESSAGES']
""")
destination = root / 'out/drv/qcom-d3d12'
destination.mkdir(parents=True, exist_ok=True)
for arch, config in [('arm64', cross), ('x64', cross_x64)]:
    directory = output / ('vkd3d-proton-' + arch)
    setup = ['--reconfigure'] if (directory / 'build.ninja').exists() else []
    run('meson', 'setup', *setup, directory, d3d12_source, '--cross-file', config, '--cross-file', d3d12_cross,
        '--buildtype', 'release', '-Db_ndebug=true', '-Db_vscrt=mt', '-Denable_trace=false')
    run('ninja', '-C', directory, '-j', min(os.cpu_count() or 1, 8))
    for name in ['d3d12', 'd3d12core']:
        copy(directory / 'libs' / name / (name + '.dll'), destination / ('qcom_' + name + '_' + arch + '.dll'))
    dxgi_cross = output / ('dxgi-' + arch + '.ini')
    write(dxgi_cross, config.read_text().replace('windres =', 'rc ='))
    directory = output / ('dxvk-' + arch)
    setup = ['--reconfigure'] if (directory / 'build.ninja').exists() else []
    run('meson', 'setup', *setup, directory, dxgi_source, '--cross-file', dxgi_cross,
        "-Dcpp_args=['/EHsc', '/DDXVK_NO_LOG', '/clang:-Wno-pragma-pack']",
        '--buildtype', 'release', '-Db_ndebug=true', '-Db_vscrt=mt', '-Denable_d3d8=false', '-Denable_d3d9=false',
        '-Denable_d3d10=false', '-Denable_d3d11=false')
    run('ninja', '-C', directory, '-j', min(os.cpu_count() or 1, 8), 'src/dxgi/dxgi.dll')
    copy(directory / 'src/dxgi/dxgi.dll', destination / ('qcom_dxgi_' + arch + '.dll'))
forwarder = output / 'd3d12-forwarder'
forwarder.mkdir(parents=True, exist_ok=True)
exports = {
    'arm64': [None, 'SetAppCompatStringPointer', 'D3D12CreateDevice', 'D3D12GetDebugInterface',
        'D3D12CoreCreateLayeredDevice', 'D3D12CoreGetLayeredDeviceSize', 'D3D12CoreRegisterLayers',
        'D3D12CreateRootSignatureDeserializer', 'D3D12CreateVersionedRootSignatureDeserializer',
        'D3D12DeviceRemovedExtendedData', 'D3D12EnableExperimentalFeatures', 'D3D12GetInterface',
        'D3D12PIXEventsReplaceBlock', 'D3D12PIXGetThreadInfo', 'D3D12PIXNotifyWakeFromFenceSignal',
        'D3D12PIXReportCounter', 'D3D12SerializeRootSignature', 'D3D12SerializeVersionedRootSignature', 'GetBehaviorValue'],
    'x64': [None, 'D3D12CoreCreateLayeredDevice', 'D3D12CreateDevice', 'D3D12GetDebugInterface',
        'D3D12CoreGetLayeredDeviceSize', 'D3D12CoreRegisterLayers', 'D3D12CreateRootSignatureDeserializer',
        'D3D12CreateVersionedRootSignatureDeserializer', 'D3D12EnableExperimentalFeatures', 'D3D12GetInterface',
        'D3D12PIXEventsReplaceBlock', 'D3D12PIXGetThreadInfo', 'D3D12PIXNotifyWakeFromFenceSignal',
        'D3D12PIXReportCounter', 'D3D12SerializeRootSignature', 'D3D12SerializeVersionedRootSignature',
        'GetBehaviorValue', 'SetAppCompatStringPointer', 'D3D12DeviceRemovedExtendedData']}
translated = set(exports['x64'][2:4] + exports['x64'][6:10] + exports['x64'][14:16])
definitions = {}
for arch in ['arm64', 'x64']:
    entries = []
    for ordinal, name in enumerate(exports[arch], 99):
        module = 'qcom_d3d12_' + arch if name in translated else 'qcom_d3d12_system'
        entries.append('    ' + (name or 'Ordinal99') + '=' + module + '.' + (name or '#99') +
            ' @' + str(ordinal) + ('' if name else ' NONAME'))
    definitions[arch] = 'EXPORTS\n' + '\n'.join(entries) + '\n'
build_forwarder(forwarder, destination / 'd3d12.dll', definitions)
copy(local / 'install-d3d12.ps1', destination / 'install-d3d12.ps1')
copy(local / 'install-composition.ps1', destination / 'install-composition.ps1')
for name in ['vulkan_freedreno.dll', 'vulkan_freedreno_x64.dll']:
    copy(package / name, destination / name)
for name, path in [('vkd3d', d3d12_source / 'LICENSE'), ('dxvk', dxgi_source / 'LICENSE'),
    ('dxil-spirv', d3d12_source / 'subprojects/dxil-spirv/LICENSE.MIT'),
    ('dxbc-spirv', d3d12_source / 'subprojects/dxil-spirv/subprojects/dxbc-spirv/LICENSE'),
    ('SPIRV-Cross', d3d12_source / 'subprojects/dxil-spirv/third_party/SPIRV-Cross/LICENSE'),
    ('libdisplay-info', dxgi_source / 'subprojects/libdisplay-info/LICENSE')]:
    copy(path, root / 'out/drv/qcom-d3d12' / ('LICENSE.' + name))
print('GPU_RENDERER=' + str(package))
#</GPU-BUILD>

#<FW-INSTALL>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$WindowsDirectory,
    [string]$BluetoothAddress,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath($env:WOA_ROOT).TrimEnd('\')
$firmwareRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'fw')).TrimEnd('\') + '\'
if (-not $WindowsDirectory) { throw 'Specify WindowsDirectory' }
function Resolve-TargetRoot([string]$Path) {
    if (-not [IO.Path]::IsPathRooted($Path)) { throw 'Target paths must be absolute' }
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\') + '\'
    if (-not (Test-Path -LiteralPath $full -PathType Container)) { throw "Target root is missing: $full" }
    if ($full.TrimEnd('\') -ieq $projectRoot -or $full.StartsWith($firmwareRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Target cannot be the source repository or fw directory' }
    $cursor = Get-Item -LiteralPath $full -Force
    while ($null -ne $cursor) {
        if ($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Target cannot traverse reparse points' }
        $cursor = $cursor.Parent
    }
    return $full
}
$windowsRoot = $null
if ($WindowsDirectory) {
    $windowsRoot = Resolve-TargetRoot $WindowsDirectory
    $stream = [IO.File]::OpenRead((Join-Path $windowsRoot 'System32\ntoskrnl.exe'))
    $reader = New-Object IO.BinaryReader($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'Invalid target Windows kernel' }
        $stream.Position = 0x3C
        $offset = $reader.ReadInt32()
        if ($offset -lt 0x40 -or $offset -gt $stream.Length - 6) { throw 'Invalid target PE header' }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0xAA64) { throw 'Target Windows must be ARM64; do not target the x64 development PC' }
    } finally { $reader.Dispose(); $stream.Dispose() }
}
$payloads = @()
if ($windowsRoot) {
    foreach ($name in @('gen80000_gmu.bin','gen80000_sqe.fw','gen80000_aqe.fw')) {
        $payloads += [pscustomobject]@{ Source = (Join-Path $firmwareRoot ('gpu\' + $name)); Root = $windowsRoot; Relative = ('System32\drivers\qcom-gpu\' + $name) }
    }
    $payloads += [pscustomobject]@{ Source = (Join-Path $firmwareRoot 'sia9177\replay.bin'); Root = $windowsRoot; Relative = 'System32\drivers\sia9177\replay.bin' }
    $payloads += [pscustomobject]@{ Source = (Join-Path $firmwareRoot 'wcd9395\replay.bin'); Root = $windowsRoot; Relative = 'System32\drivers\wcd9395\replay.bin' }
    foreach ($name in @('amss.bin','m3.bin','board.bin','regdb.bin','tmel.bin','aux_ucode.bin','qdss_trace_config.bin')) {
        $payloads += [pscustomobject]@{ Source = (Join-Path $firmwareRoot ('wcn7861\' + $name)); Root = $windowsRoot; Relative = ('System32\drivers\wcn7861\' + $name) }
    }
}
if ($windowsRoot) {
    foreach ($name in @('brhperifw20.mbn','brhperinv20.bin','tmel_peach_20.elf','brhbtfw20.mbn','brhbtnv20.bin')) {
        $payloads += [pscustomobject]@{ Source = (Join-Path $firmwareRoot ('wcn7861-bt\' + $name)); Root = $windowsRoot; Relative = ('System32\drivers\wcn7861-bt\' + $name) }
    }
}
if ($BluetoothAddress -and -not $windowsRoot) { throw 'BluetoothAddress requires WindowsDirectory' }
if ($windowsRoot) {
    $btAddressPath = 'System32\drivers\wcn7861-bt\address.bin'
    if ($BluetoothAddress) {
        if ($BluetoothAddress -notmatch '^(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$') { throw 'BluetoothAddress must contain six hexadecimal bytes separated by colons' }
        [byte[]]$address = @($BluetoothAddress.Split(':') | ForEach-Object { [Convert]::ToByte($_, 16) })
        if (@($address | Where-Object { $_ -ne 0 }).Count -eq 0 -or @($address | Where-Object { $_ -ne 255 }).Count -eq 0) { throw 'Invalid Bluetooth address' }
        [Array]::Reverse($address)
        $payloads += [pscustomobject]@{ Bytes = $address; Root = $windowsRoot; Relative = $btAddressPath }
    } else {
        $payloads += [pscustomobject]@{ Preserve = $true; Root = $windowsRoot; Relative = $btAddressPath }
    }
}
$plan = @()
foreach ($file in $payloads) {
    $source = $null
    if (-not $file.Bytes -and -not $file.Preserve) {
        $source = Get-Item -LiteralPath $file.Source -Force
        if ($source.PSIsContainer -or ($source.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Firmware input must be a regular file' }
    }
    $target = [IO.Path]::GetFullPath((Join-Path $file.Root $file.Relative))
    if (-not $target.StartsWith($file.Root, [StringComparison]::OrdinalIgnoreCase)) { throw 'Destination escapes target root' }
    $cursor = [IO.Path]::GetDirectoryName($target)
    while (-not (Test-Path -LiteralPath $cursor)) { $cursor = [IO.Path]::GetDirectoryName($cursor) }
    $parent = Get-Item -LiteralPath $cursor -Force
    if (-not $parent.PSIsContainer) { throw 'Destination parent must be a directory' }
    while ($null -ne $parent) {
        if ($parent.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Destination cannot traverse reparse points' }
        $parent = $parent.Parent
    }
    if (Test-Path -LiteralPath $target) {
        $existing = Get-Item -LiteralPath $target -Force
        if ($existing.PSIsContainer -or ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Destination must be a regular file' }
        if ($file.Preserve) {
            [byte[]]$configured = [IO.File]::ReadAllBytes($target)
            if ($configured.Length -ne 6 -or @($configured | Where-Object { $_ -ne 0 }).Count -eq 0 -or @($configured | Where-Object { $_ -ne 255 }).Count -eq 0) { throw "Invalid Bluetooth address.bin; supply the target device's factory -BluetoothAddress" }
            continue
        }
        if (-not $Force -and -not $WhatIfPreference) { throw "Target file exists; use WhatIf to review and Force to replace: $target" }
    } elseif ($file.Preserve) {
        throw "Bluetooth address.bin is missing; supply the target device's factory -BluetoothAddress before installing firmware"
    }
    $plan += [pscustomobject]@{ Source = $source.FullName; Bytes = $file.Bytes; Target = $target }
}
$copied = 0
foreach ($file in $plan) {
    if ($PSCmdlet.ShouldProcess($file.Target, 'Copy device firmware')) {
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($file.Target)) | Out-Null
        if ($file.Bytes) { [IO.File]::WriteAllBytes($file.Target, $file.Bytes) }
        else { [IO.File]::Copy($file.Source, $file.Target, $Force.IsPresent) }
        $copied++
    }
}
"FIRMWARE_SELECTED=$($plan.Count)"
"FIRMWARE_COPIED=$copied"
'NO_DRIVER_CERTIFICATE_OR_REGISTRY_CHANGES'
