"""Acoustic voice analyzer (VAD, noise floor, F0 autocorrelation, LTAS, dynamics, sibilance).

Uses NumPy when available, with an automatic pure-stdlib fallback.
"""

from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass, field

from . import KotoneProfilerError
from .wav_io import HAS_NUMPY, WavAudio

if HAS_NUMPY:
    import numpy as np  # type: ignore

EQ_CENTER_FREQS_HZ: tuple[float, ...] = (
    120.0,
    250.0,
    500.0,
    1500.0,
    3200.0,
    5000.0,
    7200.0,
    11000.0,
)

MIN_VOICED_DURATION_SEC = 5.0
FFT_SIZE = 4096


@dataclass(frozen=True)
class AnalysisResult:
    """Complete acoustic measurements extracted from a voice recording."""

    sample_rate_hz: int
    total_duration_sec: float
    voiced_duration_sec: float
    noise_floor_dbfs: float
    mean_rms_dbfs: float
    peak_dbfs: float
    crest_factor_db: float
    voiced_rms_p90_dbfs: float
    fundamental_freq_median_hz: float
    sibilance_ratio: float
    sibilance_peak_freq_hz: float
    sibilance_peak_dbfs: float
    band_rel_db: dict[float, float] = field(default_factory=dict)


def _linear_to_db(val: float, floor_db: float = -120.0) -> float:
    if not math.isfinite(val) or val <= 1e-12:
        return floor_db
    return max(floor_db, 20.0 * math.log10(val))


def _power_to_db(power: float, floor_db: float = -120.0) -> float:
    if not math.isfinite(power) or power <= 1e-24:
        return floor_db
    return max(floor_db, 10.0 * math.log10(power))


def _percentile(sorted_vals: Sequence[float], q: float) -> float:
    if not sorted_vals:
        return 0.0
    if len(sorted_vals) == 1:
        return float(sorted_vals[0])
    pos = max(0.0, min(1.0, q)) * (len(sorted_vals) - 1)
    lo = int(math.floor(pos))
    hi = min(len(sorted_vals) - 1, lo + 1)
    frac = pos - lo
    return float(sorted_vals[lo]) * (1.0 - frac) + float(sorted_vals[hi]) * frac


def _fft_radix2(x: list[complex]) -> list[complex]:
    """Iterative Cooley-Tukey radix-2 FFT in pure Python (len(x) must be a power of 2)."""
    n = len(x)
    a = list(x)

    # Bit-reversal permutation
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j ^= bit
        if i < j:
            a[i], a[j] = a[j], a[i]

    length = 2
    while length <= n:
        ang = -2.0 * math.pi / length
        wlen = complex(math.cos(ang), math.sin(ang))
        half = length >> 1
        for i in range(0, n, length):
            w = 1.0 + 0.0j
            for k in range(half):
                u = a[i + k]
                v = a[i + k + half] * w
                a[i + k] = u + v
                a[i + k + half] = u - v
                w *= wlen
        length <<= 1
    return a


