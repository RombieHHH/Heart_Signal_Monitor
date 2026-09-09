"""Compile and run the actual portable C pipeline; no hardware or HAL needed."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--cc', help='Path to gcc, clang, or zig executable')
args = parser.parse_args()
compiler = args.cc or shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
bundled = ROOT / 'build/testdeps/ziglang/zig.exe'
if not compiler and bundled.exists():
    compiler = str(bundled)
if not compiler:
    parser.error('Provide --cc with a native C compiler (gcc, clang, or zig).')
out = ROOT / 'build/host-tests'
out.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env['ZIG_GLOBAL_CACHE_DIR'] = str(out / 'zig-cache')
command = [compiler]
if Path(compiler).stem == 'zig':
    command += ['cc']
sources = ['ecg_preprocess', 'qrs_detector', 'heart_rate', 'hrv', 'ecg_monitor', 'ecg_plot']
exe = out / ('test_ecg.exe' if os.name == 'nt' else 'test_ecg')
command += ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'Inc'),
            str(ROOT / 'tests/test_ecg.c')]
command += [str(ROOT / 'Src' / (name + '.c')) for name in sources]
command += ['-lm', '-o', str(exe)]
subprocess.run(command, check=True, cwd=ROOT, env=env)
subprocess.run([str(exe)], check=True, cwd=ROOT)
