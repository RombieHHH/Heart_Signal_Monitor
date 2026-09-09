"""Compile the MCU compact encoder and verify it with the web parser."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
cc = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
cc = cc or str(root / 'build/testdeps/ziglang/zig.exe')
out = root / 'build/host-tests'
out.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env['ZIG_GLOBAL_CACHE_DIR'] = str(out / 'zig-cache')
exe = out / ('test_ecg_bt_protocol.exe' if os.name == 'nt' else 'test_ecg_bt_protocol')
command = [cc] + (['cc'] if Path(cc).stem == 'zig' else [])
command += ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'Inc'),
            str(root / 'tests/test_ecg_bt_protocol.c'), str(root / 'Src/ecg_bt_protocol.c'),
            '-o', str(exe)]
subprocess.run(command, check=True, env=env)
wire = out / 'ecg_bt_frame.bin'
subprocess.run([str(exe), str(wire)], check=True)
sys.path.insert(0, str(root / 'web'))
from ecg.protocol import FrameParser
p = FrameParser()
frames = []
data = b'noise' + wire.read_bytes()
for start in range(0, len(data), 7):
    frames.extend(p.feed(data[start:start + 7]))
assert len(frames) == 1 and p.stats['frames_crc_error'] == 0
f = frames[0]
assert (f.version, f.type, f.sample_rate, f.count, len(data) - 5) == (2, 2, 500, 50, 78)
expected = [(((i >> 4) << 4) + 8) for i in range(50)]
assert f.sample0 == 0 and [s.raw for s in f.samples] == expected
print('PASS: compact C encoder -> fragmented web parser, 78 bytes, CRC valid')
