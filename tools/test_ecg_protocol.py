"""Verify actual C-encoded frames with the supplied host protocol.py parser."""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser()
ap.add_argument('protocol', type=Path, help='Path to the host protocol.py')
ap.add_argument('--cc')
args = ap.parse_args()
cc = args.cc or shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
cc = cc or str(root / 'build/testdeps/ziglang/zig.exe')
out = root / 'build/host-tests'
out.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env['ZIG_GLOBAL_CACHE_DIR'] = str(out / 'zig-cache')
exe = out / ('test_ecg_protocol.exe' if os.name == 'nt' else 'test_ecg_protocol')
command = [cc] + (['cc'] if Path(cc).stem == 'zig' else [])
command += ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'Inc'),
            str(root / 'tests/test_ecg_protocol.c'), str(root / 'Src/ecg_protocol.c'),
            '-o', str(exe)]
subprocess.run(command, check=True, env=env)
data = out / 'ecg_protocol_frames.bin'
subprocess.run([str(exe), str(data)], check=True)
spec = importlib.util.spec_from_file_location('host_protocol', args.protocol)
host = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = host
spec.loader.exec_module(host)
parser = host.FrameParser()
frames = []
wire = data.read_bytes()
for start in range(0, len(wire), 17):
    frames += parser.feed(wire[start:start + 17])
assert len(frames) == 7 and parser.stats['frames_crc_error'] == 0
assert [f.frame_seq for f in frames] == [0, 1, 2, 4, 5, 6, 7]
assert [f.flags for f in frames] == [0, 0, 2, 3, 0, 0, 0]
assert [f.sample0 for f in frames] == [0, 50, 100, 200, 250, 0xFFFFFFE7, 25]
for f in frames:
    assert f.count == 50 and f.sample_rate == 500 and f.version == f.type == 1
    assert [r.raw for r in f.samples] == list(range(2000, 2050))
    assert [r.filtered for r in f.samples[:4]] == [-32768, 32767, -2, 2]
    assert f.samples[20].r_seq == (f.sample0 + 10) & 0xFFFFFFFF
    assert all(r.r_seq is None for i, r in enumerate(f.samples) if i != 20)
assert frames[0].hr is frames[0].sd_rr is frames[0].rmssd_rr is None
assert all(r.quality == 8 and r.alarm == 0 for r in frames[0].samples)
assert (frames[1].hr, frames[1].sd_rr, frames[1].rmssd_rr) == (72.3, 12.4, 24.6)
assert all(r.alarm == 2 for r in frames[1].samples)
assert all(r.quality == 1 and r.alarm == 1 for r in frames[3].samples)
assert all(r.alarm == 3 for r in frames[4].samples)
print('PASS: host parser accepted C frames; CRC, rounding, metrics, R indices, gaps, drops and wraparound')
