"""WAV file loader supporting 16/24/32-bit PCM and 32-bit IEEE Float with mono downmix.

Uses NumPy when installed for fast decoding, with a full pure-stdlib fallback.
"""

from __future__ import annotations

import array
import math
import struct
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from . import KotoneProfilerError

try:
    import numpy as np  # type: ignore

    HAS_NUMPY = True
except ImportError:  # pragma: no cover - exercised via fallback tests
    np = None  # type: ignore
    HAS_NUMPY = False

_WAVE_FORMAT_PCM = 0x0001
_WAVE_FORMAT_IEEE_FLOAT = 0x0003
_WAVE_FORMAT_EXTENSIBLE = 0xFFFE
_SUBFORMAT_PCM = b"\x01\x00\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"
_SUBFORMAT_IEEE_FLOAT = b"\x03\x00\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"


@dataclass(frozen=True)
class WavAudio:
    """Loaded mono audio signal normalized to [-1.0, 1.0]."""

    sample_rate: int
    num_channels_original: int
    bits_per_sample: int
    samples: Sequence[float]  # numpy.ndarray (float64) or list[float]

    @property
    def num_frames(self) -> int:
        return len(self.samples)

    @property
    def duration_sec(self) -> float:
        return len(self.samples) / float(self.sample_rate) if self.sample_rate > 0 else 0.0


def _decode_pcm_numpy(raw: bytes, num_channels: int, bits_per_sample: int, audio_format: int):
    bytes_per_sample = bits_per_sample // 8
    frame_bytes = num_channels * bytes_per_sample
    num_frames = len(raw) // frame_bytes
    usable = raw[: num_frames * frame_bytes]

    if audio_format == _WAVE_FORMAT_IEEE_FLOAT:
        if bits_per_sample == 32:
            data = np.frombuffer(usable, dtype="<f4").astype(np.float64)
        elif bits_per_sample == 64:
            data = np.frombuffer(usable, dtype="<f8").astype(np.float64)
        else:
            raise KotoneProfilerError(
                "ERR_INVALID_WAV",
                f"Unsupported IEEE Float bit depth: {bits_per_sample}",
                exit_code=1,
            )
        # Sanitize any non-finite samples in float WAVs
        data = np.where(np.isfinite(data), data, 0.0)
    elif audio_format == _WAVE_FORMAT_PCM:
        if bits_per_sample == 16:
            data = np.frombuffer(usable, dtype="<i2").astype(np.float64) / 32768.0
        elif bits_per_sample == 24:
            b = np.frombuffer(usable, dtype=np.uint8).reshape(-1, 3)
            vals = (
                b[:, 0].astype(np.int32)
                | (b[:, 1].astype(np.int32) << 8)
                | (b[:, 2].astype(np.int32) << 16)
            )
            vals = np.where(vals >= 0x800000, vals - 0x1000000, vals)
            data = vals.astype(np.float64) / 8388608.0
        elif bits_per_sample == 32:
            data = np.frombuffer(usable, dtype="<i4").astype(np.float64) / 2147483648.0
        else:
            raise KotoneProfilerError(
                "ERR_INVALID_WAV", f"Unsupported PCM bit depth: {bits_per_sample}", exit_code=1
            )
    else:
        raise KotoneProfilerError(
            "ERR_INVALID_WAV", f"Unsupported WAV format tag: 0x{audio_format:04x}", exit_code=1
        )

    mono = data.reshape(num_frames, num_channels).mean(axis=1) if num_channels > 1 else data
    return np.ascontiguousarray(mono, dtype=np.float64)


def _decode_pcm_stdlib(
    raw: bytes, num_channels: int, bits_per_sample: int, audio_format: int
) -> list[float]:
    bytes_per_sample = bits_per_sample // 8
    frame_bytes = num_channels * bytes_per_sample
    num_frames = len(raw) // frame_bytes
    usable = raw[: num_frames * frame_bytes]

    samples_interleaved: list[float]
    if audio_format == _WAVE_FORMAT_IEEE_FLOAT:
        if bits_per_sample == 32:
            arr = array.array("f")
            arr.frombytes(usable)
            if sys.byteorder != "little":
                arr.byteswap()
            samples_interleaved = [float(x) if math.isfinite(x) else 0.0 for x in arr]
        elif bits_per_sample == 64:
            arr = array.array("d")
            arr.frombytes(usable)
            if sys.byteorder != "little":
                arr.byteswap()
            samples_interleaved = [float(x) if math.isfinite(x) else 0.0 for x in arr]
        else:
            raise KotoneProfilerError(
                "ERR_INVALID_WAV",
                f"Unsupported IEEE Float bit depth: {bits_per_sample}",
                exit_code=1,
            )
    elif audio_format == _WAVE_FORMAT_PCM:
        if bits_per_sample == 16:
            arr = array.array("h")
            arr.frombytes(usable)
            if sys.byteorder != "little":
                arr.byteswap()
            inv = 1.0 / 32768.0
            samples_interleaved = [x * inv for x in arr]
        elif bits_per_sample == 24:
            inv = 1.0 / 8388608.0
            total_samples = num_frames * num_channels
            samples_interleaved = [0.0] * total_samples
            mv = memoryview(usable)
            idx = 0
            for i in range(total_samples):
                val = mv[idx] | (mv[idx + 1] << 8) | (mv[idx + 2] << 16)
                if val >= 0x800000:
                    val -= 0x1000000
                samples_interleaved[i] = val * inv
                idx += 3
        elif bits_per_sample == 32:
            arr = array.array("i")
            arr.frombytes(usable)
            if sys.byteorder != "little":
                arr.byteswap()
            inv = 1.0 / 2147483648.0
            samples_interleaved = [x * inv for x in arr]
        else:
            raise KotoneProfilerError(
                "ERR_INVALID_WAV", f"Unsupported PCM bit depth: {bits_per_sample}", exit_code=1
            )
    else:
        raise KotoneProfilerError(
            "ERR_INVALID_WAV", f"Unsupported WAV format tag: 0x{audio_format:04x}", exit_code=1
        )

    if num_channels == 1:
        return samples_interleaved

    inv_ch = 1.0 / float(num_channels)
    mono = [0.0] * num_frames
    base = 0
    for f in range(num_frames):
        acc = 0.0
        for c in range(num_channels):
            acc += samples_interleaved[base + c]
        mono[f] = acc * inv_ch
        base += num_channels
    return mono


