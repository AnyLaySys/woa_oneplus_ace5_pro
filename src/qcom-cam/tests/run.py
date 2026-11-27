from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
subprocess.run(['python3', str(root / 'tests/isp.py')], check=True)
with tempfile.TemporaryDirectory(prefix='qcom-cam-test-') as directory:
    work = Path(directory)
    for source, name in [(root / 'flash.c', 'flash.c'), (root / 'tests/flash.c', 'flash-test.c'), (root / 'cci.c', 'cci.c'), (root / 'focus.c', 'focus.c'), (root / 'pdaf.h', 'pdaf.h'), (root / 'tests/focus.c', 'focus-test.c'), (root / 'tests/pdaf.c', 'pdaf-test.c'), (root / 'scm.c', 'scm.c'), (root / 'dma-table.h', 'dma-table.h'),
                         (root / 'dma-address.h', 'dma-address.h'),
                         (root / 'frame-map-table.h', 'frame-map-table.h'),
                         (root / 'frame-context.h', 'frame-context.h'),
                         (root / 'dma-idle.h', 'dma-idle.h'),
                         (root / 'rdi-frame.h', 'rdi-frame.h'), (root / 'frame-clock.h', 'frame-clock.h'),
                         (root / 'dma-ready.h', 'dma-ready.h'),
                         (root / 'scm-ready.h', 'scm-ready.h'),
                         (root / 'dma-route.h', 'dma-route.h'),
                         (root / 'cdm-command.h', 'cdm-command.h'),
                         (root / 'cdm-target.h', 'cdm-target.h'),
                         (root / 'cdm-route.h', 'cdm-route.h'),
                         (root / 'cdm-abort.h', 'cdm-abort.h'),
                         (root / 'smmu-fault.h', 'smmu-fault.h'),
                         (root / 'smmu-route-check.h', 'smmu-route-check.h'),
                         (root / 'smmu-restore.h', 'smmu-restore.h'),
                         (root / 'frame-route.h', 'frame-route.h'),
                         (root / 'tbu-translate.h', 'tbu-translate.h'),
                         (root / 'mnoc-qos.h', 'mnoc-qos.h'),
                         (root / 'scm-qos-payload.h', 'scm-qos-payload.h'),
                         (root / 'uapi.h', 'uapi.h'), (root / 'video/nv12.h', 'nv12.h'), (root / 'video/raw-row.h', 'raw-row.h'),
                         (root / 'rpmh-message.h', 'rpmh-message.h'),
                         (root / 'wait.h', 'wait.h'),
                         (root / 'rcg-restore.h', 'rcg-restore.h'),
                         (root / 'csid-commit.h', 'csid-commit.h'),
                         (root / 'crm-vote.h', 'crm-vote.h'),
                         (root / 'clock-measure.h', 'clock-measure.h'),
                         (root / 'gdsc-cycle.h', 'gdsc-cycle.h'),
                         (root / 'cphy-lanes.h', 'cphy-lanes.h'),
                         (root / 'tests/host.h', 'cam.h'), (root / 'tests/host.h', 'host.h'),
                         (root / 'tests/host.c', 'host.c')]:
        shutil.copyfile(source, work / name)
    subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-g', str(work / 'cci.c'), str(work / 'scm.c'), str(work / 'host.c'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
    subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-g', str(work / 'focus.c'), str(work / 'focus-test.c'), '-o', str(work / 'focus-test')], check=True)
    subprocess.run([str(work / 'focus-test')], check=True)
    subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-g', str(work / 'pdaf-test.c'), '-o', str(work / 'pdaf-test')], check=True)
    subprocess.run([str(work / 'pdaf-test')], check=True)

    subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g', str(work / 'flash.c'), str(work / 'flash-test.c'), '-o', str(work / 'flash-test')], check=True)
    subprocess.run([str(work / 'flash-test')], check=True)

if shutil.which('aarch64-linux-gnu-g++') and shutil.which('qemu-aarch64'):
    with tempfile.TemporaryDirectory(prefix='qcom-cam-arm64-') as directory:
        binary = Path(directory) / 'raw-mapped'
        subprocess.run(['aarch64-linux-gnu-g++', '-O2', '-static', '-Wall', '-Wextra', '-Werror',
                        str(root / 'tests/raw-mapped.cpp'), str(root / 'video/raw-row.cpp'), '-o', str(binary)], check=True)
        subprocess.run(['qemu-aarch64', str(binary)], check=True)
