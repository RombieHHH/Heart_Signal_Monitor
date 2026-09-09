"""Check a raw binary UART capture without opening a serial port."""
import argparse
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'web'))
from ecg.protocol import FrameParser, HEADER_SIZE, MAX_COUNT, RECORD_SIZE


def analyze(data):
    parser = FrameParser()
    frames = parser.feed(data)
    headers = []
    for offset in range(len(data) - HEADER_SIZE + 1):
        if data[offset:offset + 4] != b'\xa5\x5a\x01\x01':
            continue
        fields = struct.unpack_from('<HBBHHIIHHHHH', data, offset)
        payload, seq, sample0, rate, count = fields[3], fields[5], fields[6], fields[7], fields[8]
        if not (0 < count <= MAX_COUNT and payload == count * RECORD_SIZE and rate == 500):
            continue
        headers.append((offset, seq, sample0, count))
    print(f'Capture: {len(data)} bytes; CRC-valid frames: {len(frames)}')
    print('Parser statistics:', parser.stats)
    print('Header candidates (not CRC-verified): offset, frame_seq, sample0, count')
    for header in headers[:20]:
        print(header)
    # The fixed 50-sample firmware increments frame_seq for every built/dropped frame.
    print('Adjacent 50-sample headers: seq1 -> seq2, received span / nominal span')
    for first, second in list(zip(headers, headers[1:]))[:20]:
        delta = (second[1] - first[1]) & 0xFFFFFFFF
        if first[3] == second[3] == 50 and 0 < delta < 10000:
            print(f'{first[1]} -> {second[1]}: {second[0] - first[0]} / {delta * 528} bytes')
    print('Nominal spans assume no MCU-side frame drops; CRC-valid frames remain the acceptance criterion.')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path)
    analyze(ap.parse_args().capture.read_bytes())