def load_wav_mono(path: str | Path, *, use_numpy: bool = True) -> WavAudio:
    """Load a WAV file (16/24/32-bit PCM or 32-bit IEEE Float) and downmix to mono."""
    file_path = Path(path)
    if not file_path.is_file():
        raise KotoneProfilerError(
            "ERR_FILE_NOT_FOUND", f"WAV file not found: {file_path}", exit_code=1
        )

    try:
        blob = file_path.read_bytes()
    except OSError as exc:
        raise KotoneProfilerError(
            "ERR_INVALID_WAV", f"Unable to read WAV file: {exc}", exit_code=1
        ) from exc

    if len(blob) < 44 or blob[0:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise KotoneProfilerError("ERR_INVALID_WAV", "Invalid RIFF/WAVE header", exit_code=1)

    offset = 12
    fmt_info: tuple[int, int, int, int] | None = None
    data_chunk: bytes | None = None

    while offset + 8 <= len(blob):
        chunk_id = blob[offset : offset + 4]
        chunk_size = struct.unpack_from("<I", blob, offset + 4)[0]
        chunk_start = offset + 8
        chunk_end = chunk_start + chunk_size
        if chunk_end > len(blob):
            # Allow slightly truncated trailing data chunk if at least some data exists
            if chunk_id == b"data" and chunk_start < len(blob):
                chunk_end = len(blob)
            else:
                raise KotoneProfilerError("ERR_INVALID_WAV", "Truncated WAV chunk", exit_code=1)

        if chunk_id == b"fmt ":
            if chunk_size < 16:
                raise KotoneProfilerError("ERR_INVALID_WAV", "Malformed fmt chunk", exit_code=1)
            (
                audio_fmt,
                num_ch,
                sample_rate,
                _byte_rate,
                _block_align,
                bits_per_sample,
            ) = struct.unpack_from("<HHIIHH", blob, chunk_start)
            if audio_fmt == _WAVE_FORMAT_EXTENSIBLE:
                if chunk_size < 40:
                    raise KotoneProfilerError(
                        "ERR_INVALID_WAV",
                        "Malformed WAVE_FORMAT_EXTENSIBLE chunk",
                        exit_code=1,
                    )
                subformat = blob[chunk_start + 24 : chunk_start + 40]
                if subformat == _SUBFORMAT_PCM:
                    audio_fmt = _WAVE_FORMAT_PCM
                elif subformat == _SUBFORMAT_IEEE_FLOAT:
                    audio_fmt = _WAVE_FORMAT_IEEE_FLOAT
                else:
                    raise KotoneProfilerError(
                        "ERR_INVALID_WAV",
                        "Unsupported extensible WAV subformat",
                        exit_code=1,
                    )
            fmt_info = (audio_fmt, num_ch, sample_rate, bits_per_sample)
        elif chunk_id == b"data":
            data_chunk = blob[chunk_start:chunk_end]

        # RIFF chunks are word-aligned (pad byte if chunk_size is odd)
        offset = chunk_end + (chunk_size & 1)

    if fmt_info is None or data_chunk is None:
        raise KotoneProfilerError(
            "ERR_INVALID_WAV", "Missing required fmt or data chunk", exit_code=1
        )

    audio_fmt, num_ch, sample_rate, bits_per_sample = fmt_info
    if num_ch < 1 or num_ch > 8:
        raise KotoneProfilerError(
            "ERR_INVALID_WAV", f"Unsupported channel count: {num_ch}", exit_code=1
        )
    if sample_rate < 8000 or sample_rate > 192000:
        raise KotoneProfilerError(
            "ERR_INVALID_WAV", f"Unsupported sample rate: {sample_rate}", exit_code=1
        )
    if bits_per_sample not in (16, 24, 32, 64):
        raise KotoneProfilerError(
            "ERR_INVALID_WAV", f"Unsupported bit depth: {bits_per_sample}", exit_code=1
        )

    frame_bytes = num_ch * (bits_per_sample // 8)
    if len(data_chunk) < frame_bytes:
        raise KotoneProfilerError(
            "ERR_INVALID_WAV", "WAV data chunk contains zero frames", exit_code=1
        )

    if use_numpy and HAS_NUMPY:
        samples = _decode_pcm_numpy(data_chunk, num_ch, bits_per_sample, audio_fmt)
    else:
        samples = _decode_pcm_stdlib(data_chunk, num_ch, bits_per_sample, audio_fmt)

    return WavAudio(
        sample_rate=sample_rate,
        num_channels_original=num_ch,
        bits_per_sample=bits_per_sample,
        samples=samples,
    )
