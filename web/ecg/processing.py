# -*- coding: utf-8 -*-
"""Streaming ECG processing for compact Bluetooth level samples."""

from collections import deque
from dataclasses import dataclass
import math
import statistics


class _Biquad:
    """Direct-form-II biquad, matching the MCU implementation."""

    def __init__(self, coefficients):
        self.b0, self.b1, self.b2, self.a1, self.a2 = coefficients
        self.s1 = 0.0
        self.s2 = 0.0

    def set_steady_state(self, input_value, output_value):
        self.s1 = output_value - self.b0 * input_value
        self.s2 = self.b2 * input_value - self.a2 * output_value

    def push(self, value):
        output = self.b0 * value + self.s1
        self.s1 = self.b1 * value - self.a1 * output + self.s2
        self.s2 = self.b2 * value - self.a2 * output
        return output


def _butterworth(cutoff, sample_rate, highpass=False):
    omega = 2.0 * math.pi * cutoff / sample_rate
    cosine, sine = math.cos(omega), math.sin(omega)
    alpha = sine / math.sqrt(2.0)
    scale = 1.0 / (1.0 + alpha)
    if highpass:
        b0 = (1.0 + cosine) * 0.5 * scale
        b1 = -(1.0 + cosine) * scale
    else:
        b0 = (1.0 - cosine) * 0.5 * scale
        b1 = (1.0 - cosine) * scale
    return b0, b1, b0, -2.0 * cosine * scale, (1.0 - alpha) * scale


def _notch(frequency, quality, sample_rate):
    omega = 2.0 * math.pi * frequency / sample_rate
    cosine, sine = math.cos(omega), math.sin(omega)
    alpha = sine / (2.0 * quality)
    scale = 1.0 / (1.0 + alpha)
    return (scale, -2.0 * cosine * scale, scale,
            -2.0 * cosine * scale, (1.0 - alpha) * scale)


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

    def __init__(self, sample_rate=250):
        self.sample_rate = sample_rate
        self.reset()

    def reset(self):
        self.index = 0
        self.filters_initialized = False
        self.display_highpass = _Biquad(_butterworth(0.7, self.sample_rate, True))
        self.display_notch = _Biquad(_notch(50.0, 25.0, self.sample_rate))
        self.display_lowpass = _Biquad(_butterworth(25.0, self.sample_rate))
        self.qrs_highpass = _Biquad(_butterworth(5.0, self.sample_rate, True))
        self.qrs_lowpass = _Biquad(_butterworth(15.0, self.sample_rate))
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
        self.suppress_until = 0

    def handle_gap(self):
        """Break timing continuity without discarding previously valid RR data."""
        self.last_r = None
        self.energy_window.clear()
        self.previous_energy = 0.0
        self.pending_energy = 0.0
        self.pending_landmark = None
        self.filtered_history.clear()
        self.history_order.clear()
        self.suppress_until = self.index + round(0.25 * self.sample_rate)

    def push(self, raw, sequence):
        landmark_updates = []
        raw = float(raw)
        if not self.filters_initialized:
            self.display_highpass.set_steady_state(raw, 0.0)
            self.display_notch.set_steady_state(0.0, 0.0)
            self.display_lowpass.set_steady_state(0.0, 0.0)
            self.qrs_highpass.set_steady_state(raw, 0.0)
            self.qrs_lowpass.set_steady_state(0.0, 0.0)
            self.filters_initialized = True
        display = self.display_highpass.push(raw)
        display = self.display_notch.push(display)
        self.lowpass = self.display_lowpass.push(display)
        qrs = self.qrs_lowpass.push(self.qrs_highpass.push(raw))
        if len(self.history_order) == self.history_order.maxlen:
            self.filtered_history.pop(self.history_order[0], None)
        self.history_order.append(sequence)
        self.filtered_history[sequence] = self.lowpass
        landmark_updates.extend(self._finish_landmarks(sequence))

        derivative = qrs - self.previous
        self.previous = qrs
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
        suppressed = self.index < self.suppress_until
        if not learning and not suppressed and self.pending_energy >= self.previous_energy and self.pending_energy > energy:
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

        quality = 8 if learning else (4 if suppressed else 0)
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
        radius = max(1, round(2 * self.sample_rate / 500))
        values = [self.filtered_history.get(sequence + offset)
                  for offset in range(-radius, radius + 1)]
        if any(value is None for value in values):
            return None
        return sum(values) / len(values)

    def _extremum(self, r_seq, first, last, point, direction, fraction, amplitude):
        left = self._smoothed(r_seq + first)
        right = self._smoothed(r_seq + last)
        margin = max(1, round(3 * self.sample_rate / 500))
        minimum_window = max(4, round(8 * self.sample_rate / 500))
        if left is None or right is None or last - first < minimum_window:
            return None
        floor = max(direction * left, direction * right)
        best, chosen = floor, None
        for offset in range(first + margin, last - margin + 1):
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
        scale = self.sample_rate / 500.0
        p_start = round(max(-120, -int(rr_ms * 0.15)) * scale)
        t_end = round(min(180, int(rr_ms * 0.27)) * scale)
        updates = [(r_seq, 1)]
        p_end = round(-40 * scale)
        q_start, q_end = round(-33 * scale), round(-4 * scale)
        for args in ((p_start, p_end, 2, polarity, .02),
                     (q_start, q_end, 3, -polarity, .015)):
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
        scale = self.sample_rate / 500.0
        s_start, s_end = round(4 * scale), round(40 * scale)
        t_start = round(45 * scale)
        settle = max(1, round(3 * scale))
        if pending_s and sequence - r_seq >= round(43 * scale):
            mark = self._extremum(r_seq, s_start, s_end, 4, -polarity, .015, amplitude)
            if mark is not None:
                updates.append(mark)
            pending[4] = False
        if pending_t and sequence - r_seq >= t_end + settle:
            mark = self._extremum(r_seq, t_start, t_end, 5, polarity, .04, amplitude)
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
