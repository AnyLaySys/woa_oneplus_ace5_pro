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
    shutil.copyfile(root / 'qtee.c', work / 'qtee.c')
    shutil.copyfile(root / 'tests/qtee.c', work / 'qtee-test.c')
    subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-g', str(work / 'qtee.c'), str(work / 'qtee-test.c'), '-o', str(work / 'qtee-test')], check=True)
    subprocess.run([str(work / 'qtee-test')], check=True)
    shutil.copyfile(work / 'fingerprint.h', work / 'base.h')
    shutil.copyfile(root / 'tests/storage-host.h', work / 'fingerprint.h')
    (work / 'ntstrsafe.h').write_text('/* RtlStringCbPrintfW is supplied by the host shim. */\n')
    shutil.copyfile(root / 'storage.c', work / 'storage.c')
    shutil.copyfile(root / 'tests/storage.c', work / 'storage-test.c')
    subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(work), '-g', str(work / 'storage.c'), str(work / 'storage-test.c'),
                    '-o', str(work / 'storage-test')], check=True)
    subprocess.run([str(work / 'storage-test')], check=True)
