from pathlib import Path
import argparse
import ctypes
import importlib.util
import json
import random
import struct
import subprocess
import tempfile
import zlib

root = Path(__file__).resolve().parents[1]
tools = root.parents[1] / 'out/tools'
parser = argparse.ArgumentParser()
parser.add_argument('--stock', type=Path)
parser.add_argument('--iq', type=Path)
parser.add_argument('--tuning', type=Path, nargs='+')
args = parser.parse_args()
if args.iq and not args.stock:
    parser.error('--iq requires --stock')
if args.tuning and not args.iq:
    parser.error('--tuning requires --iq')
oracle = None
iq = None
if args.stock:
    spec = importlib.util.spec_from_file_location('isp_stock', tools / 'isp-stock.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    oracle = module.Apollo980(args.stock)
    if args.iq:
        iq = module.Apollo980Iq(args.iq)

u32 = ctypes.c_uint32
i32 = ctypes.c_int32

class GtmInput(ctypes.Structure):
    _fields_ = [('enabled',u32),('bank',u32),('config',u32*4),('base',u32*256),
                ('slope',u32*256),('shift',u32*256),('segments',u32*256)]

class GtmOutput(ctypes.Structure):
    _fields_ = [('registers',u32*263),('table',ctypes.c_uint64*256)]
with tempfile.TemporaryDirectory(prefix='qcam-isp-') as directory:
    input_binary = Path(directory) / 'isp-input'
    subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(root / 'tests/isp-input.c'), '-o', str(input_binary)], check=True)
    subprocess.run([str(input_binary)], check=True)
    pixel_binary = Path(directory) / 'isp-pixel'
    subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(root / 'tests/isp-pixel.c'), '-o', str(pixel_binary)], check=True)
    subprocess.run([str(pixel_binary)], check=True)
    queue_binary = Path(directory) / 'isp-queue'
    subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(root / 'tests/isp-queue.c'), '-o', str(queue_binary)], check=True)
    subprocess.run([str(queue_binary)], check=True)
    fd_binary = Path(directory) / 'isp-fd'
    subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(root / 'tests/isp-fd.c'), '-o', str(fd_binary)], check=True)
    subprocess.run([str(fd_binary)], check=True)
    lut_binary=Path(directory)/'isp-lut'
    subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    str(root/'tests/isp-lut.c'),'-o',str(lut_binary)],check=True)
    subprocess.run([str(lut_binary)],check=True)
    stop_binary=Path(directory)/'isp-stop'
    subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    str(root/'tests/isp-stop.c'),'-o',str(stop_binary)],check=True)
    subprocess.run([str(stop_binary)],check=True)
    binary = Path(directory) / 'isp.so'
    subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                    '-fsanitize=undefined', str(root / 'tests/isp.c'), '-o', str(binary)], check=True)
    library = ctypes.CDLL(str(binary))
    library.mnds.argtypes = [ctypes.POINTER(u32)] + [u32] * 4
    library.pixel_plan.argtypes = [ctypes.POINTER(u32), u32, u32]
    library.fd_pixel_plan.argtypes = library.pixel_plan.argtypes
    library.fd_exact_plan.argtypes = [ctypes.POINTER(u32)]
    library.common_plan.argtypes = [ctypes.POINTER(u32)] * 4 + [ctypes.c_int]
    library.gamma_plan.argtypes = [ctypes.POINTER(u32), ctypes.POINTER(u32), u32]
    library.gamma_dmi.argtypes = [ctypes.POINTER(u32), u32, ctypes.c_uint64, u32, u32, ctypes.POINTER(u32)]
    library.gamma_registers.argtypes = library.gamma_dmi.argtypes
    library.compand_raw10_reference.argtypes = [ctypes.POINTER(u32)]
    library.compdecomp.argtypes = [ctypes.POINTER(u32),ctypes.POINTER(u32)]
    library.compdecomp_registers.argtypes = [ctypes.POINTER(u32),u32,ctypes.POINTER(u32)]
    library.compdecomp_disabled.argtypes = [ctypes.POINTER(u32),u32,u32,u32,u32]
    library.gtm.argtypes = [ctypes.POINTER(GtmOutput),ctypes.POINTER(GtmInput)]
    library.channel_gain_reference.argtypes = [ctypes.POINTER(u32),u32]
    library.channel_gain.argtypes = [ctypes.POINTER(u32), ctypes.POINTER(u32)]
    library.cc_plan.argtypes = [ctypes.POINTER(u32), ctypes.POINTER(i32), ctypes.POINTER(i32), u32]
    library.demux_bayer.argtypes = [ctypes.POINTER(u32)]
    library.fd_crop.argtypes = [ctypes.POINTER(u32), u32, u32, u32]
    library.fd_chroma_crop.argtypes = [ctypes.POINTER(u32)]
    library.luma_diagnostic_plan.argtypes = [ctypes.POINTER(u32),u32]
    library.fd_cst_bias.argtypes = [ctypes.POINTER(u32),u32,u32]
    library.fd_geometry.argtypes = [ctypes.POINTER(u32), u32, u32]
    library.demosaic.argtypes = [ctypes.POINTER(u32)]
    library.crop.argtypes = [ctypes.POINTER(u32)] + [u32] * 4 + [ctypes.c_int]
    library.raw10_input.argtypes = [ctypes.POINTER(u32)] + [u32] * 6
    library.layout.argtypes = [ctypes.POINTER(u32), u32, u32, ctypes.c_uint64, u32]
    library.fd_plan.argtypes = library.layout.argtypes
    library.wb.argtypes = [ctypes.POINTER(u32)] + [u32] * 3 + [ctypes.POINTER(i32)]
    library.cst.argtypes = [ctypes.POINTER(u32)] + [ctypes.POINTER(i32)] * 3 + [ctypes.POINTER(u32)] * 2
    library.bds2_plan.argtypes=[ctypes.POINTER(u32)]
    bds2=(u32*455)();assert library.bds2_plan(bds2) and bds2[0]==227
    if oracle and iq:
        geometry,parameters=oracle.bds2_request()
        assert geometry==(2048,1536,1)
        demo=oracle.demosaic(iq.native_demosaic_bds2(parameters))
        assert list(struct.unpack('<20I',demo))==[4097,2048,1536,7603216,0,0,0,0,0,0,0,0,16,0,0,0,0,1,0,0]
        native=oracle.serialize(0xc3480,demo,0x1983a8)
        scaled,metadata,mnds=oracle.fd_mnds_geometry(2048,1536)
        assert scaled==(1280,960)
        native+=oracle.serialize(0xf37d0,mnds,0x199528)
        native+=oracle.serialize(0xcf6a0,oracle.fd_crop(1280,720,2,x=0,y=120),0x198ea8,
            {0x18:17,0x90:0x8cf4,0x94:0x7360,0x98:0x7368,0x9c:0x7370,0xa0:0x7560,0xa4:0x7568,0xa8:0x7570})
        bds_regs=dict(zip(bds2[1::2],bds2[2::2]))
        assert all(bds_regs[o]==v for o,v in native)
        print('BDS2 request: original frontend half-size geometry, complete IQ/packer/emitter and native FD1280x960/crop720 agree')
    constant_cst=(u32*19)()
    assert library.cst(constant_cst,(i32*9)(*([0]*9)),(i32*3)(0,0,0),
                       (i32*3)(256,512,768),(u32*3)(0,0,0),(u32*3)(1023,1023,1023))
    assert list(constant_cst)==[1,0,0,0,1023,0,256,0,0,0,1023,0,512,0,0,0,1023,0,768]
    if oracle:
        assert bytes(constant_cst)==oracle.cst(struct.pack('<22H',1,0,0,0,256,512,768,
                                                          *([0]*9),0,0,0,1023,1023,1023))
    cases = json.loads((root / 'tests/isp-cases.json').read_text())
    if oracle:
        for control in (0,1,0xa5a50001,0xfffffffe,0xffffffff):
            emitted=oracle.serialize(0xd0c00,struct.pack('<19I',control,*([0]*18)),
                0x198ea8,configuration=bytes(4))
            assert emitted==[(0x6d60,control&~1)]
        print('CST120 bypass: original disable emitter changes only bit0 at6d60')
        for control in (0,1,0xa5a50001,0xfffffffe,0xffffffff):
            emitted=oracle.serialize(0xca3d0,struct.pack('<11I',control,*([0]*10)),
                                     0x1983a8,configuration=struct.pack('<I',0))
            assert emitted==[(0x6460,control&~1)]
        print('CC131 bypass: original disable emitter clears only bit0 and emits only0x6460')
        for control in (0,1,0xa5a50001,0xfffffffe,0xffffffff):
            for address,count,context,offset in ((0xcb640,14,0xdbf4,0xda60),(0xf7380,11,0xe1f4,0xe060)):
                packet=struct.pack(f'<{count}I',0,control,*([0]*(count-2)))
                emitted=oracle.serialize(address,packet,0x1983a8,configuration=bytes(4))
                assert emitted==[(context,0),(offset,control&~1)]
        print('BLS/WB bypass: original context+control disable emitters verified; unrelated control bits preserved')
        luma_packet=oracle.cst(struct.pack('<22H',1,512,512,512,0,0,0,
            *[v&0xffff for v in [601,117,306,-338,510,-172,-427,-83,510]],0,0,0,1023,1023,1023))
        luma_registers=dict(oracle.serialize(0xd0970,luma_packet,0x198ea8))
        assert luma_registers[0x6d78]==512 and luma_registers[0x6d7c]==0
        print('Signed luma probe: original native CST matrix and known bias512 packet/emitter verified')
        for mode in range(7):
            luma_plan=(u32*455)()
            assert library.luma_diagnostic_plan(luma_plan,mode) and luma_plan[0]==227
            registers=dict(zip(luma_plan[1::2],luma_plan[2::2]))
            matrix=([-338,510,-172],[-601,-117,-306],[601,117,306],[1024,0,0],[-1024,0,0],[601,117,306],[0,0,0])[mode]+[-338,510,-172,-427,-83,510]
            packet=oracle.cst(struct.pack('<22H',1,0 if mode>=5 else 512,512,512,0,0,0,
                *[v&0xffff for v in matrix],1023 if mode>=5 else 0,0,0,1023,1023,1023))
            assert [(o,v) for o,v in registers.items() if 0x6d60<=o<=0x6dac]==oracle.serialize(0xd0970,packet,0x198ea8)
        print('Y-row isolation: native Cb/inverted-luma CST packets and full emitter outputs agree')
        for bits in (6,8):
            crop_words=(u32*27)()
            assert library.fd_crop(crop_words,1280,720,bits)
            assert bytes(crop_words)==oracle.fd_crop(1280,720,bits)
        print('Native CRC: six/eight-bit hardware rounding packets verified for internal-range diagnostics')
        chroma=(u32*27)()
        assert library.fd_chroma_crop(chroma)
        assert bytes(chroma)==oracle.fd_crop(1280,720,2,x=0,y=120,chroma_mode=1)
        print('CRC chroma diagnostic: exact original resource17 mode1 crop/round/clamp packet verified')
        bias_plan=(u32*455)()
        assert library.fd_cst_bias(bias_plan,4096,3072) and bias_plan[0]==227
        bias_regs=dict(zip(bias_plan[1::2],bias_plan[2::2]))
        bias_packet=oracle.cst(struct.pack('<22H',1,256,512,768,0,0,0,*([0]*9),0,0,0,1023,1023,1023))
        assert [(o,v) for o,v in bias_regs.items() if 0x6d60<=o<=0x6dac]==oracle.serialize(0xd0970,bias_packet,0x198ea8)
        print('CST alternate offset diagnostic: exact original packer/emitter with zero matrix and separate bias group verified')
        for width,height in [(4096,3072),(3264,2448),(1600,1200),(2304,1728)]:
            geometry_words=(u32*47)()
            assert library.fd_geometry(geometry_words,width,height)
            scaled,metadata,native_mnds=oracle.fd_mnds_geometry(width,height)
            assert scaled==(1280,960)
            crops,controls,native_crc=oracle.fd_crop_geometry(width,height)
            assert crops==[(0,120,1279,839),(0,60,639,419)] and controls==(3,2,0,255,0)
            assert bytes(geometry_words)==native_mnds+native_crc,(width,height)
        print('FD live geometry: all four original module calculations, luma interpolation offsets and centered chroma/luma CRC packets verified')
        for width, height in [(4096,3072), (3264,2448), (1600,1200), (2304,1728), (2048,1536)]:
            pixel = (u32 * 455)()
            assert library.pixel_plan(pixel, width, height) and pixel[0] == 227
            native = oracle.serialize(0xcca70, oracle.common([0]*6, bytes(16)), 0x198da8)
            demux, tables = oracle.serialize(0xd1c40, oracle.demux_bayer(0), 0x198fa8, dmi=True)
            assert not tables
            native += demux
            native += oracle.serialize(0xcb2e0, oracle.channel_gain(struct.pack('<22H', 1,0,0xac,0,0xc9,0,1024,1024,1024,1024,*([0]*12))), 0x1983a8)
            native += oracle.serialize(0xf6e20, oracle.wb(struct.pack('<5H2x6i', 1,0,1024,1024,1024,*([0]*6))), 0x199628)
            demosaic = next(c for c in cases if c['kind'] == 'demosaic')['words']
            native += oracle.serialize(0xc3480, struct.pack('<20I', *demosaic), 0x1983a8)
            native += oracle.serialize(0xc9ea0, oracle.cc(struct.pack('<14H',1,128,0,0,0,128,0,0,0,128,0,0,0,0)), 0x1983a8)
            gamma = oracle.pack(0xd9800, struct.pack('<5H',0,0,0,0,0),12)
            registers,tables = oracle.serialize(0xd9340,gamma,0x1983a8,dmi=True)
            native += registers
            assert tables == [(0x6b08,1,0,256),(0x6b08,2,256,256),(0x6b08,3,512,256)]
            cst = [601,117,306,-338,510,-172,-427,-83,510]
            data = struct.pack('<22H',1,0,512,512,0,0,0,*[v&0xffff for v in cst],0,0,0,1023,1023,1023)
            native += oracle.serialize(0xd0970,oracle.cst(data),0x1983a8)
            native += oracle.serialize(0xf37d0,oracle.mnds(width,height,width/1280,height/720,width/640,height/360),0x1983a8)
            native += oracle.serialize(0xcf6a0,oracle.fd_crop(1280,720,2),0x198ea8,
                {0x18:17,0x90:0x8cf4,0x94:0x7360,0x98:0x7368,0x9c:0x7370,0xa0:0x7560,0xa4:0x7568,0xa8:0x7570})
            emitted = list(zip(pixel[1::2],pixel[2::2]))
            assert emitted == native, (width,height,next(((i,a,b) for i,(a,b) in enumerate(zip(emitted,native)) if a!=b),None))
            if (width,height)!=(2048,1536):
                scaled,metadata,native_mnds=oracle.fd_mnds_geometry(width,height)
                geometry_registers=oracle.serialize(0xf37d0,native_mnds,0x1983a8)
                geometry_registers+=oracle.serialize(0xcf6a0,oracle.fd_crop(1280,720,2,0,120),0x198ea8,
                    {0x18:17,0x90:0x8cf4,0x94:0x7360,0x98:0x7368,0x9c:0x7370,0xa0:0x7560,0xa4:0x7568,0xa8:0x7570})
                replacement=dict(geometry_registers)
                expected=[(offset,replacement.get(offset,value)) for offset,value in native]
                assert library.fd_pixel_plan(pixel,width,height)
                assert list(zip(pixel[1::2],pixel[2::2]))==expected,(width,height)
                if width==4096:
                    scaled,metadata,native_mnds=oracle.fd_mnds_geometry(width,height,1024,768)
                    assert scaled==(1024,768) and struct.unpack('<4I8f',metadata)[4:8]==(4,8,4,8)
                    crops,controls,native_crc=oracle.fd_crop_geometry(width,height,1024,768,1024,768)
                    assert crops==[(0,0,1023,767),(0,0,511,383)]
                    exact_registers=oracle.serialize(0xf37d0,native_mnds,0x1983a8)
                    exact_registers+=oracle.serialize(0xcf6a0,native_crc,0x198ea8,
                        {0x18:17,0x90:0x8cf4,0x94:0x7360,0x98:0x7368,0x9c:0x7370,0xa0:0x7560,0xa4:0x7568,0xa8:0x7570})
                    replacement=dict(exact_registers)
                    expected=[(offset,replacement.get(offset,value)) for offset,value in native]
                    assert library.fd_exact_plan(pixel)
                    assert list(zip(pixel[1::2],pixel[2::2]))==expected
        print('Pixel MMIO: all 227 destinations/values match ten original emitters; Gamma includes all three DMI selectors')
    for case in cases:
        kind = case['kind']
        values = case['input']
        words = (u32 * len(case['words']))()
        if kind == 'mnds':
            result = library.mnds(words, *values)
        elif kind == 'wb':
            result = library.wb(words, *values[:3], (i32 * 6)(*values[3]))
        elif kind == 'demosaic':
            result = library.demosaic(words)
        elif kind == 'crop':
            result = library.crop(words, *values)
        elif kind == 'layout':
            result = library.layout(words, *values)
        elif kind == 'fd_plan':
            result = library.fd_plan(words, *values)
        else:
            result = library.cst(words, (i32 * 9)(*values[0]), (i32 * 3)(*values[1]),
                                 (i32 * 3)(*values[2]), (u32 * 3)(*values[3]), (u32 * 3)(*values[4]))
        assert result and list(words) == case['words'], (kind, values, list(words), case['words'])
    untouched = (u32 * 20)(*[0xa5a5a5a5] * 20)
    for values in [(0, 720, 1280, 720), (4096, 3072, 4097, 3072), (1600, 1200, 1920, 1080),
                   (16386, 1200, 1280, 720), (4096, 3072, 2, 2)]:
        assert not library.mnds(untouched, *values)
        assert list(untouched) == [0xa5a5a5a5] * 20
    assert not library.mnds(None, 4096, 3072, 1280, 720)
    assert not library.wb(untouched, 32768, 1024, 1024, (i32 * 6)(0, 0, 0, 0, 0, 0))
    assert not library.wb(untouched, 1024, 1024, 1024, (i32 * 6)(0, 0, 0, 0, 0, 8388608))
    assert not library.cst(untouched, (i32 * 9)(4096, *([0] * 8)), (i32 * 3)(), (i32 * 3)(),
                           (u32 * 3)(), (u32 * 3)(32767, 32767, 32767))
    assert list(untouched) == [0xa5a5a5a5] * 20
    assert not library.demosaic(None)
    untouched24 = (u32 * 24)(*[0xa5a5a5a5] * 24)
    for values in [(1, 0, 1280, 720, 0), (0, 0, 0, 720, 0), (65534, 0, 4, 720, 0), (0, 0, 65536, 2, 1)]:
        assert not library.crop(untouched24, *values)
        assert list(untouched24) == [0xa5a5a5a5] * 24
    for values in [(1280, 720, 0, 921600), (1280, 720, 0xfffffffff, 1382400),
                   (1280, 720, 0x100000, 1382399), (1279, 720, 0x100000, 1382400)]:
        assert not library.layout(untouched, *values)
        assert list(untouched) == [0xa5a5a5a5] * 20
    untouched84 = (u32 * 84)(*[0xa5a5a5a5] * 84)
    for values in [(1280, 720, 0, 1382400), (1280, 720, 0x100000, 1382399),
                   (4096, 3072, 0xffff00000, 18874368), (1279, 720, 0x100000, 1382400)]:
        assert not library.fd_plan(untouched84, *values)
        assert list(untouched84) == [0xa5a5a5a5] * 84
    assert not library.fd_plan(None, 1280, 720, 0x100000, 1382400)
    raw_offsets = [0x3f0, 0x1800, 0x1804, *range(0x180c, 0x1824, 4),
                   *range(0x190c, 0x1920, 4), *range(0x1824, 0x1834, 4),
                   0x688, 0x68c, 0x690, 0x694, 0x638, 0x60c, 0x600]
    for width, height in [(4096, 3072), (3264, 2448), (2304, 1728), (1600, 1200)]:
        for vc, cid in [(0, 0), (31, 15)]:
            words = (u32 * 50)()
            assert library.raw10_input(words, vc, cid, 0, 0, width, height)
            crop_words = (u32 * 24)()
            assert library.crop(crop_words, 0, 0, width, height, 1)
            if oracle:
                assert bytes(crop_words) == oracle.csid_crop(0, 0, width, height, 1)
                assert list(zip(words[::2], words[1::2]))[:23] == oracle.csid_crop_registers(0, 0, width, height)
                for context_id in (0, 1, 31):
                    original = struct.unpack('<24I', oracle.csid_crop(0, 0, width, height, 1, context_id))
                    expected = list(crop_words)
                    expected[0] = expected[22] = context_id
                    assert list(original) == expected
                    emitted = oracle.csid_crop_registers(0, 0, width, height, context_id)
                    assert emitted[0] == (0x3f0, context_id) and emitted[22] == (0x638, context_id)
            assert list(words[::2]) == raw_offsets
            assert list(words[1::2]) == [*crop_words[:23], 0, vc << 22 | cid << 27 | 0x2b2260]
            assert not words[49] & 0x80000000
            assert words[1] == words[45] == 0
    untouched50 = (u32 * 50)(*[0xa5a5a5a5] * 50)
    for values in [(32, 0, 0, 0, 4096, 3072), (0, 16, 0, 0, 4096, 3072),
                   (0, 0, 16384, 0, 4096, 3072), (0, 0, 0, 16384, 4096, 3072),
                   (0, 0, 0, 0, 4095, 3072)]:
        assert not library.raw10_input(untouched50, *values)
        assert list(untouched50) == [0xa5a5a5a5] * 50
    assert not library.raw10_input(None, 0, 0, 0, 0, 4096, 3072)
    gamma_points = [min(i * 16, 1023) for i in range(65)]
    library.gamma_reference.argtypes=[ctypes.POINTER(u32),u32]
    native_gamma_reference=(u32*67)()
    assert library.gamma_reference(native_gamma_reference,1) and list(native_gamma_reference[:3])==[1,1,1]
    gamma_reference_guard=bytes(native_gamma_reference)
    assert not library.gamma_reference(native_gamma_reference,2) and bytes(native_gamma_reference)==gamma_reference_guard
    assert not library.gamma_reference(None,0)
    gamma_words = (u32 * 67)()
    for bank in (0, 1):
        assert library.gamma_plan(gamma_words, (u32 * 65)(*gamma_points), bank)
        assert list(gamma_words[:3]) == [bank, bank, 1]
        assert gamma_words[3] == 16384 and gamma_words[-1] == 16368
        if oracle:
            assert bytes(gamma_words)[:12] == oracle.fd_gamma_config(bank)
        if iq:
            assert bytes(gamma_words)[12:] == iq.native_fd_gamma(gamma_points, bank)
    untouched67 = (u32 * 67)(*[0xa5a5a5a5] * 67)
    for values, bank in [([0]*64+[1024], 0), ([1]+[0]*64, 0), (gamma_points, 2)]:
        assert not library.gamma_plan(untouched67, (u32 * 65)(*values), bank)
        assert list(untouched67) == [0xa5a5a5a5]*67
    assert library.gamma_plan(gamma_words, (u32 * 65)(*gamma_points), 1)
    command = (u32 * 256)()
    assert library.gamma_dmi(command,1024,0x300000,0x20000,255,gamma_words)
    # Decode with OEM CDM field definitions and compare to the full native emitter.
    assert command[0] >> 24 == 8 and command[0] & 0xffffff == 0x20000
    assert command[1] >> 24 == 4 and command[1] & 65535 == 3
    registers = [(command[2+i*2],command[3+i*2]) for i in range(3)]
    tables = []
    for first in range(8,17,3):
        assert command[first] >> 24 == 1
        length = (command[first]&65535)+1
        source_offset = command[first+1]-0x300000
        assert length == 256 and 256 <= source_offset <= 768
        assert list(command[source_offset//4:(source_offset+length)//4]) == list(gamma_words[3:])
        tables.append((command[first+2]&0xffffff,command[first+2]>>24,source_offset-256,length))
    assert command[17] == 0x06000001 and command[18] == 255
    assert list(command[19:64]) == [0]*45
    # Walk every command using OEM header sizes, not fixed positional slices.
    cursor = 0
    opcodes = []
    while cursor < 19:
        opcode = command[cursor] >> 24
        opcodes.append(opcode)
        if opcode == 8:
            cursor += 1
        elif opcode == 4:
            count = command[cursor] & 65535
            assert count == 3
            cursor += 1 + count * 2
        elif opcode == 1:
            cursor += 3
        elif opcode == 6:
            cursor += 2
        else:
            raise AssertionError(f'Unsupported CDM opcode {opcode} at word {cursor}')
    assert cursor == 19 and opcodes == [8,4,1,1,1,6]
    register_command = (u32 * 256)()
    assert library.gamma_registers(register_command,1024,0x300000,0x20000,255,gamma_words)
    assert list(register_command[:8]) == list(command[:8])
    assert list(register_command[8:10]) == [0x06000001,255]
    assert list(register_command[10:]) == [0]*246  # No DMI header or table fetch.
    if oracle:
        assert (registers,tables) == oracle.serialize(0xd9340,bytes(gamma_words)[:12],0x199128,dmi=True)
    command_guard = (u32 * 256)(*[0xa5a5a5a5]*256)
    for capacity,address,base,marker in [(1023,0x300000,0x20000,0),(1024,0xfffffd00,0x20000,0),
        (1024,0x300001,0x20000,0),(1024,0x300000,0x20001,0),
        (1024,0x300000,0xffa000,0),(1024,0x300000,0x20000,256)]:
        assert not library.gamma_dmi(command_guard,capacity,address,base,marker,gamma_words)
        assert list(command_guard) == [0xa5a5a5a5]*256
        assert not library.gamma_registers(command_guard,capacity,address,base,marker,gamma_words)
        assert list(command_guard) == [0xa5a5a5a5]*256
    print('Gamma CDM: three native DMI destinations, 32-bit source bounds, retained LUT payload and no-write guards passed; not submitted to hardware')
    assert not library.gamma_plan(None, (u32 * 65)(*gamma_points), 0)
    assert not library.gamma_plan(untouched67, None, 0)
    precision_rng = random.Random(267)
    for resource in (1,2):
        for mode in (0,1):
            for context in range(32):
                disabled=(u32*4)()
                control=precision_rng.getrandbits(32) if context>1 else (4 if context else 0)
                assert library.compdecomp_disabled(disabled,resource,context,mode,control)
                if oracle:
                    assert list(zip(disabled[::2],disabled[1::2]))==oracle.compdecomp_disabled(resource,context,mode,control)
    disabled_guard=(u32*4)(*[0xa5a5a5a5]*4)
    for resource,context,mode in ((0,0,1),(17,0,1),(1,32,1),(1,0,2)):
        assert not library.compdecomp_disabled(disabled_guard,resource,context,mode,0)
        assert list(disabled_guard)==[0xa5a5a5a5]*4
        if oracle:
            try:
                oracle.compdecomp_disabled(resource,context,mode)
            except ValueError:
                pass
            else:
                raise AssertionError('Native disable oracle accepted invalid metadata')
    assert not library.compdecomp_disabled(None,1,0,1,0)
    print('CompDecomp disable: original constructor destinations, bit0 update and preserved controls agree for both resources, 32 contexts and two native selectors; not live image evidence')
    for _ in range(1024):
        values = [precision_rng.randrange(2),precision_rng.randrange(4)]
        values += [precision_rng.getrandbits(32) for _ in range(31)]
        values += [precision_rng.randrange(16384) for _ in range(16)]
        values += [precision_rng.randrange(64) for _ in range(16)]
        packed = (u32*49)()
        assert library.compdecomp(packed,(u32*65)(*values))
        if oracle:
            native_input = struct.pack('<2B2x31I16H16B',*values)
            assert bytes(packed) == oracle.compdecomp(native_input)
            for resource,start in [(1,0x8d00),(2,0xe200),(17,0x6200)]:
                offsets = (u32*49)();first=u32()
                assert library.compdecomp_registers(offsets,resource,ctypes.byref(first))
                emitted = oracle.serialize(0xcdeb0,bytes(packed),0x198e28,
                    {0x18:resource,0x90:start+0x1f4,0x94:start+0x60,0x98:start+0x68,
                     0x9c:start+0xb4,0xa0:start+0x100,0xa4:start+0x134})
                assert emitted == list(zip(offsets[first.value:],packed[first.value:]))
    precision_guard = (u32*49)(*[0xa5a5a5a5]*49)
    for index,value in [(0,2),(1,4),(33,16384),(64,64)]:
        invalid=(u32*65)();invalid[index]=value
        assert not library.compdecomp(precision_guard,invalid)
        assert list(precision_guard)==[0xa5a5a5a5]*49
    assert not library.compdecomp(precision_guard,None)
    for _ in range(256):
        native=GtmInput();native.enabled=precision_rng.randrange(2);native.bank=precision_rng.randrange(2)
        native.config[:]=[precision_rng.randrange(64) for _ in range(3)]+[precision_rng.randrange(128)]
        native.base[:]=[precision_rng.randrange(1<<20) for _ in range(256)]
        native.slope[:]=[precision_rng.randrange(1<<24) for _ in range(256)]
        native.shift[:]=[precision_rng.randrange(32) for _ in range(256)]
        native.segments[:]=[precision_rng.getrandbits(32) for _ in range(256)]
        output=GtmOutput();assert library.gtm(ctypes.byref(output),ctypes.byref(native))
        if oracle:
            data=struct.pack('<2H512I256B256I4H',native.enabled,native.bank,*native.base,
                             *native.slope,*native.shift,*native.segments,*native.config)
            registers,table=oracle.gtm(data)
            assert bytes(output.registers)==registers and bytes(output.table)==table
            emitted,tables=oracle.serialize(0xe9e60,registers,0x1992a8,dmi=True)
            assert emitted==[(0x6658+i*4,value) for i,value in enumerate(output.registers)]
            assert tables==[(0x6608,1,0,2048)]
    library.gtm_reference.argtypes=[ctypes.POINTER(GtmOutput),u32]
    gtm_ref=GtmOutput();assert library.gtm_reference(ctypes.byref(gtm_ref),0)
    assert list(gtm_ref.registers[:7])==[0,0,1,16,32,16,0]
    if oracle:
        native_input=struct.pack('<2H512I256B256I4H',1,0,*([1024]*256),*([0]*256),*([30]*256),
            *gtm_ref.registers[7:],16,32,16,0)
        regs,table=oracle.gtm(native_input)
        assert regs==bytes(gtm_ref.registers) and table==bytes(gtm_ref.table)
        if iq:
            spec=importlib.util.spec_from_file_location('gtm_tuning',tools/'isp-tuning.py')
            gtm_tuning=importlib.util.module_from_spec(spec);spec.loader.exec_module(gtm_tuning)
            for tuning_path in args.tuning or []:
                profile=gtm_tuning.Chromatix(tuning_path)
                module=next(item for item in profile.modules if item['name']=='gtm133_tfe_fd_v2' and item['mode']==11)
                root_bytes=profile.payload(module);assert len(root_bytes)==52
                reserve_key=struct.unpack_from('<I',root_bytes,28)[0]
                regions=profile._regions(module,44,262)
                def gtm_leaves(nodes):
                    for node in nodes:
                        yield from node['values'];yield from gtm_leaves(node['children'])
                leaves=list(gtm_leaves(regions));assert len(leaves)==1
                packed=iq.native_gtm_reference(struct.pack('<262f',*leaves[0]),profile.symbols[reserve_key])
                native_regs,native_table=oracle.gtm(packed)
                assert native_regs==bytes(gtm_ref.registers) and native_table==bytes(gtm_ref.table)
            print('GTM reference: four original mode11 leaves, complete native calculation/packing and all256 segments/49-bit LUT words agree')
    assert library.gtm_reference(ctypes.byref(gtm_ref),1) and list(gtm_ref.registers[:3])==[1,1,1]
    library.gtm_dmi.argtypes=[ctypes.POINTER(u32),u32,ctypes.c_uint64,u32,u32,ctypes.POINTER(GtmOutput)]
    gtm_command=(u32*1024)()
    assert library.gtm_dmi(gtm_command,4096,0x300000,0x20000,9,ctypes.byref(gtm_ref))
    assert list(gtm_command[:3])==[0x08020000,0x03000107,0x6658]
    assert list(gtm_command[266:271])==[0x010007ff,0x300500,0x01006608,0x06000001,9]
    assert bytes(gtm_command)[1280:3328]==bytes(gtm_ref.table)
    library.gtm_dmi_format.argtypes=library.gtm_dmi.argtypes+[u32]
    generic_command=bytes(gtm_command)
    assert library.gtm_dmi_format(gtm_command,4096,0x300000,0x20000,9,ctypes.byref(gtm_ref),1)
    assert gtm_command[266]==0x0b0007ff  # Original PacketBuilder CmdType::CDMDMI64 + CSL encoding.
    changed_words=[i for i,(a,b) in enumerate(zip(struct.unpack('<1024I',generic_command),gtm_command)) if a!=b]
    assert changed_words==[266] and bytes(gtm_command)[1280:3328]==bytes(gtm_ref.table)
    native64_command=bytes(gtm_command)
    assert not library.gtm_dmi_format(gtm_command,4096,0x300000,0x20000,9,ctypes.byref(gtm_ref),2)
    assert bytes(gtm_command)==native64_command
    assert library.gtm_dmi_format(gtm_command,4096,0x300000,0x20000,9,ctypes.byref(gtm_ref),0)
    assert bytes(gtm_command)==generic_command
    if oracle:
        regs,tables=oracle.serialize(0xe9e60,bytes(gtm_ref.registers),0x1992a8,dmi=True)
        assert regs==[(gtm_command[2]+i*4,gtm_command[3+i]) for i in range(gtm_command[1]&65535)]
        assert tables==[(gtm_command[268]&0xffffff,gtm_command[268]>>24,0,(gtm_command[266]&65535)+1)]
    old_command=bytes(gtm_command)
    for capacity,address,base,marker in ((3327,0x300000,0x20000,9),(4096,0x300001,0x20000,9),
        (4096,0xffffff00,0x20000,9),(4096,0x300000,0x20001,9),(4096,0x300000,0x20000,256)):
        assert not library.gtm_dmi(gtm_command,capacity,address,base,marker,ctypes.byref(gtm_ref))
        assert bytes(gtm_command)==old_command
    guard_reference=bytes(gtm_ref)
    assert not library.gtm_reference(ctypes.byref(gtm_ref),2) and bytes(gtm_ref)==guard_reference
    assert not library.gtm_reference(None,0)
    gtm_guard=GtmOutput();ctypes.memset(ctypes.byref(gtm_guard),0xa5,ctypes.sizeof(gtm_guard))
    for field,index,value in [('base',255,1<<20),('slope',255,1<<24),('shift',255,32),('config',3,128)]:
        invalid=GtmInput();getattr(invalid,field)[index]=value
        assert not library.gtm(ctypes.byref(gtm_guard),ctypes.byref(invalid))
        assert bytes(gtm_guard)==bytes([0xa5])*ctypes.sizeof(gtm_guard)
    assert not library.gtm(None,ctypes.byref(GtmInput()))
    assert not library.gtm(ctypes.byref(gtm_guard),None)
    if oracle:
        for control in [0,1,0xffffffff]+[precision_rng.getrandbits(32) for _ in range(256)]:
            assert oracle.gtm_disabled(control)==[(0x6660,control&~1)]
        print('GTM disable: original emitter targets control 0x6660, preserves other bits and emits no LUT bank write')
    print('Native precision: complete CompDecomp20 packets/three resources and GTM133 registers/49-bit LUT packing passed; production curves not selected')
    demux_words = (u32 * 92)()
    assert library.demux_bayer(demux_words)
    assert demux_words[2] == 0x805 and sum(demux_words) == 0x805
    if oracle:
        for bayer in range(4):
            assert bytes(demux_words) == oracle.demux_bayer(bayer)
            if iq:
                native_iq=iq.native_ordinary_demux((2,3,4,1)[bayer])
                assert bytes(demux_words)==oracle.demux_bayer(bayer,native_iq)
        if iq:
            for sensor_format in (0,5,19):
                try:
                    iq.native_ordinary_demux(sensor_format)
                    raise AssertionError('Non-ordinary Bayer format was accepted')
                except ValueError:
                    pass
    assert not library.demux_bayer(None)
    channel_words = (u32 * 14)()
    for bayer in range(4):
        assert library.channel_gain_reference(channel_words,bayer)
        if iq:
            for bits in (14,16):
                native=iq.native_blacklevel([0]*5,bayer,bits=bits)
                assert bytes(channel_words)==oracle.channel_gain(native)
    channel_reference_guard=(u32*14)(*[0xa5a5a5a5]*14)
    assert not library.channel_gain_reference(channel_reference_guard,4)
    assert list(channel_reference_guard)==[0xa5a5a5a5]*14
    assert not library.channel_gain_reference(None,0)
    print('Channel gain reference: all four nonzero Bayer selector pairs, native zero-black/unity-gain calculation and complete packer agree')
    cc_words = (u32 * 11)()
    identity = [128, 0, 0, 0, 128, 0, 0, 0, 128]
    assert library.cc_plan(cc_words, (i32 * 9)(*identity), (i32 * 3)(), 0)
    if oracle:
        assert bytes(cc_words) == oracle.cc(struct.pack('<14H', 1, *identity, 0, 0, 0, 0))
    cc_guard = (u32 * 11)(*[0xa5a5a5a5]*11)
    for matrix, offsets, q in [(identity, [0]*3, 4), ([2048]+[0]*8, [0]*3, 0), (identity, [16384,0,0], 0)]:
        assert not library.cc_plan(cc_guard, (i32 * 9)(*matrix), (i32 * 3)(*offsets), q)
        assert list(cc_guard) == [0xa5a5a5a5]*11
    channel_guard = (u32 * 14)(*[0xa5a5a5a5]*14)
    for packet in [[2]+[0]*21, [1,8]+[0]*20, [1]+[0]*21, [0]*21+[65536]]:
        assert not library.channel_gain(channel_guard, (u32 * 22)(*packet))
        assert list(channel_guard) == [0xa5a5a5a5]*14
    assert not library.channel_gain(None, (u32 * 22)())
    assert not library.channel_gain(channel_guard, None)
    crop27 = (u32 * 27)()
    assert library.fd_crop(crop27, 1280, 720, 2)
    if oracle:
        assert bytes(crop27) == oracle.fd_crop(1280, 720, 2)
        emitted = oracle.serialize(0xcf6a0, bytes(crop27), 0x198ea8,
                                   {0x18:17, 0x90:0x8cf4, 0x94:0x7360, 0x98:0x7368, 0x9c:0x7370,
                                    0xa0:0x7560, 0xa4:0x7568, 0xa8:0x7570})
        expected_offsets = [0x7360, 0x7368, 0x736c, *range(0x7370,0x7398,4),
                            0x7560, 0x7568, 0x756c, *range(0x7570,0x7598,4)]
        assert emitted == list(zip(expected_offsets, list(crop27)[1:])), emitted
    crop_guard = (u32 * 27)(*[0xa5a5a5a5]*27)
    for values in [(1, 720, 2), (1280, 719, 2), (16386, 720, 2), (1280, 720, 16)]:
        assert not library.fd_crop(crop_guard, *values)
        assert list(crop_guard) == [0xa5a5a5a5]*27
    if iq:
        for width, height in [(4096, 3072), (3264, 2448), (1600, 1200), (2304, 1728), (2, 2), (16384, 16384)]:
            for bayer in range(4):
                words = (u32 * 20)()
                assert library.demosaic(words)
                assert bytes(words) == oracle.demosaic(iq.native_demosaic(width, height, bayer))
    if args.tuning:
        spec = importlib.util.spec_from_file_location('isp_tuning', tools / 'isp-tuning.py')
        tuning = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tuning)
        black_regions = 0
        gamma_regions = cc_regions = 0
        for path in args.tuning:
            profile = tuning.Chromatix(path)
            profile.manifest()
            # First mode5 SW reserve is a reference, not a production-mode
            # selection or a complete CamX metadata decoder.
            reserve=next(item for item in profile.modules if item['name']=='compand20_sw')
            payload=profile.payload(reserve)
            assert reserve['mode']==5 and reserve['version']==[2,0,0] and len(payload)==152
            points=struct.unpack('<32I',payload[24:])
            native_compand=iq.native_compand_reference(points)
            fields=struct.unpack('<2B2x31I16H16B',native_compand)
            packed_compand=(u32*49)()
            assert library.compdecomp(packed_compand,(u32*65)(*fields))
            assert bytes(packed_compand)==oracle.compdecomp(native_compand)
            reference_compand=(u32*49)()
            assert library.compand_raw10_reference(reference_compand)
            assert bytes(reference_compand)==bytes(packed_compand)
            reference_offsets=(u32*49)();reference_first=u32()
            assert library.compdecomp_registers(reference_offsets,17,ctypes.byref(reference_first)) and reference_first.value==1
            destinations=[0x63f4,0x6260,0x6268,0x62b4,0x6300,0x6334]
            emitted=oracle.serialize(0xcdeb0,bytes(reference_compand),0x198e28,
                {0x18:17,**{0x90+i*4:v for i,v in enumerate(destinations)}},configuration=bytes(4))
            assert emitted==list(zip(reference_offsets[1:],reference_compand[1:]))
            for invalid in (points[:-1],(-1,)+points[1:],points[:31]+(4,)):
                try:
                    iq.native_compand_reference(invalid)
                except ValueError:
                    pass
                else:
                    raise AssertionError('Native compression accepted invalid reserve')
            for item in profile.modules:
                def leaves(nodes):
                    for node in nodes:
                        yield from node['values']
                        yield from leaves(node['children'])
                if item['name'] == 'gamma162_tfe_fd_v2':
                    for curve in leaves(profile.gamma_regions(item)):
                        assert len(curve) == 65 and curve[0] == 0 and curve[-1] == 1023
                        assert all(a <= b for a, b in zip(curve, curve[1:]))
                        rounded = [int(value + 0.5) for value in curve]
                        assert library.gamma_plan(gamma_words, (u32 * 65)(*rounded), 0)
                        assert bytes(gamma_words)[12:] == iq.native_fd_gamma(curve)
                        assert library.gamma_reference(native_gamma_reference,0)
                        assert bytes(native_gamma_reference)==bytes(gamma_words)
                        gamma_regions += 1
                    continue
                if item['name'] == 'cc13_tfe_fd_v2':
                    for values in leaves(profile.cc_regions(item)):
                        assert len(values) == 12
                        assert values == [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]
                        cc_regions += 1
                    continue
                if item['name'] == 'demuxblklevel142_tfe_v2':
                    for values in leaves(profile.demux_regions(item)):
                        for bayer in range(4):
                            native = iq.native_blacklevel(values, bayer)
                            halfwords = struct.unpack('<22H', native)
                            assert library.channel_gain(channel_words, (u32 * 22)(*halfwords))
                            assert bytes(channel_words) == oracle.channel_gain(native)
                            assert halfwords[0] == 1 and any(halfwords[6:10])
                            bypass = struct.unpack('<22H', iq.native_blacklevel(values, bayer, disable_offsets=True))
                            assert halfwords[:10] == bypass[:10]
                            assert not any(bypass[10:])
                        black_regions += 1
                    continue
                if item['name'] != 'cst12_tfe_fd':
                    continue
                parameters = profile.cst(item)
                matrix = [int(v * 1024 + (0.5 if v >= 0 else -0.5)) for v in parameters['matrix']]
                words = (u32 * 19)()
                assert library.cst(words, (i32 * 9)(*matrix), (i32 * 3)(*parameters['add']),
                                   (i32 * 3)(*parameters['subtract']), (u32 * 3)(*parameters['low']), (u32 * 3)(*parameters['high']))
                assert bytes(words) == oracle.cst(iq.fd_cst(profile.payload(item)))
        invalid_path = Path(directory) / 'invalid-tuning.bin'
        original = tuning.Chromatix(args.tuning[0]).blob
        for position, value, checksum in [(32, 6, True), (176, len(original), True),
                                           (180, 0, True), (168, 1, True), (100, 1, False)]:
            invalid = bytearray(original)
            struct.pack_into('<I', invalid, position, value)
            if checksum:
                struct.pack_into('<I', invalid, len(invalid) - 4, zlib.crc32(invalid[:-4]))
            invalid_path.write_bytes(invalid)
            try:
                tuning.Chromatix(invalid_path)
            except ValueError:
                pass
            else:
                raise AssertionError('Corrupt tuning file was accepted')
        expected = {
            14: [1, 0, 172, 0, 201, 0, 1092, 1092, 1092, 1092, 0, 1026, 0, 1026, 0, 1024, 0, 1024, 1024, 0, 0, 0],
            16: [1, 0, 172, 0, 201, 0, 1040, 1040, 1040, 1040, 0, 1026, 0, 1026, 0, 1024, 0, 1024, 1024, 0, 0, 0],
        }
        for bits, reference in expected.items():
            assert list(struct.unpack('<22H', iq.native_blacklevel([0, 1024, 1026, 1026, 1024], 0, bits=bits))) == reference
        asymmetric = [0, 300, 600, 900, 1200]
        outputs = [struct.unpack('<22H', iq.native_blacklevel(asymmetric, bayer, gains=(1, 2, 3, 4))) for bayer in range(4)]
        assert len(set(row[:10] for row in outputs)) == 4
        assert all(any(row[10:]) for row in outputs)
        assert iq.native_blacklevel(asymmetric, 0, bits=14) != iq.native_blacklevel(asymmetric, 0, bits=16)
        for invalid in [dict(values=[0, 0, 0, 0, float('nan')], bayer=0),
                        dict(values=[0, 65535, 0, 0, 0], bayer=0),
                        dict(values=[0]*5, bayer=4), dict(values=[0]*5, bayer=0, bits=10),
                        dict(values=[0]*5, bayer=0, gains=(0, 1, 1, 1))]:
            try:
                iq.native_blacklevel(**invalid)
            except ValueError:
                pass
            else:
                raise AssertionError('Invalid original black-level input was accepted')
        print(f'Original version 5 tuning: CRC, bounded sections, symbol/module aliases, native TFE CST and {black_regions} black-level regions verified for {len(args.tuning)} cameras')
        print('Original compression reference: four mode5 SW reserves, native calculation with bounded libc log and exact CompDecomp packing passed; production metadata not selected')
        print(f'Original FD profiles: {gamma_regions} bounded monotonic 65-point gamma regions and {cc_regions} identity color-correction regions decoded; no production mode selected')
    if oracle:
        randomizer = random.Random(980)
        for _ in range(1024):
            throttles = [randomizer.randrange(65536 if i&1 else 2) for i in range(6)]
            flags = [randomizer.randrange(4 if i==4 else 2) for i in range(5)]
            pdaf = [randomizer.randrange(2) for _ in range(2)]
            hdr = randomizer.randrange(2)
            common = (u32 * 13)()
            assert library.common_plan(common,(u32 * 6)(*throttles),(u32 * 5)(*flags),(u32 * 2)(*pdaf),hdr)
            assert bytes(common) == oracle.common(throttles,struct.pack('<5B3x2I',*flags,*pdaf),hdr)
        print('Common10: 1024 throttle/mux/native overlapping PDAF flag comparisons passed')
        if iq:
            for _ in range(1024):
                values = sorted(randomizer.randrange(1024) for _ in range(65))
                bank = randomizer.randrange(2)
                assert library.gamma_plan(gamma_words, (u32 * 65)(*values), bank)
                assert bytes(gamma_words)[:12] == oracle.fd_gamma_config(bank)
                assert bytes(gamma_words)[12:] == iq.native_fd_gamma(values, bank)
            print('FD gamma: 1024 native IQ LUT/config comparisons passed')
        for _ in range(1024):
            matrix = [randomizer.randrange(-2048, 2048) for _ in range(9)]
            offsets = [randomizer.randrange(-16384, 16384) for _ in range(3)]
            q = randomizer.randrange(4)
            assert library.cc_plan(cc_words, (i32 * 9)(*matrix), (i32 * 3)(*offsets), q)
            payload = struct.pack('<14H', 1, *(v & 0xffff for v in matrix), *(v & 0xffff for v in offsets), q)
            assert bytes(cc_words) == oracle.cc(payload)
        print('CC131: 1024 signed matrix/offset comparisons against the original packer passed')
        for _ in range(1024):
            width = randomizer.randrange(1,8193)*2
            height = randomizer.randrange(1,8193)*2
            bits = randomizer.randrange(16)
            assert library.fd_crop(crop27, width, height, bits)
            assert bytes(crop27) == oracle.fd_crop(width, height, bits)
        print('FD CRC13: paired luma/chroma register emission and 1024 native crop/round/clamp comparisons passed')
        for _ in range(2048):
            width = randomizer.randrange(1, 2049) * 2
            height = randomizer.randrange(1, 1537) * 2
            x = randomizer.randrange(0, (65536 - width) // 2 + 1) * 2
            y = randomizer.randrange(0, (65536 - height) // 2 + 1) * 2
            measure = randomizer.randrange(2)
            words = (u32 * 24)()
            assert library.crop(words, x, y, width, height, measure)
            assert bytes(words) == oracle.csid_crop(x, y, width, height, measure)
        for _ in range(2048):
            width = randomizer.randrange(2, 8193) * 2
            height = randomizer.randrange(2, 8193) * 2
            outWidth = randomizer.randrange(max(1, (width + 127) // 128), width // 2 + 1) * 2
            outHeight = randomizer.randrange(max(1, (height + 127) // 128), height // 2 + 1) * 2
            words = (u32 * 20)()
            assert library.mnds(words, width, height, outWidth, outHeight)
            expected = oracle.mnds(width, height, width / outWidth, height / outHeight,
                                   width / (outWidth // 2), height / (outHeight // 2))
            assert bytes(words) == expected, (width, height, outWidth, outHeight, list(words), struct.unpack('<20I', expected))
            gains = [randomizer.randrange(1, 32768) for _ in range(3)]
            offsets = [randomizer.randrange(-8388608, 8388608) for _ in range(6)]
            words = (u32 * 11)()
            assert library.wb(words, *gains, (i32 * 6)(*offsets))
            data = struct.pack('<5H2x6i', 1, 0, gains[1], gains[2], gains[0], *offsets)
            assert bytes(words) == oracle.wb(data)
            matrix = [randomizer.randrange(-4096, 4096) for _ in range(9)]
            subtract = [randomizer.randrange(-8192, 8192) for _ in range(3)]
            add = [randomizer.randrange(-8192, 8192) for _ in range(3)]
            low = [randomizer.randrange(32768) for _ in range(3)]
            high = [randomizer.randrange(v, 32768) for v in low]
            words = (u32 * 19)()
            assert library.cst(words, (i32 * 9)(*matrix), (i32 * 3)(*subtract), (i32 * 3)(*add),
                               (u32 * 3)(*low), (u32 * 3)(*high))
            data = struct.pack('<22H', 1, *[v & 0xffff for v in subtract],
                               *[v & 0xffff for v in add], *[v & 0xffff for v in matrix], *low, *high)
            assert bytes(words) == oracle.cst(data)
    print(f'Apollo980 ISP configuration: {len(cases)} reference fixtures and invalid-input guards passed' +
          ('; 8192 randomized comparisons against original ARM64 packers passed' if oracle else '') +
          ('; native demosaic matches original IQ calculation for four Bayer patterns and six geometries' if iq else ''))
