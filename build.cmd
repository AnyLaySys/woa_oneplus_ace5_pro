@echo off
setlocal EnableExtensions DisableDelayedExpansion
for %%I in ("%~dp0.") do set "ROOT=%%~fI"
if /i "%~1"=="--help" goto help
if /i "%~1"=="-h" goto help
if /i "%~1"=="/?" goto help
if /i "%~1"=="install-fw" goto install_fw
if not "%~2"=="" goto invalid
if /i "%~1"=="all" goto build_all
set "NAME=%~1"
set "TARGET="
if /i "%NAME%"=="syna-s3910" (set "NAME=syna-s3910"& set "TARGET=syna_s3910")
if /i "%NAME%"=="qcom-audio" (set "NAME=qcom-audio"& set "TARGET=sia9177")
if /i "%NAME%"=="qcom-bluetooth" (set "NAME=qcom-bluetooth"& set "TARGET=qcom_bluetooth")
if /i "%NAME%"=="qcom-wcn7861" (set "NAME=qcom-wcn7861"& set "TARGET=wcn7861")
if /i "%NAME%"=="qcom-charge" (set "NAME=qcom-audio"& set "TARGET=sia9177")
if /i "%NAME%"=="qcom-battery" (set "NAME=qcom-battery"& set "TARGET=qcom_battery")
if /i "%NAME%"=="qcom-cpupower" (set "NAME=qcom-cpupower"& set "TARGET=qcom_cpupower")
if /i "%NAME%"=="qcom-keys" (set "NAME=qcom-keys"& set "TARGET=qcom_keys")
if /i "%NAME%"=="qcom-gpu" (set "NAME=qcom-gpu"& set "TARGET=qcom_gpu")
if not defined TARGET goto invalid
set "CREDENTIAL="
if /i "%NAME%"=="qcom-audio" (
  set "CREDENTIAL=%ROOT%\fw\charge\credential.bin"
  for %%F in ("%ROOT%\fw\charge\credential.bin") do if not "%%~zF"=="16" (
    echo ERROR fw\charge\credential.bin is missing or not exactly 16 bytes.
    exit /b 1
  )
)
set "SRC=%ROOT%\src\%NAME%"
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
set "WORK=%ROOT%\out\%NAME%\ARM64\Release"
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
set "GPU_RENDER_PACKAGE=%ROOT%\out\mesa\umd-package"
if defined GPU_PACKAGE call :build_gpu_renderer
if errorlevel 1 exit /b 1
if defined GPU_PACKAGE (
  if not exist "%BIN%" mkdir "%BIN%"
  for %%F in (a830_umd.dll a830_gl.dll a830_gl_x64.dll vulkan_freedreno.dll vulkan_freedreno_x64.dll freedreno_icd.json freedreno_icd_x64.json msvcp140.dll vcruntime140.dll) do (
    if not exist "%GPU_RENDER_PACKAGE%\%%F" (
      echo ERROR GPU renderer output is missing: %GPU_RENDER_PACKAGE%\%%F
      exit /b 1
    )
    copy /y "%GPU_RENDER_PACKAGE%\%%F" "%BIN%\%%F" >nul
    if errorlevel 1 exit /b 1
  )
  copy /y "%ROOT%\fw\gpu\gen80000_zap.mbn" "%BIN%\gen80000_zap.mbn" >nul
  if errorlevel 1 exit /b 1
)
"%MSB%" "%PROJECT%" /t:Rebuild /p:Configuration=Release /p:Platform=ARM64 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:OutDir="%BIN%/" /p:IntDir="%WORK%\obj/" /p:SkipPackageVerification=true /p:SignMode=Off /v:minimal
if errorlevel 1 exit /b 1
if defined GPU_PACKAGE for %%E in (sys inf cat) do if exist "%DST%\*.%%E" del /q "%DST%\*.%%E"
copy /y "%BIN%\%TARGET%.sys" "%DST%\%TARGET%.sys" >nul
if errorlevel 1 exit /b 1
copy /y "%BIN%\%TARGET%.inf" "%DST%\%TARGET%.inf" >nul
if errorlevel 1 exit /b 1
if defined GPU_PACKAGE (
  for %%F in (a830_umd.dll a830_gl.dll a830_gl_x64.dll vulkan_freedreno.dll vulkan_freedreno_x64.dll) do (
    copy /y "%BIN%\%%F" "%DST%\%%F" >nul
    if errorlevel 1 exit /b 1
    "%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\%%F"
    if errorlevel 1 exit /b 1
  )
  for %%F in (freedreno_icd.json freedreno_icd_x64.json msvcp140.dll vcruntime140.dll gen80000_zap.mbn) do (
    copy /y "%BIN%\%%F" "%DST%\%%F" >nul
    if errorlevel 1 exit /b 1
  )
)
"%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\%TARGET%.sys"
if errorlevel 1 exit /b 1
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
"%INF2CAT%" /driver:"%DST%" /os:10_CO_ARM64 /uselocaltime
if errorlevel 1 exit /b 1
"%SIGNTOOL%" sign /q /fd sha256 /s My /sha1 %CERT% "%DST%\%TARGET%.cat"
if errorlevel 1 exit /b 1
echo.
echo DRIVER=%DST%
dir "%DST%" /b
exit /b 0

