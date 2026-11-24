from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jiiov-fp-test-') as directory:
    work = Path(directory)
    for source, name in [(root / 'spi.c', 'spi.c'), (root / 'uapi.h', 'uapi.h'),
                         (root / 'tests/host.h', 'fingerprint.h'), (root / 'tests/host.c', 'host.c')]:
        shutil.copyfile(source, work / name)
    subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-g', str(work / 'spi.c'), str(work / 'host.c'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
