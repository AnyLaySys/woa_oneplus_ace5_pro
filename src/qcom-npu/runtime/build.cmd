@echo off
setlocal EnableExtensions DisableDelayedExpansion
for %%I in ("%~dp0..\..\..") do set "WOA_ROOT=%%~fI"
set "NPU_BUILD_MSB=C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe"
"%NPU_BUILD_MSB%" "%~dp0qcom_npu_rpc.vcxproj" /t:Build /p:Configuration=Release /p:Platform=ARM64 /p:OutDir="%WOA_ROOT%\out\qcom-npu\inference\runner/" /p:IntDir="%WOA_ROOT%\out\qcom-npu\inference\rpc-obj/" /v:minimal
if errorlevel 1 exit /b 1
exit /b 0