:build_all
for %%D in (syna-s3910 qcom-audio qcom-bluetooth qcom-wcn7861 qcom-battery qcom-cpupower qcom-keys qcom-gpu) do (
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
echo        build.cmd syna-s3910
echo        build.cmd qcom-audio
echo        build.cmd qcom-bluetooth
echo        build.cmd qcom-wcn7861
echo        build.cmd qcom-charge
echo        build.cmd qcom-battery
echo        build.cmd qcom-cpupower
echo        build.cmd qcom-keys
echo        build.cmd qcom-gpu
echo        build.cmd install-fw -WindowsDirectory "W:\Windows" [-BluetoothAddress "XX:XX:XX:XX:XX:XX"] [-WhatIf] [-Force]
echo all builds every driver.
echo qcom-charge builds qcom-audio with autonomous kernel charging.
echo qcom-gpu builds the Qualcomm Adreno 830 desktop driver, renderer and signed package.
echo GPU builds use WSL Ubuntu-26.10 by default. Set WOA_WSL_DISTRO to override it.
echo Firmware copy requires an explicit WindowsDirectory. Existing files require -Force.
echo Windows targets require a valid existing Bluetooth address.bin or -BluetoothAddress.
exit /b 0

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
copy(local / 'tu_knl_windows.cc', vulkan / 'tu_knl_windows.cc')
abi = (root / 'src/qcom-gpu/wddm/abi.h').read_text().replace('#include "../uapi.h"', '#include "qcom_gpu_uapi.h"')
for backend in [vulkan, source / 'src/freedreno/drm']:
    for name in ['a830_windows.h', 'a830_host.h']:
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
    '-Dtu-build-id=' + mesa_commit]
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
umd_build = output / 'umd-arm64'
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
            source / 'include', source / 'src', source / 'src/compiler/spirv', vulkan,
            local, root / 'src/qcom-gpu/wddm', umd]
args = ['/nologo', '/O2', '/MD', '/DNDEBUG', '/D_CRT_SECURE_NO_WARNINGS', '/D_USE_MATH_DEFINES',
        '/DLIBVKD3D_SHADER_SOURCE', '/DVKD3D_NO_TRACE_MESSAGES', '/DVKD3D_NO_DEBUG_MESSAGES', '/w']
args += ['/I' + str(path) for path in includes]
files = list(shader.glob('*.c'))
files += [umd_build / (name + suffix) for name in ['hlsl', 'preproc'] for suffix in ['.tab.c', '.yy.c']]
files += [shader_source / 'libs/vkd3d-common' / (name + '.c') for name in ['debug', 'memory', 'error', 'utf8']]


def compile_file(path):
    obj = umd_build / (path.stem + '.obj')
    cpp = path.suffix == '.cpp'
    if cpp or not obj.exists() or obj.stat().st_mtime < path.stat().st_mtime:
        flags = ['/clang:-ferror-limit=8', '/std:c++17', '/EHsc'] if cpp else ['/std:c11']
        options = [arg for arg in args if arg != '/w'] + ['/W4', '/WX', '/clang:-Wno-unused-parameter', '/clang:-Wno-missing-field-initializers'] if cpp else args
        result = subprocess.run(compiler + options + flags + ['/c', str(path), '/Fo' + str(obj)], capture_output=True, text=True)
        if result.returncode:
            print(result.stdout + result.stderr, flush=True)
            raise RuntimeError(str(path))
    return obj


with ThreadPoolExecutor(max_workers=6) as pool:
    objects = list(pool.map(compile_file, files))
    objects += list(pool.map(compile_file, [
        umd / (name + '.cpp') for name in ['core', 'driver', 'pipeline', 'query', 'resource', 'shader', 'state']]))
definition = umd_build / 'a830_umd.def'
write(definition, 'LIBRARY a830_umd\nEXPORTS\nOpenAdapter10\nOpenAdapter10_2\n')
run('lld-link', '/DLL', '/MACHINE:ARM64', '/OUT:' + str(umd_build / 'a830_umd.dll'),
    '/DEF:' + str(definition), '/MAP:' + str(umd_build / 'a830_umd.map'), *link,
    *objects, 'kernel32.lib', 'user32.lib', 'gdi32.lib', 'advapi32.lib', 'ucrt.lib',
    'vcruntime.lib', 'msvcrt.lib', 'oldnames.lib', 'uuid.lib')
copy(umd_build / 'a830_umd.dll', package / 'a830_umd.dll')
copy(shader_source / 'COPYING', package / 'COPYING.vkd3d')
cross_x64 = output / 'cross-x64.ini'
write(cross_x64, cross.read_text().replace('aarch64', 'x86_64').replace("'arm64'", "'x86_64'").replace('/arm64', '/x64'))
options_x64 = [option.replace('a830_gl', 'a830_gl_x64') for option in options] + ['-Db_vscrt=mt']
build_x64 = output / 'x64'
build_mesa(build_x64, cross_x64, options_x64, 'src/freedreno/vulkan/vulkan_freedreno.dll', 'src/gallium/targets/wgl/a830_gl_x64.dll', 'src/gallium/targets/libgl-gdi/opengl32.dll')
copy(build_x64 / 'src/gallium/targets/wgl/a830_gl_x64.dll', package / 'a830_gl_x64.dll')
copy(build_x64 / 'src/freedreno/vulkan/vulkan_freedreno.dll', package / 'vulkan_freedreno_x64.dll')
write(package / 'freedreno_icd_x64.json', (package / 'freedreno_icd.json').read_text().replace('vulkan_freedreno.dll', 'vulkan_freedreno_x64.dll'))
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
