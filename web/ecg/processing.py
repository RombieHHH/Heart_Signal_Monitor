# -*- coding: utf-8 -*-
"""Streaming ECG processing for compact Bluetooth level samples."""

from collections import deque
from dataclasses import dataclass
import math
import statistics


@dataclass
class ProcessedSample:
    filtered: int
    r_seq: int | None
    quality: int
    alarm: int
    hr: float | None
    sdnn: float | None
    rmssd: float | None
    landmarks: list


class HostECGProcessor:
    """Filter, detect P/Q/R/S/T and calculate short-window HR/HRV."""

    def __init__(self, sample_rate=50):
        self.sample_rate = sample_rate
        self.reset()

    def reset(self):
        self.index = 0
        self.baseline = None
        self.lowpass = 0.0
        self.previous = 0.0
        self.energy_window = deque(maxlen=max(3, round(0.12 * self.sample_rate)))
        self.learning_energy = []
        self.previous_energy = 0.0
        self.pending_energy = 0.0
        self.pending_index = 0
        self.last_r = None
        self.rr = deque(maxlen=30)
        self.raw_window = deque(maxlen=2 * self.sample_rate)
        self.clip_window = deque(maxlen=2 * self.sample_rate)
        self.signal_level = 0.0
        self.noise_level = 0.0
        self.threshold = float('inf')
        self.alarm_active = False
        self.abnormal_updates = 0
        self.normal_updates = 0
        self.filtered_history = {}
        self.history_order = deque(maxlen=512)
        self.pending_landmark = None

    def push(self, raw, sequence):
        landmark_updates = []
        raw = float(raw)
        if self.baseline is None:
            self.baseline = raw
        hp_a = math.exp(-2.0 * math.pi * 0.5 / self.sample_rate)
        lp_a = math.exp(-2.0 * math.pi * 15.0 / self.sample_rate)
        self.baseline = hp_a * self.baseline + (1.0 - hp_a) * raw
        highpassed = raw - self.baseline
        self.lowpass = lp_a * self.lowpass + (1.0 - lp_a) * highpassed
        if len(self.history_order) == self.history_order.maxlen:
            self.filtered_history.pop(self.history_order[0], None)
        self.history_order.append(sequence)
        self.filtered_history[sequence] = self.lowpass
        landmark_updates.extend(self._finish_landmarks(sequence))

        derivative = self.lowpass - self.previous
        self.previous = self.lowpass
        self.energy_window.append(derivative * derivative)
        energy = sum(self.energy_window) / len(self.energy_window)
        self.raw_window.append(raw)
        self.clip_window.append(raw <= 24.0 or raw >= 4072.0)

        learning = self.index < 2 * self.sample_rate
        if learning:
            self.learning_energy.append(energy)
            if self.index == 2 * self.sample_rate - 1:
                ordered = sorted(self.learning_energy)
                self.noise_level = statistics.median(ordered)
                self.signal_level = ordered[max(0, int(len(ordered) * 0.95) - 1)]
                self._set_threshold()

        r_seq = None
        # One-sample-delayed local maximum of integrated slope energy.
        if not learning and self.pending_energy >= self.previous_energy and self.pending_energy > energy:
            refractory = self.last_r is None or self.pending_index - self.last_r >= round(0.25 * self.sample_rate)
            if refractory and self.pending_energy > self.threshold:
                r_seq = self._waveform_peak(self.pending_index)
                self.signal_level = 0.875 * self.signal_level + 0.125 * self.pending_energy
                self._accept_r(r_seq)
                landmark_updates.extend(self._start_landmarks(r_seq))
            else:
                self.noise_level = 0.875 * self.noise_level + 0.125 * self.pending_energy
            self._set_threshold()
        self.previous_energy = self.pending_energy
        self.pending_energy = energy
        self.pending_index = sequence

        quality = 8 if learning else 0
        if len(self.clip_window) == self.clip_window.maxlen and sum(self.clip_window) > len(self.clip_window) // 10:
            quality |= 2
        if len(self.raw_window) == self.raw_window.maxlen and max(self.raw_window) - min(self.raw_window) < 16:
            quality |= 4
        invalid = bool(quality & 6)
        hr, sdnn, rmssd = self._metrics()
        alarm = 1 if invalid else (3 if self.alarm_active else (2 if sdnn is not None else 0))
        self.index += 1
        return ProcessedSample(round(self.lowpass), r_seq, quality, alarm,
                               None if invalid else hr,
                               None if invalid else sdnn,
                               None if invalid else rmssd, landmark_updates)

    def _waveform_peak(self, detected):
        candidates = [(abs(value), seq) for seq, value in self.filtered_history.items()
                      if 0 <= detected - seq < round(0.16 * self.sample_rate)]
        return max(candidates, default=(0.0, detected))[1]

    def _smoothed(self, sequence):
        values = [self.filtered_history.get(sequence + offset)
                  for offset in range(-2, 3)]
        if any(value is None for value in values):
            return None
        return sum(values) / 5.0

    def _extremum(self, r_seq, first, last, point, direction, fraction, amplitude):
        left = self._smoothed(r_seq + first)
        right = self._smoothed(r_seq + last)
        if left is None or right is None or last - first < 8:
            return None
        floor = max(direction * left, direction * right)
        best, chosen = floor, None
        for offset in range(first + 3, last - 2):
            seq = r_seq + offset
            before, value, after = self._smoothed(seq - 1), self._smoothed(seq), self._smoothed(seq + 1)
            if before is None or value is None or after is None:
                continue
            value *= direction
            if value > best and value >= direction * before and value > direction * after:
                best, chosen = value, seq
        if chosen is not None and best - floor >= max(4.0, fraction * amplitude):
            return (chosen, point)
        return None

    def _start_landmarks(self, r_seq):
        value = self.filtered_history.get(r_seq, 0.0)
        polarity = 1.0 if value >= 0 else -1.0
        amplitude = abs(value)
        rr_ms = self.rr[-1] if self.rr else 1000.0
        p_start = max(-120, -int(rr_ms * 0.15))
        t_end = min(180, int(rr_ms * 0.27))
        updates = [(r_seq, 1)]
        for args in ((p_start, -40, 2, polarity, .02), (-33, -4, 3, -polarity, .015)):
            mark = self._extremum(r_seq, *args, amplitude)
            if mark is not None:
                updates.append(mark)
        self.pending_landmark = [r_seq, polarity, amplitude, t_end, True, True]
        return updates

    def _finish_landmarks(self, sequence):
        pending = self.pending_landmark
        if pending is None:
            return []
        r_seq, polarity, amplitude, t_end, pending_s, pending_t = pending
        updates = []
        if pending_s and sequence - r_seq >= 43:
            mark = self._extremum(r_seq, 4, 40, 4, -polarity, .015, amplitude)
            if mark is not None:
                updates.append(mark)
            pending[4] = False
        if pending_t and sequence - r_seq >= t_end + 3:
            mark = self._extremum(r_seq, 45, t_end, 5, polarity, .04, amplitude)
            if mark is not None:
                updates.append(mark)
            pending[5] = False
        if not pending[4] and not pending[5]:
            self.pending_landmark = None
        return updates

    def _set_threshold(self):
        self.threshold = self.noise_level + 0.30 * max(0.0, self.signal_level - self.noise_level)

    def _accept_r(self, peak):
        if self.last_r is not None:
            interval = peak - self.last_r
            bpm = 60.0 * self.sample_rate / interval if interval else 0.0
            if 30.0 <= bpm <= 220.0:
                self.rr.append(1000.0 * interval / self.sample_rate)
            else:
                self.rr.clear()
        self.last_r = peak
        self._update_alarm()

    def _metrics(self):
        if not self.rr:
            return None, None, None
        recent = list(self.rr)[-5:]
        hr = 60000.0 / (sum(recent) / len(recent))
        if len(self.rr) < 30:
            return round(hr, 1), None, None
        values = list(self.rr)
        mean = sum(values) / len(values)
        sdnn = math.sqrt(sum((x - mean) ** 2 for x in values) / (len(values) - 1))
        rmssd = math.sqrt(sum((b - a) ** 2 for a, b in zip(values, values[1:])) / (len(values) - 1))
        return round(hr, 1), round(sdnn, 1), round(rmssd, 1)

    def _update_alarm(self):
        _, sdnn, rmssd = self._metrics()
        if sdnn is None:
            return
        mean = sum(self.rr) / len(self.rr)
        abnormal = sdnn / mean > 0.10 or rmssd / mean > 0.12
        clear = sdnn / mean < 0.08 and rmssd / mean < 0.10
        if not self.alarm_active:
            self.abnormal_updates = self.abnormal_updates + 1 if abnormal else 0
            if self.abnormal_updates >= 3:
                self.alarm_active = True
                self.abnormal_updates = 0
        else:
            self.normal_updates = self.normal_updates + 1 if clear else 0
            if self.normal_updates >= 5:
                self.alarm_active = False
                self.normal_updates = 0
