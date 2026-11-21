from pathlib import Path
import struct

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2
from unicorn.arm64_const import UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5
from unicorn.arm64_const import UC_ARM64_REG_X6, UC_ARM64_REG_X7, UC_ARM64_REG_X18
from unicorn.arm64_const import UC_ARM64_REG_X19, UC_ARM64_REG_X20, UC_ARM64_REG_X29
from unicorn.arm64_const import UC_ARM64_REG_X30, UC_ARM64_REG_SP, UC_ARM64_REG_PC

root = Path(__file__).resolve().parents[1]
obj = root.parents[1] / 'out/qcom-cam/ARM64/Release/obj/scm-slow.obj'
data = obj.read_bytes()
machine, sections, _, symbols, count, optional, _ = struct.unpack_from('<HHIIIHH', data)
assert machine == 0xaa64
code = None
for index in range(sections):
    header = 20 + optional + index * 40
    name = data[header:header + 8].rstrip(b'\0')
    size, offset = struct.unpack_from('<II', data, header + 16)
    if name == b'.text':
        code = data[offset:offset + size]
assert code and bytes.fromhex('030000d4') in code
entries = {}
strings = symbols + count * 18
index = 0
while index < count:
    record = symbols + index * 18
    name = data[record:record + 8]
    if name[:4] == b'\0' * 4:
        offset = struct.unpack_from('<I', name, 4)[0]
        start = strings + offset
        name = data[start:data.index(b'\0', start)]
    else:
        name = name.rstrip(b'\0')
    value, section = struct.unpack_from('<Ih', data, record + 8)
    if name in [b'CamScmSlow', b'CamScmOnce']:
        assert section > 0
        entries[name.decode()] = value
    index += 1 + data[record + 17]
assert set(entries) == {'CamScmSlow', 'CamScmOnce'}

registers = [UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3,
             UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X6, UC_ARM64_REG_X7]
base, stack, memory = 0x1000000, 0x2000000, 0x3000000
return_address = base + 0x1000
parameters = [0x42000601, 1, 0x0200180a, 0, 0, 0, 0, 0]

def run(interruptions, result, entry='CamScmSlow'):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for address in [base, stack, memory]:
        cpu.mem_map(address, 0x10000)
    cpu.mem_write(base, code)
    cpu.mem_write(memory, struct.pack('<8Q', *parameters))
    saved = {UC_ARM64_REG_X18: 0x1818181818181818, UC_ARM64_REG_X19: 0x1919191919191919,
             UC_ARM64_REG_X20: 0x2020202020202020, UC_ARM64_REG_X29: 0x2929292929292929,
             UC_ARM64_REG_X30: return_address, UC_ARM64_REG_SP: stack + 0xf000}
    for register, value in saved.items():
        cpu.reg_write(register, value)
    cpu.reg_write(UC_ARM64_REG_X0, memory)
    calls = 0

    def instruction(uc, address, size, _):
        nonlocal calls
        if bytes(uc.mem_read(address, size)) != bytes.fromhex('030000d4'):
            return
        if calls == 0:
            assert [uc.reg_read(r) for r in registers] == parameters
        else:
            assert uc.reg_read(UC_ARM64_REG_X0) == 1
            assert uc.reg_read(UC_ARM64_REG_X6) == 0xa6000000 + calls
            for index in [1, 2, 3, 4, 5, 7]:
                assert uc.reg_read(registers[index]) == parameters[index]
            assert uc.reg_read(UC_ARM64_REG_X18) == saved[UC_ARM64_REG_X18]
        calls += 1
        uc.reg_write(UC_ARM64_REG_X18, 0)
        if calls <= interruptions:
            uc.reg_write(UC_ARM64_REG_X0, 1)
            for index in [1, 2, 3, 4, 5, 7]:
                uc.reg_write(registers[index], 0xbad00000 + index + calls * 16)
            uc.reg_write(UC_ARM64_REG_X6, 0xa6000000 + calls)
        else:
            uc.reg_write(UC_ARM64_REG_X0, result)
            uc.reg_write(UC_ARM64_REG_X1, 1)
        uc.reg_write(UC_ARM64_REG_PC, address + size)

    cpu.hook_add(UC_HOOK_CODE, instruction)
    cpu.emu_start(base + entries[entry], return_address, count=2000)
    assert cpu.reg_read(UC_ARM64_REG_PC) == return_address
    for register, value in saved.items():
        assert cpu.reg_read(register) == value
    returned = struct.unpack('<8Q', bytes(cpu.mem_read(memory, 64)))
    limit = 1 if entry == 'CamScmOnce' else 64
    assert calls == min(interruptions + 1, limit)
    assert returned[0] == (1 if interruptions >= limit else result)
    if interruptions < limit:
        assert returned[1] == 1

for parameters in [[0x42000601, 1, 0x0200180a, 0, 0, 0, 0, 0],
                   [0x4200180a, 0x83, 1, 0x90123000, 8, 0, 0, 0],
                   [0x4200180a, 0x83, 1, 0x90123000, 16, 0, 0, 0],
                   [0x4200180a, 0x83, 1, 0x90123000, 40, 0, 0, 0],
                   [0x42000601, 1, 0x02000501, 0, 0, 0, 0, 0],
                   [0x82000501, 1, 0x0ad90010, 0, 0, 0, 0, 0],
                   [0xc2000501, 1, 0x0ad90010, 0, 0, 0, 0, 0]]:
    for interruptions, result in [(0, 0), (1, 0), (3, 0), (100, 0), (0, 0xffffffff)]:
        run(interruptions, result)
    for interruptions, result in [(0, 0), (1, 0), (3, 0), (0, 2), (0, 3)]:
        run(interruptions, result, 'CamScmOnce')
print('Assembled Windows SMC ABI, original argument recovery, single call, waitq returns and retry bound passed')
