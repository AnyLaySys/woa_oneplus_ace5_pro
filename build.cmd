@echo off
setlocal EnableExtensions DisableDelayedExpansion
for %%I in ("%~dp0.") do set "ROOT=%%~fI"
if /i "%~1"=="--help" goto help
if /i "%~1"=="-h" goto help
if /i "%~1"=="/?" goto help
if /i "%~1"=="install-fw" goto install_fw
if not "%~2"=="" goto invalid
set "NAME=%~1"
set "TARGET="
if /i "%NAME%"=="syna-s3910" (set "NAME=syna-s3910"& set "TARGET=syna_s3910")
if /i "%NAME%"=="qcom-audio" (set "NAME=qcom-audio"& set "TARGET=sia9177")
if /i "%NAME%"=="qcom-bluetooth" (set "NAME=qcom-bluetooth"& set "TARGET=qcom_bluetooth")
if /i "%NAME%"=="qcom-wcn7861" (set "NAME=qcom-wcn7861"& set "TARGET=wcn7861")
if /i "%NAME%"=="qcom-battery" (set "NAME=qcom-battery"& set "TARGET=qcom_battery")
if /i "%NAME%"=="qcom-pmic-glink" (set "NAME=qcom-pmic-glink"& set "TARGET=qcom_pmic_glink")
if not defined TARGET goto invalid
set "SRC=%ROOT%\src\%NAME%"
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
set "WORK=%ROOT%\out\%NAME%\ARM64\Release"
set "BIN=%WORK%\bin"
set "VS=C:\Program Files\Microsoft Visual Studio\18\Insiders"
set "MSB=%VS%\MSBuild\Current\Bin\MSBuild.exe"
set "KITBIN=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0"
set "SIGNTOOL=%KITBIN%\x64\signtool.exe"
set "INF2CAT=%KITBIN%\x86\Inf2Cat.exe"
set "CERT=21156C97591C00F302E01411B08F4591B184CCE6"
if not exist "%MSB%" exit /b 1
if not exist "%DST%" mkdir "%DST%"
if not exist "%WORK%\obj" mkdir "%WORK%\obj"
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 exit /b 1
"%MSB%" "%PROJECT%" /t:Rebuild /p:Configuration=Release /p:Platform=ARM64 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:OutDir="%BIN%/" /p:IntDir="%WORK%\obj/" /p:SkipPackageVerification=true /p:SignMode=Off /v:minimal
if errorlevel 1 exit /b 1
copy /y "%BIN%\%TARGET%.sys" "%DST%\%TARGET%.sys" >nul
if errorlevel 1 exit /b 1
copy /y "%BIN%\%TARGET%.inf" "%DST%\%TARGET%.inf" >nul
if errorlevel 1 exit /b 1
"%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\%TARGET%.sys"
if errorlevel 1 exit /b 1
"%INF2CAT%" /driver:"%DST%" /os:10_CO_ARM64 /uselocaltime
if errorlevel 1 exit /b 1
"%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\%TARGET%.cat"
if errorlevel 1 exit /b 1
echo.
echo DRIVER=%DST%
dir "%DST%" /b
exit /b 0

:install_fw
set "WOA_BUILD_ENTRY=%~f0"
set "WOA_ROOT=%ROOT%"
set "WOA_WINDOWS="
set "WOA_ALS="
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
if /i "%~1"=="-AlsRoot" (
  if defined WOA_ALS goto invalid
  if "%~2"=="" goto invalid
  set "WOA_ALS=%~2"
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
if not defined WOA_WINDOWS if not defined WOA_ALS goto invalid
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$text=[IO.File]::ReadAllText($env:WOA_BUILD_ENTRY); $marker=[regex]::Match($text,'(?m)^#<FW-INSTALL>\r?$'); if(-not $marker.Success){throw 'Missing firmware installer body'}; $code=$text.Substring($marker.Index+$marker.Length); & ([scriptblock]::Create($code)) -WindowsDirectory $env:WOA_WINDOWS -AlsRoot $env:WOA_ALS -BluetoothAddress $env:WOA_BLUETOOTH_ADDRESS -Force:($env:WOA_FORCE -eq '1') -WhatIf:($env:WOA_WHATIF -eq '1')"
exit /b %errorlevel%

:invalid
echo ERROR invalid target or arguments
call :help
exit /b 2

:help
echo Usage: build.cmd syna-s3910
echo        build.cmd qcom-audio
echo        build.cmd qcom-bluetooth
echo        build.cmd qcom-wcn7861
echo        build.cmd qcom-battery
echo        build.cmd qcom-pmic-glink
echo        build.cmd install-fw -WindowsDirectory "W:\Windows" -AlsRoot "A:\" [-BluetoothAddress "XX:XX:XX:XX:XX:XX"] [-WhatIf] [-Force]
echo Firmware copy requires at least one explicit target. Existing files require -Force.
exit /b 0

#<FW-INSTALL>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$WindowsDirectory,
    [string]$AlsRoot,
    [string]$BluetoothAddress,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath($env:WOA_ROOT).TrimEnd('\')
$firmwareRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'fw')).TrimEnd('\') + '\'
if (-not $WindowsDirectory -and -not $AlsRoot) { throw 'Specify WindowsDirectory, AlsRoot, or both' }
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
$alsDirectory = $null
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
if ($AlsRoot) {
    $alsDirectory = Resolve-TargetRoot $AlsRoot
    if ($alsDirectory -ieq [IO.Path]::GetPathRoot($env:SystemRoot) -and $env:PROCESSOR_ARCHITECTURE -ne 'ARM64') { throw 'ALS target cannot be the x64 development system volume' }
}
$payloads = @()
if ($windowsRoot) {
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
if ($BluetoothAddress) {
    if (-not $windowsRoot) { throw 'BluetoothAddress requires WindowsDirectory' }
    if ($BluetoothAddress -notmatch '^(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$') { throw 'BluetoothAddress must contain six hexadecimal bytes separated by colons' }
    [byte[]]$address = @($BluetoothAddress.Split(':') | ForEach-Object { [Convert]::ToByte($_, 16) })
    if (@($address | Where-Object { $_ -ne 0 }).Count -eq 0 -or @($address | Where-Object { $_ -ne 255 }).Count -eq 0) { throw 'Invalid Bluetooth address' }
    [Array]::Reverse($address)
    $payloads += [pscustomobject]@{ Bytes = $address; Root = $windowsRoot; Relative = 'System32\drivers\wcn7861-bt\address.bin' }
}
if ($alsDirectory) {
    $adspRoot = Join-Path $firmwareRoot 'adsp'
    foreach ($name in @('adsp.mdt','adsp_dtb.mdt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $adspRoot $name) -PathType Leaf)) { throw "ADSP input missing: $name" }
    }
    foreach ($file in Get-ChildItem -LiteralPath $adspRoot -File) {
        if ($file.Name -notmatch '^adsp(?:_dtb)?\.(?:mdt|b[0-9]{2})$') { continue }
        $payloads += [pscustomobject]@{ Source = $file.FullName; Root = $alsDirectory; Relative = ('ADSP\' + $file.Name) }
    }
}
$plan = @()
foreach ($file in $payloads) {
    $source = $null
    if (-not $file.Bytes) {
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
        if (-not $Force -and -not $WhatIfPreference) { throw "Target file exists; use WhatIf to review and Force to replace: $target" }
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
