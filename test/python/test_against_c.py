"""The Python implementation against the C core, through the ul_tool program.

Set UL_TOOL to the ul_tool executable, or build it in build/ first.
"""
from __future__ import annotations

import os
import random
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import uart_link as ul  # noqa: E402


def find_tool() -> str:
    if os.environ.get("UL_TOOL"):
        return os.environ["UL_TOOL"]
    for name in ("ul_tool", "ul_tool.exe"):
        candidate = ROOT / "build" / name
        if candidate.exists():
            return str(candidate)
    found = shutil.which("ul_tool")
    if found:
        return found
    # In CI a missing tool must fail the job, not skip it into a green result.
    (pytest.fail if os.environ.get("CI") else pytest.skip)("ul_tool not built; set UL_TOOL")


@pytest.fixture(scope="module")
def tool() -> str:
    return find_tool()


def run(tool: str, *args: str) -> list[str]:
    return subprocess.run([tool, *args], capture_output=True, text=True, check=True).stdout.split("\n")


def random_frames(seed: int, count: int) -> list[ul.Frame]:
    rng = random.Random(seed)
    frames = []
    for _ in range(count):
        length = rng.choice((0, 1, 2, rng.randrange(0, ul.MAX_PAYLOAD + 1), ul.MAX_PAYLOAD))
        payload = bytes(rng.choice((0, rng.randrange(256))) for _ in range(length))
        frames.append(ul.Frame(rng.choice((1, 2, 3, 7)), rng.randrange(1 << 16), rng.randrange(256), payload))
    return frames


def test_c_encodes_like_python(tool: str) -> None:
    for frame in random_frames(11, 150):
        out = run(tool, "encode", str(frame.kind), str(frame.session), str(frame.seq), frame.payload.hex() or "-")
        assert bytes.fromhex(out[0]) == ul.encode_frame(frame)


def test_c_decodes_python_frames(tool: str) -> None:
    frames = random_frames(12, 30)
    wire = b"".join(ul.encode_frame(f) for f in frames)
    lines = run(tool, "decode", wire.hex())
    decoded = [line.split() for line in lines if line.startswith("frame")]
    assert len(decoded) == len(frames)
    for (_, kind, session, seq, payload), frame in zip(decoded, frames):
        assert (int(kind), int(session), int(seq)) == (frame.kind, frame.session, frame.seq)
        assert (b"" if payload == "-" else bytes.fromhex(payload)) == frame.payload


def test_both_count_damaged_input_alike(tool: str) -> None:
    rng = random.Random(13)
    for _ in range(40):
        wire = bytearray(b"".join(ul.encode_frame(f) for f in random_frames(rng.randrange(1 << 30), 4)))
        for _ in range(rng.randrange(1, 6)):
            wire[rng.randrange(len(wire))] = rng.choice((0, rng.randrange(256)))
        wire += bytes(rng.randrange(256) for _ in range(rng.randrange(0, 40))) + b"\x00"

        lines = run(tool, "decode", wire.hex())
        c_frames = [line for line in lines if line.startswith("frame")]
        c_stats = [int(x) for x in next(line for line in lines if line.startswith("stats")).split()[1:]]

        decoder = ul.Decoder()
        py_frames = decoder.feed(bytes(wire))
        s = decoder.stats
        assert c_stats == [s.frames, s.crc_errors, s.format_errors, s.overruns]
        assert len(c_frames) == len(py_frames)
