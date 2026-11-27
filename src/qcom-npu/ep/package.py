"""Package the signed provider for Windows ML's system extension catalog."""
from pathlib import Path
import os
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
KIT = Path('/mnt/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64')


def windows(path):
    return subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip()


package = ROOT / 'out/drv/qcom-npu'
stage = ROOT / 'out/qcom-npu/winml-package'
(stage / 'EP').mkdir(parents=True, exist_ok=True)
version = re.search(r'DriverVer=[^,]+,([\d.]+)', (HERE.parent / 'kmdf/qcom_npu.inx').read_text()).group(1)
(stage / 'AppxManifest.xml').write_text((HERE / 'AppxManifest.xml').read_text().replace('@VERSION@', version))
shutil.copyfile(package / 'qcom_npu_ep.dll', stage / 'EP/qcom_npu_ep.dll')
shutil.copyfile(HERE / 'logo.png', stage / 'logo.png')
output = package / 'qcom_npu_ep.msix'
subprocess.run([str(KIT / 'MakeAppx.exe'), 'pack', '/o', '/d', windows(stage), '/p', windows(output)], check=True)
subprocess.run([str(KIT / 'signtool.exe'), 'sign', '/q', '/fd', 'sha256', '/s', 'My', '/sha1',
    os.environ.get('WOA_SIGN_CERT', 'FF7908A360512CF12E08C0BD99625830C1EA4E6D'), windows(output)], check=True)
