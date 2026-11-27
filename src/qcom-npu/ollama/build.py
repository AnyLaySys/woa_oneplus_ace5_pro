"""Build the pinned Ollama backend and its unchanged official desktop client."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tarfile
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from fetch import download, extract

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
CACHE = Path.home() / '.cache/qcom-npu/ollama'
BUILD = ROOT / 'out/qcom-npu/ollama-build'
PACKAGE = ROOT / 'out/app/ollama-npu'
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
    tree = BUILD / 'source'
    stamp = tree / '.npu-source'
    prefix = f'ollama-{VERSION}/'
    if not stamp.is_file() or stamp.read_text().strip() != SOURCE_SHA:
        extract(archive, tree, prefix)
        stamp.write_text(SOURCE_SHA)
    patch = (HERE / 'backend.patch').read_text()
    paths = re.findall(r'^--- a/(.+)$', patch, re.MULTILINE)
    with tarfile.open(archive) as tar:
        for name in paths:
            with tar.extractfile(prefix + name) as data:
                (tree / name).write_bytes(data.read())
    subprocess.run(['patch', '--batch', '-p1', '-i', str(HERE / 'backend.patch')], cwd=tree, check=True)
    for name in ('qcom_npu_windows.go', 'qcom_npu_other.go'):
        shutil.copyfile(HERE / name, tree / 'ml' / name)
    return tree


def main():
    tree = source()
    go = toolchain('go1.26.10.linux-amd64.tar.gz', 'https://go.dev/dl/go1.26.10.linux-amd64.tar.gz',
        'ccfe7ae822d2c2f41f4fe7c956199967a4dac22d0ee0d2b2bfdb440aab3b20c5', 'go')
    name = 'llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64'
    llvm = toolchain(name + '.tar.xz', f'https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/{name}.tar.xz',
        'e7a15a49a2ea95e9f3d8a9d0cfabe5052a4b10fb4777cc54de5a8c27b64bb701', name)
    env = dict(os.environ, GOOS='windows', GOARCH='arm64', CGO_ENABLED='1',
        CC=str(llvm / 'bin/aarch64-w64-mingw32-clang'), CXX=str(llvm / 'bin/aarch64-w64-mingw32-clang++'),
        GOMODCACHE=str(CACHE / 'go-modules'), GOCACHE=str(CACHE / 'go-build'))
    headers = BUILD / 'headers'
    headers.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(llvm / 'generic-w64-mingw32/include/eventtoken.h', headers / 'EventToken.h')
    env['CGO_CXXFLAGS'] = env.get('CGO_CXXFLAGS', '') + ' -I' + str(headers)
    ui = tree / 'app/ui/app'
    if not (ui / 'dist/index.html').is_file():
        cached = CACHE / ('v' + VERSION) / 'app/ui/app'
        if (cached / 'node_modules').is_dir() and (cached / 'package-lock.json').read_bytes() == (ui / 'package-lock.json').read_bytes():
            (ui / 'node_modules').symlink_to(cached / 'node_modules', target_is_directory=True)
        else:
            subprocess.run(['npm', 'ci', '--no-audit', '--no-fund'], cwd=ui, check=True)
        subprocess.run(['npm', 'run', 'build'], cwd=ui, check=True)
    PACKAGE.mkdir(parents=True, exist_ok=True)
    exe = go / 'bin/go'
    subprocess.run([str(exe), 'build', '-trimpath', '-ldflags',
        f'-s -w -X github.com/ollama/ollama/version.Version={VERSION}+qcom.npu.9',
        '-o', str(PACKAGE / 'ollama.exe'), '.'], cwd=tree, env=env, check=True)
    subprocess.run([str(exe), 'build', '-trimpath', '-ldflags',
        f'-s -w -H windowsgui -X github.com/ollama/ollama/app/version.Version={VERSION}',
        '-o', str(PACKAGE / 'ollama app.exe'), './app/cmd/app'], cwd=tree, env=env, check=True)
    native = ROOT / 'out/qcom-npu/chat-build/native/bin'
    libraries = PACKAGE / 'lib/ollama'
    libraries.mkdir(parents=True, exist_ok=True)
    for name in ('llama-server.exe', 'llama-server-impl.dll', 'llama-common.dll', 'llama.dll', 'mtmd.dll',
                 'ggml.dll', 'ggml-base.dll', 'ggml-cpu.dll', 'msvcp140.dll', 'vcruntime140.dll'):
        shutil.copyfile(native / name, libraries / name)
    shutil.copyfile(tree / 'LICENSE', PACKAGE / 'Ollama-LICENSE.txt')
    shutil.copyfile(ROOT / 'out/qcom-npu/inference/ggml/GGML-LICENSE.txt', PACKAGE / 'GGML-LICENSE.txt')
    shutil.copyfile(HERE / 'install.ps1', PACKAGE / 'install.ps1')
    print('Ollama package:', PACKAGE)


if __name__ == '__main__':
    main()
