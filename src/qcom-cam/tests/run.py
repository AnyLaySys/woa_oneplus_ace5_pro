from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='qcom-cam-test-') as directory:
    work = Path(directory)
    for source, name in [(root / 'cci.c', 'cci.c'), (root / 'scm.c', 'scm.c'), (root / 'dma-table.h', 'dma-table.h'),
                         (root / 'dma-address.h', 'dma-address.h'),
                         (root / 'dma-ready.h', 'dma-ready.h'),
                         (root / 'scm-ready.h', 'scm-ready.h'),
                         (root / 'dma-route.h', 'dma-route.h'),
                         (root / 'cdm-command.h', 'cdm-command.h'),
                         (root / 'cdm-route.h', 'cdm-route.h'),
                         (root / 'cdm-abort.h', 'cdm-abort.h'),
                         (root / 'uapi.h', 'uapi.h'), (root / 'video/nv12.h', 'nv12.h'),
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
