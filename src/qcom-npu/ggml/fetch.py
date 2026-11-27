"""Fetch pinned upstream GGML runtime and native build dependencies."""
from pathlib import Path
import argparse
import json
import shutil
import subprocess
import tarfile
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from fetch import download, digest, extract

ROOT = Path(__file__).resolve().parents[3]
RELEASE = "b12359"
COMMIT = "b1b770d90d94cad9e9ea5b993bdbc383a522fb28"
RUNTIME_HASH = "b9967897ab8ca0cad3b465929f88266aa589df01495982d6c0a914146fb9e18b"
ARCHIVE_HASH = "8183b26d575a74fbc3ea75214669b622eb01b309013b0349814f7bad4dfe01e1"

def runtime():
    target = ROOT / "out/qcom-npu/inference/ggml"
    library = target / "libggml-htp-v79.so"
    if library.is_file() and digest(library) == RUNTIME_HASH and (target / "GGML-LICENSE.txt").is_file(): return
    archive = target / f"llama-{RELEASE}-bin-android-arm64-snapdragon.tar.gz"
    download(f"https://github.com/ggml-org/llama.cpp/releases/download/{RELEASE}/{archive.name}", archive, ARCHIVE_HASH)
    with tarfile.open(archive, "r:gz") as tar:
        for member, output in [(f"llama-{RELEASE}/lib/libggml-htp-v79.so", library), (f"llama-{RELEASE}/LICENSE", target / "GGML-LICENSE.txt")]:
            with tar.extractfile(member) as stream: output.write_bytes(stream.read())
    if digest(library) != RUNTIME_HASH: raise RuntimeError("V79 GGML runtime hash mismatch")

def source():
    build = ROOT / "out/qcom-npu/chat-build"
    tree = build / "llama-src"
    stamp = tree / ".npu-source"
    if not stamp.is_file() or stamp.read_text().strip() != COMMIT:
        archive = build / "downloads" / f"llama-{COMMIT}.tar.gz"
        download(f"https://codeload.github.com/ggml-org/llama.cpp/tar.gz/{COMMIT}", archive)
        extract(archive, tree, "llama.cpp-" + COMMIT + "/")
        stamp.write_text(COMMIT + "\n")
    ui = build / "downloads/llama-ui-b12359.tar.gz"
    download("https://huggingface.co/buckets/ggml-org/llama-ui/resolve/b12359/dist.tar.gz?download=true", ui,
        "273d1d4e410715f4832579c97868e128d80946c1bde86d682ab06b4b8440933b")
    ui_target = tree / "tools/ui/dist"
    ui_target.mkdir(parents=True, exist_ok=True)
    extract(ui, ui_target)
    packages = [
        ("cmake", "4.4.5", "cmake-4.4.5-py3-none-win_amd64.whl", "d5521b4a9e864505c550abae99ff593d501bd08225583e69b3a9a4ca7f36a03f"),
        ("ninja", "1.13.2", "ninja-1.13.2-py3-none-win_amd64.whl", "1293f4078278b70d0ee4b6cc8f3a9e030656c9b2f59909970343c4fe76070118"),
    ]
    for name, version, filename, sha in packages:
        wheel = build / "downloads" / filename
        if not wheel.is_file() or digest(wheel) != sha:
            metadata = build / "downloads" / (name + ".json")
            download(f"https://pypi.org/pypi/{name}/{version}/json", metadata)
            item = next(item for item in json.loads(metadata.read_text())["urls"] if item["filename"] == filename)
            download(item["url"], wheel, sha)
        executable = build / "build-tools" / ("cmake/data/bin/cmake.exe" if name == "cmake" else "ninja-1.13.2.data/scripts/ninja.exe")
        if not executable.is_file():
            extract(wheel, build / "build-tools")
    binary = build / "build-tools/bin/ninja.exe"
    binary.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(build / "build-tools/ninja-1.13.2.data/scripts/ninja.exe", binary)
    subprocess.run(["python3", str(Path(__file__).with_name("prepare.py")), str(tree)], check=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", action="store_true")
    args = parser.parse_args()
    runtime()
    if args.source: source()
    print("Pinned V79 GGML runtime verified" + ("; native source prepared" if args.source else ""))
