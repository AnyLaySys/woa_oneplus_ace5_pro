import argparse
import hashlib
from pathlib import Path
import re


def array(source, name):
    match = re.search(r'\b' + re.escape(name) + r'\[\]\s*=\s*\{(.*?)\n\};', source, re.S)
    if not match:
        raise ValueError(name)
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', match.group(1), flags=re.S)


def sensor(source, source_name, name):
    rows = re.findall(r'\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*\}', array(source, source_name))
    if not rows:
        raise ValueError(source_name)
    rows = [(int(a, 16), int(v, 16)) for a, v in rows if int(a, 16) != 0x0100]
    if any(a >= 65536 or v >= 256 for a, v in rows):
        raise ValueError(source_name)
    return 'static const SENSOR_REG ' + name + '[]={\n' + ''.join(f'    {{0x{a:04x},0x{v:02x}}},\n' for a, v in rows) + '};\n'


def phy(source, source_name, name):
    rows = re.findall(r'\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(CSIPHY_\w+)\s*\}', array(source, source_name))
    if not rows:
        raise ValueError(source_name)
    kinds = {'CSIPHY_DEFAULT_PARAMS': 0, 'CSIPHY_3PH_REGS': 0, 'CSIPHY_STANDARD_CHANNEL_PARAMS': 0,
             'CSIPHY_SETTLE_CNT_LOWER_BYTE': 1, 'CSIPHY_SETTLE_CNT_HIGHER_BYTE': 2}
    return 'static const PHY_REG ' + name + '[]={\n' + ''.join(
        f'    {{{a},{v},{d},{kinds.get(k, 3)}}},\n' for a, v, d, k in rows) + '};\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('ace5', type=Path)
    parser.add_argument('camera_kernel', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sensor_path = args.ace5 / 'linux/drivers/media/i2c/imx906_tables.h'
    phy_path = args.camera_kernel / 'drivers/cam_sensor_module/cam_csiphy/include/cam_csiphy_2_3_0_hwreg_hummer_main.h'
    s = sensor_path.read_text()
    (root / 'imx906-regs.h').write_text('#pragma once\n\n' + sensor(s, 'imx906_init_regs', 'Imx906Init') + '\n' + sensor(s, 'imx906_start_regs', 'Imx906Mode'))
    p = phy_path.read_text()
    output = '#pragma once\n\ntypedef struct { ULONG Address,Value,Delay,Kind; } PHY_REG;\n'
    for source, name in [('csiphy_reset_enter_reg_2_3_0_hummer_main', 'PhyReset'),
                         ('csiphy_common_reg_2_3_0_hummer_main', 'PhyCommon'),
                         ('csiphy_3ph_v2_3_0_reg_hummer_main', 'PhyCphy'),
                         ('csiphy_reset_exit_reg_2_3_0_hummer_main', 'PhyResetExit')]:
        output += '\n' + phy(p, source, name)
    rates = re.findall(r'\.bandwidth\s*=\s*(\d+).*?\.data_rate_reg_array\[5\]\[0\]\s*=\s*(\w+)', p, re.S)
    if len(rates) != 29 or any(int(a[0]) >= int(b[0]) for a, b in zip(rates, rates[1:])):
        raise ValueError('PHY rate table')
    for index, (_, source) in enumerate(rates):
        output += '\n' + phy(p, source, f'PhyRate{index}')
    output += '\nstatic const struct { ULONGLONG Bandwidth; const PHY_REG *Registers; ULONG Count; } PhyRates[]={\n'
    output += ''.join(f'    {{{bandwidth}ULL,PhyRate{index},RTL_NUMBER_OF(PhyRate{index})}},\n' for index, (bandwidth, _) in enumerate(rates)) + '};\n'
    (root / 'csiphy-regs.h').write_text(output)
    for path in [sensor_path, phy_path]:
        print(f'{path.name}: {hashlib.sha256(path.read_bytes()).hexdigest()}')


if __name__ == '__main__':
    main()
