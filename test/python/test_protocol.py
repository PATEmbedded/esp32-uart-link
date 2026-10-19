"""The Python implementation in tools/uart_link.py on its own."""
from __future__ import annotations

import random
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))

import uart_link as ul  # noqa: E402

# The examples from the COBS article on Wikipedia.
COBS_VECTORS = [
    (b"", b"\x01"),
    (b"\x00", b"\x01\x01"),
    (b"\x00\x00", b"\x01\x01\x01"),
    (b"\x00\x11\x00", b"\x01\x02\x11\x01"),
    (b"\x11\x22\x00\x33", b"\x03\x11\x22\x02\x33"),
    (b"\x11\x22\x33\x44", b"\x05\x11\x22\x33\x44"),
    (b"\x11\x00\x00\x00", b"\x02\x11\x01\x01\x01"),
    (bytes(range(1, 255)), b"\xff" + bytes(range(1, 255))),
    (bytes(range(0, 255)), b"\x01\xff" + bytes(range(1, 255))),
    (bytes(range(1, 256)), b"\xff" + bytes(range(1, 255)) + b"\x02\xff"),
    (bytes(range(2, 256)) + b"\x00", b"\xff" + bytes(range(2, 256)) + b"\x01\x01"),
    (bytes(range(3, 256)) + b"\x00\x01", b"\xfe" + bytes(range(3, 256)) + b"\x02\x01"),
]


@pytest.mark.parametrize("raw, encoded", COBS_VECTORS)
def test_cobs_vectors(raw: bytes, encoded: bytes) -> None:
    assert ul.cobs_encode(raw) == encoded
    assert ul.cobs_decode(encoded) == raw


def test_cobs_round_trip() -> None:
    rng = random.Random(1)
    for _ in range(500):
        raw = bytes(rng.choice((0, rng.randrange(1, 256))) for _ in range(rng.randrange(0, 700)))
        encoded = ul.cobs_encode(raw)
        assert 0 not in encoded
        assert len(encoded) <= len(raw) + len(raw) // 254 + 1
        assert ul.cobs_decode(encoded) == raw


@pytest.mark.parametrize("bad", [b"\x00", b"\x03\x11", b"\x03\x11\x00"])
def test_cobs_rejects_invalid_input(bad: bytes) -> None:
    with pytest.raises(ValueError):
        ul.cobs_decode(bad)


def test_crc_check_value() -> None:
    assert ul.crc16(b"123456789") == 0x29B1


def test_wire_layout() -> None:
    # The same hand-written frame as test_wire_layout in test/host/test_frame.c.
    assert ul.encode_frame(ul.Frame(ul.KIND_ACK, 0x1234, 0x56)) == bytes.fromhex("0702341256ef2f00")


def test_decoder_resynchronises_and_counts() -> None:
    frame = ul.encode_frame(ul.Frame(ul.KIND_DATAGRAM, 9, 0, b"\xaa\x00\xbb"))
    decoder = ul.Decoder()
    found = decoder.feed(b"ABC\xff\x13\x00\x99\x98" + frame + frame)
    assert [f.payload for f in found] == [b"\xaa\x00\xbb"]
    assert decoder.stats.crc_errors + decoder.stats.format_errors == 2


def test_decoder_drops_frames_that_are_too_long() -> None:
    decoder = ul.Decoder()
    assert decoder.feed(b"\x55" * (ul.MAX_ENCODED + 50) + b"\x00") == []
    assert decoder.stats.overruns == 1
    assert decoder.feed(b"\x01" * ul.MAX_ENCODED + b"\x00") == []  # valid COBS, decodes too long
    assert decoder.stats.format_errors == 1


class Wire:
    """Frames written in one step arrive in the next; some are lost or damaged."""

    def __init__(self, rng: random.Random, drop: float = 0.0, corrupt: float = 0.0) -> None:
        self.rng, self.drop, self.corrupt = rng, drop, corrupt
        self.pending = bytearray()

    def write(self, data: bytes) -> None:
        if self.rng.random() < self.drop:
            return
        data = bytearray(data)
        if self.rng.random() < self.corrupt:
            data[self.rng.randrange(len(data))] ^= 1 << self.rng.randrange(8)
        self.pending += data

    def take(self) -> bytes:
        data, self.pending = bytes(self.pending), bytearray()
        return data


def test_link_delivers_exactly_once_over_a_lossy_line() -> None:
    rng = random.Random(7)
    ab, ba = Wire(rng, 0.2, 0.05), Wire(rng, 0.2, 0.05)
    got: list[bytes] = []
    results: list[bool] = []
    a = ul.Link(ab.write, lambda p, r: True, results.append, session=0xA001, ack_timeout=0.02, max_retries=100)
    b = ul.Link(ba.write, lambda p, r: got.append(p), session=0xB002, ack_timeout=0.02)

    messages = [bytes(rng.randrange(256) for _ in range(rng.randrange(0, 49))) for _ in range(200)]
    now, next_index = 0.0, 0
    while (next_index < len(messages) or a.busy) and now < 600.0:
        if next_index < len(messages) and not a.busy:
            a.send(messages[next_index], now)
            next_index += 1
        now += 0.001
        b.receive(ab.take(), now)
        a.receive(ba.take(), now)

    assert got == messages
    assert results == [True] * len(messages)
    assert a.stats.retransmits > 0
    assert b.stats.duplicates > 0


def test_link_gives_up_and_numbers_the_next_message() -> None:
    sent: list[bytes] = []
    results: list[bool] = []
    a = ul.Link(sent.append, lambda p, r: True, results.append, session=1, ack_timeout=0.02, max_retries=2)
    a.send(b"lost", 0.0)
    for t in (0.02, 0.04, 0.06):
        a.poll(t)
    assert results == [False]
    assert len(sent) == 3 and a.stats.retransmits == 2 and a.stats.failed == 1
    a.send(b"next", 0.1)
    frame = ul.Decoder().feed(sent[-1])[0]
    assert frame.seq == 1


def test_link_refusal_makes_the_sender_repeat() -> None:
    ab, ba = Wire(random.Random(3)), Wire(random.Random(4))
    answers = iter([False, True])
    a = ul.Link(ab.write, lambda p, r: True, session=1, ack_timeout=0.02)
    b = ul.Link(ba.write, lambda p, r: next(answers), session=2)
    a.send(b"full", 0.0)
    now = 0.0
    while a.busy and now < 1.0:
        now += 0.001
        b.receive(ab.take(), now)
        a.receive(ba.take(), now)
    assert a.stats.sent == 1 and a.stats.retransmits == 1
    assert b.stats.refused == 1 and b.stats.received == 1


def test_link_rejects_long_payloads_and_a_second_send() -> None:
    a = ul.Link(lambda d: None, lambda p, r: True, session=1)
    with pytest.raises(ValueError):
        a.send(bytes(ul.MAX_PAYLOAD + 1), 0.0)
    a.send(b"x", 0.0)
    with pytest.raises(ul.BusyError):
        a.send(b"y", 0.0)
    assert a.time_to_deadline(0.03) == pytest.approx(0.07)