def _rfft_power_stdlib(frame: Sequence[float], window: Sequence[float]) -> list[float]:
    n = len(window)
    buf = [complex(frame[i] * window[i], 0.0) for i in range(n)]
    spec = _fft_radix2(buf)
    half = (n // 2) + 1
    return [spec[k].real * spec[k].real + spec[k].imag * spec[k].imag for k in range(half)]


def _estimate_f0_frame_numpy(seg, sr: int, lag_min: int, lag_max: int) -> float | None:
    x = seg - float(np.mean(seg))
    energy = float(np.dot(x, x))
    if energy <= 1e-12:
        return None
    n = len(x)
    nfft = 1
    while nfft < 2 * n:
        nfft <<= 1
    spec = np.fft.rfft(x, n=nfft)
    acf = np.fft.irfft(spec * np.conj(spec), n=nfft)[: lag_max + 2]
    acf /= energy

    region = acf[lag_min : lag_max + 1]
    if len(region) < 3:
        return None
    max_val = float(np.max(region))
    if max_val < 0.35:
        return None

    # Pick first strong local peak (>= 85% of global max) to prevent octave-down errors
    threshold = max(0.35, 0.85 * max_val)
    best_lag = -1
    for i in range(1, len(region) - 1):
        val = float(region[i])
        if val >= threshold and val >= float(region[i - 1]) and val >= float(region[i + 1]):
            best_lag = lag_min + i
            break
    if best_lag < 0:
        best_lag = lag_min + int(np.argmax(region))

    # Parabolic interpolation around best_lag
    if 1 <= best_lag < len(acf) - 1:
        y_l = float(acf[best_lag - 1])
        y_c = float(acf[best_lag])
        y_r = float(acf[best_lag + 1])
        denom = y_l - 2.0 * y_c + y_r
        if abs(denom) > 1e-12:
            delta = 0.5 * (y_l - y_r) / denom
            delta = max(-0.5, min(0.5, delta))
            return float(sr) / (float(best_lag) + delta)
    return float(sr) / float(best_lag)


def _estimate_f0_frame_stdlib(
    seg: Sequence[float], sr: int, lag_min: int, lag_max: int
) -> float | None:
    n = len(seg)
    mean = sum(seg) / float(n)
    x = [s - mean for s in seg]
    energy = sum(v * v for v in x)
    if energy <= 1e-12:
        return None

    inv_e = 1.0 / energy
    acf: list[float] = [0.0] * (lag_max + 2)
    max_val = -1.0
    max_lag = lag_min
    for lag in range(lag_min, lag_max + 1):
        s = 0.0
        limit = n - lag
        for i in range(limit):
            s += x[i] * x[i + lag]
        r = s * inv_e
        acf[lag] = r
        if r > max_val:
            max_val = r
            max_lag = lag

    if max_val < 0.35:
        return None

    threshold = max(0.35, 0.85 * max_val)
    best_lag = max_lag
    for lag in range(lag_min + 1, lag_max):
        val = acf[lag]
        if val >= threshold and val >= acf[lag - 1] and val >= acf[lag + 1]:
            best_lag = lag
            break

    if lag_min < best_lag < lag_max:
        y_l = acf[best_lag - 1]
        y_c = acf[best_lag]
        y_r = acf[best_lag + 1]
        denom = y_l - 2.0 * y_c + y_r
        if abs(denom) > 1e-12:
            delta = max(-0.5, min(0.5, 0.5 * (y_l - y_r) / denom))
            return float(sr) / (float(best_lag) + delta)
    return float(sr) / float(best_lag)


def analyze_audio(audio: WavAudio, *, use_numpy: bool = True) -> AnalysisResult:
    """Run VAD, noise floor, F0, LTAS, dynamics, and sibilance analysis on mono WavAudio."""
    sr = audio.sample_rate
    n_samples = audio.num_frames
    if sr < 8000 or n_samples == 0:
        raise KotoneProfilerError("ERR_INVALID_WAV", "Empty or invalid audio buffer", exit_code=1)

    frame_len = max(16, int(round(sr * 0.020)))  # 20 ms
    hop_len = max(8, int(round(sr * 0.010)))  # 10 ms
    if n_samples < frame_len:
        raise KotoneProfilerError(
            "ERR_INSUFFICIENT_SPEECH",
            f"Audio too short ({audio.duration_sec:.2f}s < "
            f"{MIN_VOICED_DURATION_SEC:.1f}s required)",
            exit_code=2,
        )

    num_frames = 1 + (n_samples - frame_len) // hop_len
    hop_sec = hop_len / float(sr)
    use_np = bool(use_numpy and HAS_NUMPY)

    if use_np:
        sig = np.asarray(audio.samples, dtype=np.float64)
        starts = np.arange(num_frames) * hop_len
        idx = starts[:, None] + np.arange(frame_len)[None, :]
        frames = sig[idx]
        rms_vals = np.sqrt(np.mean(frames * frames, axis=1))
        frame_rms_db = 20.0 * np.log10(np.maximum(rms_vals, 1e-12))
        frame_zcr = np.mean((frames[:, :-1] * frames[:, 1:]) < 0.0, axis=1)

        noise_floor_dbfs = float(np.percentile(frame_rms_db, 10))
        noise_floor_dbfs = max(-120.0, min(0.0, noise_floor_dbfs))
        vad_thresh_db = max(noise_floor_dbfs + 10.0, -50.0)
        voiced_mask = frame_rms_db >= vad_thresh_db
        voiced_indices = np.flatnonzero(voiced_mask)
        voiced_duration_sec = float(len(voiced_indices)) * hop_sec

        if voiced_duration_sec < MIN_VOICED_DURATION_SEC:
            raise KotoneProfilerError(
                "ERR_INSUFFICIENT_SPEECH",
                f"Voiced speech duration {voiced_duration_sec:.2f}s "
                f"is below minimum {MIN_VOICED_DURATION_SEC:.1f}s",
                exit_code=2,
            )

        # Dynamics over voiced frames
        voiced_frames = frames[voiced_indices]
        mean_rms_lin = float(np.sqrt(np.mean(voiced_frames * voiced_frames)))
        peak_lin = float(np.max(np.abs(voiced_frames)))
        mean_rms_dbfs = _linear_to_db(mean_rms_lin)
        peak_dbfs = _linear_to_db(peak_lin)
        crest_factor_db = max(0.0, peak_dbfs - mean_rms_dbfs)
        voiced_rms_p90_dbfs = float(np.percentile(frame_rms_db[voiced_indices], 90))

        # F0 median via autocorrelation on voiced tonal frames (zcr < 0.25)
        f0_win = max(frame_len, int(round(sr * 0.040)))
        lag_min = max(2, int(sr / 350.0))
        lag_max = min(f0_win // 2, int(sr / 70.0))
        f0_candidates: list[float] = []
        tonal_indices = voiced_indices[frame_zcr[voiced_indices] < 0.25]
        if len(tonal_indices) == 0:
            tonal_indices = voiced_indices
        step = max(1, len(tonal_indices) // 160)
        for fi in tonal_indices[::step]:
            start = int(fi) * hop_len
            end = start + f0_win
            if end > n_samples:
                continue
            f0 = _estimate_f0_frame_numpy(sig[start:end], sr, lag_min, lag_max)
            if f0 is not None and 60.0 <= f0 <= 400.0:
                f0_candidates.append(f0)
        f0_candidates.sort()
        f0_median = _percentile(f0_candidates, 0.5) if f0_candidates else 150.0

        # Build concatenated voiced sample buffer for 4096-pt Hann LTAS
        sample_mask = np.zeros(n_samples, dtype=bool)
        for fi in voiced_indices:
            s0 = int(fi) * hop_len
            sample_mask[s0 : min(n_samples, s0 + frame_len)] = True
        voiced_samples = sig[sample_mask]

        win_hann = 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(FFT_SIZE) / float(FFT_SIZE))
        win_norm = float(np.sum(win_hann * win_hann))
        fft_hop = FFT_SIZE // 2
        if len(voiced_samples) < FFT_SIZE:
            padded = np.zeros(FFT_SIZE, dtype=np.float64)
            padded[: len(voiced_samples)] = voiced_samples
            fft_frames = [padded]
        else:
            n_fft_frames = 1 + (len(voiced_samples) - FFT_SIZE) // fft_hop
            fft_frames = [
                voiced_samples[k * fft_hop : k * fft_hop + FFT_SIZE] for k in range(n_fft_frames)
            ]

        ltas_power = np.zeros(FFT_SIZE // 2 + 1, dtype=np.float64)
        freqs = np.arange(FFT_SIZE // 2 + 1, dtype=np.float64) * (float(sr) / float(FFT_SIZE))
        sib_mask = (freqs >= 5000.0) & (freqs <= min(9000.0, sr * 0.48))
        sib_frame_dbs: list[float] = []

        for seg in fft_frames:
            spec = np.fft.rfft(seg * win_hann)
            pwr = (spec.real * spec.real + spec.imag * spec.imag) / (win_norm * FFT_SIZE)
            ltas_power += pwr
            if np.any(sib_mask):
                # Multiply by 2 for single-sided spectrum RMS equivalence
                sib_rms = math.sqrt(max(0.0, 2.0 * float(np.sum(pwr[sib_mask]))))
                sib_frame_dbs.append(_linear_to_db(sib_rms))

        ltas_power /= float(len(fft_frames))
        ltas_list = ltas_power.tolist()
    else:
        # Pure Python standard-library fallback path
        sig_list = list(audio.samples)
        frame_rms_db_list: list[float] = []
        frame_zcr_list: list[float] = []
        frame_sq_means: list[float] = []
        frame_peaks: list[float] = []

        inv_fl = 1.0 / float(frame_len)
        inv_zm = 1.0 / float(max(1, frame_len - 1))
        for fi in range(num_frames):
            s0 = fi * hop_len
            s1 = s0 + frame_len
            sum_sq = 0.0
            pk = 0.0
            zc = 0
            prev = sig_list[s0]
            sum_sq += prev * prev
            pk = abs(prev)
            for idx_s in range(s0 + 1, s1):
                cur = sig_list[idx_s]
                sum_sq += cur * cur
                ac = abs(cur)
                if ac > pk:
                    pk = ac
                if (prev < 0.0 and cur > 0.0) or (prev > 0.0 and cur < 0.0):
                    zc += 1
                prev = cur
            mean_sq = sum_sq * inv_fl
            frame_sq_means.append(mean_sq)
            frame_peaks.append(pk)
            frame_rms_db_list.append(_linear_to_db(math.sqrt(mean_sq)))
            frame_zcr_list.append(zc * inv_zm)

        sorted_rms_db = sorted(frame_rms_db_list)
        noise_floor_dbfs = max(-120.0, min(0.0, _percentile(sorted_rms_db, 0.10)))
        vad_thresh_db = max(noise_floor_dbfs + 10.0, -50.0)
        voiced_indices_list = [i for i, db in enumerate(frame_rms_db_list) if db >= vad_thresh_db]
        voiced_duration_sec = float(len(voiced_indices_list)) * hop_sec

        if voiced_duration_sec < MIN_VOICED_DURATION_SEC:
            raise KotoneProfilerError(
                "ERR_INSUFFICIENT_SPEECH",
                f"Voiced speech duration {voiced_duration_sec:.2f}s "
                f"is below minimum {MIN_VOICED_DURATION_SEC:.1f}s",
                exit_code=2,
            )

        mean_rms_lin = math.sqrt(
            sum(frame_sq_means[i] for i in voiced_indices_list) / float(len(voiced_indices_list))
        )
        peak_lin = max(frame_peaks[i] for i in voiced_indices_list)
        mean_rms_dbfs = _linear_to_db(mean_rms_lin)
        peak_dbfs = _linear_to_db(peak_lin)
        crest_factor_db = max(0.0, peak_dbfs - mean_rms_dbfs)
        voiced_rms_sorted = sorted(frame_rms_db_list[i] for i in voiced_indices_list)
        voiced_rms_p90_dbfs = _percentile(voiced_rms_sorted, 0.90)

        # F0 median via pure-Python autocorrelation
        f0_win = max(frame_len, int(round(sr * 0.040)))
        lag_min = max(2, int(sr / 350.0))
        lag_max = min(f0_win // 2, int(sr / 70.0))
        tonal_list = [i for i in voiced_indices_list if frame_zcr_list[i] < 0.25]
        if not tonal_list:
            tonal_list = voiced_indices_list
        step = max(1, len(tonal_list) // 48)
        f0_candidates = []
        for fi in tonal_list[::step]:
            s0 = fi * hop_len
            s1 = s0 + f0_win
            if s1 > n_samples:
                continue
            f0 = _estimate_f0_frame_stdlib(sig_list[s0:s1], sr, lag_min, lag_max)
            if f0 is not None and 60.0 <= f0 <= 400.0:
                f0_candidates.append(f0)
        f0_candidates.sort()
        f0_median = _percentile(f0_candidates, 0.5) if f0_candidates else 150.0

        # Gather voiced samples for 4096-point Hann LTAS
        sample_mask_list = bytearray(n_samples)
        for fi in voiced_indices_list:
            s0 = fi * hop_len
            s1 = min(n_samples, s0 + frame_len)
            sample_mask_list[s0:s1] = b"\x01" * (s1 - s0)
        voiced_samples_list = [sig_list[i] for i in range(n_samples) if sample_mask_list[i]]

        win_hann_list = [
            0.5 - 0.5 * math.cos(2.0 * math.pi * i / float(FFT_SIZE)) for i in range(FFT_SIZE)
        ]
        win_norm = sum(w * w for w in win_hann_list)
        fft_hop = FFT_SIZE // 2

        if len(voiced_samples_list) < FFT_SIZE:
            padded_list = voiced_samples_list + [0.0] * (FFT_SIZE - len(voiced_samples_list))
            fft_segments = [padded_list]
        else:
            n_fft_frames = 1 + (len(voiced_samples_list) - FFT_SIZE) // fft_hop
            fft_step = max(1, n_fft_frames // 48)
            fft_segments = [
                voiced_samples_list[k * fft_hop : k * fft_hop + FFT_SIZE]
                for k in range(0, n_fft_frames, fft_step)
            ]

        n_bins = FFT_SIZE // 2 + 1
        ltas_list = [0.0] * n_bins
        bin_hz = float(sr) / float(FFT_SIZE)
        sib_lo_bin = max(1, int(math.ceil(5000.0 / bin_hz)))
        sib_hi_bin = min(n_bins - 1, int(math.floor(min(9000.0, sr * 0.48) / bin_hz)))
        sib_frame_dbs = []
        scale = 1.0 / (win_norm * float(FFT_SIZE))

        for seg in fft_segments:
            pwr_raw = _rfft_power_stdlib(seg, win_hann_list)
            sib_sum = 0.0
            for k in range(n_bins):
                p = pwr_raw[k] * scale
                ltas_list[k] += p
                if sib_lo_bin <= k <= sib_hi_bin:
                    sib_sum += p
            sib_frame_dbs.append(_linear_to_db(math.sqrt(max(0.0, 2.0 * sib_sum))))

        inv_frames = 1.0 / float(len(fft_segments))
        ltas_list = [p * inv_frames for p in ltas_list]

    # Compute 1/3-octave band energies and sibilance metrics from LTAS
    bin_hz = float(sr) / float(FFT_SIZE)
    n_bins = len(ltas_list)
    nyquist_safe = sr * 0.48

    def band_energy(f_lo: float, f_hi: float) -> float:
        lo = max(1, int(math.floor(f_lo / bin_hz)))
        hi = min(n_bins - 1, int(math.ceil(min(f_hi, nyquist_safe) / bin_hz)))
        if hi < lo:
            return 0.0
        return sum(ltas_list[lo : hi + 1])

    third_oct_factor = 2.0 ** (1.0 / 6.0)
    raw_band_db: dict[float, float] = {}
    for fc in EQ_CENTER_FREQS_HZ:
        if fc >= nyquist_safe:
            raw_band_db[fc] = -120.0
        else:
            e = band_energy(fc / third_oct_factor, fc * third_oct_factor)
            raw_band_db[fc] = _power_to_db(e)

    # Reference level: average of 500 Hz and 1500 Hz mid-voice anchor bands
    ref_db = 0.5 * (raw_band_db[500.0] + raw_band_db[1500.0])
    if ref_db <= -100.0:
        valid_vals = [v for v in raw_band_db.values() if v > -100.0]
        ref_db = sum(valid_vals) / float(len(valid_vals)) if valid_vals else -60.0

    band_rel_db = {fc: raw_band_db[fc] - ref_db for fc in EQ_CENTER_FREQS_HZ}

    # Sibilance metrics (5 kHz - 9 kHz vs 80 Hz - 12 kHz total speech energy)
    sib_energy = band_energy(5000.0, 9000.0)
    total_energy = band_energy(80.0, 12000.0)
    sibilance_ratio = max(0.0, min(1.0, sib_energy / max(total_energy, 1e-18)))

    # Sibilance peak frequency in 4.5 kHz - 9.5 kHz
    sib_search_lo = max(1, int(math.floor(4500.0 / bin_hz)))
    sib_search_hi = min(n_bins - 1, int(math.ceil(min(9500.0, nyquist_safe) / bin_hz)))
    if sib_search_hi >= sib_search_lo:
        peak_bin = max(range(sib_search_lo, sib_search_hi + 1), key=lambda k: ltas_list[k])
        sibilance_peak_freq_hz = peak_bin * bin_hz
    else:
        sibilance_peak_freq_hz = 6500.0
    sibilance_peak_freq_hz = max(3000.0, min(12000.0, sibilance_peak_freq_hz))

    sib_frame_dbs.sort()
    sibilance_peak_dbfs = _percentile(sib_frame_dbs, 0.95) if sib_frame_dbs else -60.0

    return AnalysisResult(
        sample_rate_hz=sr,
        total_duration_sec=audio.duration_sec,
        voiced_duration_sec=voiced_duration_sec,
        noise_floor_dbfs=noise_floor_dbfs,
        mean_rms_dbfs=mean_rms_dbfs,
        peak_dbfs=peak_dbfs,
        crest_factor_db=crest_factor_db,
        voiced_rms_p90_dbfs=voiced_rms_p90_dbfs,
        fundamental_freq_median_hz=f0_median,
        sibilance_ratio=sibilance_ratio,
        sibilance_peak_freq_hz=sibilance_peak_freq_hz,
        sibilance_peak_dbfs=sibilance_peak_dbfs,
        band_rel_db=band_rel_db,
    )
