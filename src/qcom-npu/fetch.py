"""Shared, checked downloads and archive extraction for NPU build dependencies."""
from pathlib import Path
import hashlib
import subprocess
import tarfile
import zipfile


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
