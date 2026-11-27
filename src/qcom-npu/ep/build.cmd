@echo off
setlocal EnableExtensions DisableDelayedExpansion
for %%I in ("%~dp0..\..\..") do set "WOA_ROOT=%%~fI"
set "NPU_BUILD_ROOT=%WOA_ROOT%\out\qcom-npu\inference"
set "NPU_BUILD_MSB=C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe"
"%NPU_BUILD_MSB%" "%~dp0provider.vcxproj" /t:Build /p:Configuration=Release /p:Platform=ARM64 /p:OutDir="%NPU_BUILD_ROOT%\runner/" /p:IntDir="%NPU_BUILD_ROOT%\ep-obj/" /v:minimal
if errorlevel 1 exit /b 1
copy /y "%WOA_ROOT%\out\qcom-npu\inference\sdk\LICENSE.pdf" "%NPU_BUILD_ROOT%\runner\QNN-LICENSE.pdf" >nul
if errorlevel 1 exit /b 1
copy /y "%WOA_ROOT%\src\qcom-npu\runtime\vendor\LICENSE.txt" "%NPU_BUILD_ROOT%\runner\FastRPC-LICENSE.txt" >nul
if errorlevel 1 exit /b 1
copy /y "%WOA_ROOT%\src\qcom-npu\ep\include\LICENSE" "%NPU_BUILD_ROOT%\runner\ONNX-Runtime-LICENSE.txt" >nul
exit /b 0
