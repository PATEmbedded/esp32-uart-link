#!/usr/bin/env python3
"""The uart_link protocol for the PC side, and a small command-line tool.

The framing, CRC and link rules match the C core in core/ byte for byte (test/python checks
that). The module needs only the standard library; the command-line tool also needs pyserial.

    python uart_link.py --port COM5 listen
    python uart_link.py --port /dev/ttyUSB0 send "hello" --count 10
"""
from __future__ import annotations

import argparse
import binascii
import os
import struct
import sys
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

MAX_PAYLOAD = 256
HEADER = struct.Struct("<BHB")  # kind, session, seq
CRC = struct.Struct("<H")
MAX_FRAME = HEADER.size + MAX_PAYLOAD + CRC.size
MAX_ENCODED = MAX_FRAME + MAX_FRAME // 254 + 1

KIND_DATA = 1
KIND_ACK = 2
KIND_DATAGRAM = 3


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE, as ul_crc16() in C."""
    return binascii.crc_hqx(data, 0xFFFF)


def cobs_encode(data: bytes) -> bytes:
    out = bytearray([0])
    code_pos = 0
    code = 1
    for i, byte in enumerate(data):
        if byte:
            out.append(byte)
            code += 1
        if byte == 0 or code == 0xFF:
            out[code_pos] = code
            code = 1
            if byte and i + 1 == len(data):
                return bytes(out)  # ends with a full block: no final code byte
            code_pos = len(out)
            out.append(0)
    out[code_pos] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        if code == 0:
            raise ValueError("zero byte in COBS data")
        block = data[i:i + code - 1]
        if len(block) != code - 1 or 0 in block:
            raise ValueError("COBS block runs past the end or holds a zero")
        out += block
        i += code - 1
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


@dataclass(frozen=True)
class Frame:
    kind: int
    session: int
    seq: int
    payload: bytes = b""


def encode_frame(frame: Frame) -> bytes:
    """The frame as it goes on the wire, with the zero delimiter."""
    if len(frame.payload) > MAX_PAYLOAD:
        raise ValueError(f"payload longer than {MAX_PAYLOAD} bytes")
    raw = HEADER.pack(frame.kind, frame.session, frame.seq) + frame.payload
    raw += CRC.pack(crc16(raw))
    return cobs_encode(raw) + b"\x00"


@dataclass
class DecoderStats:
    frames: int = 0
    crc_errors: int = 0
    format_errors: int = 0
    overruns: int = 0


class Decoder:
    """Finds frames in a byte stream that may start or break anywhere, like ul_decoder_t."""

    def __init__(self) -> None:
        self._buf = bytearray()
        self._overrun = False
        self.stats = DecoderStats()

    def feed(self, data: bytes) -> list[Frame]:
        frames = []
        for byte in data:
            if byte == 0:
                if self._overrun:
                    self.stats.overruns += 1
                elif self._buf:
                    frame = self._finish()
                    if frame is not None:
                        frames.append(frame)
                self._buf.clear()
                self._overrun = False
            elif not self._overrun:
                if len(self._buf) == MAX_ENCODED:
                    self._overrun = True
                else:
                    self._buf.append(byte)
        return frames

    def _finish(self) -> Optional[Frame]:
        try:
            raw = cobs_decode(bytes(self._buf))
        except ValueError:
            self.stats.format_errors += 1
            return None
        if not HEADER.size + CRC.size <= len(raw) <= MAX_FRAME:
            self.stats.format_errors += 1
            return None
        body, (crc,) = raw[:-CRC.size], CRC.unpack(raw[-CRC.size:])
        if crc16(body) != crc:
            self.stats.crc_errors += 1
            return None
        self.stats.frames += 1
        kind, session, seq = HEADER.unpack(body[:HEADER.size])
        return Frame(kind, session, seq, body[HEADER.size:])


class BusyError(RuntimeError):
    """A reliable message is still in flight."""


@dataclass
class LinkStats:
    sent: int = 0
    failed: int = 0
    retransmits: int = 0
    received: int = 0
    refused: int = 0
    duplicates: int = 0
    ignored: int = 0
    wire: DecoderStats = field(default_factory=DecoderStats)


class Link:
    """The reliable link without I/O, like ul_link_t. Times are in seconds.

    on_message(payload, reliable) may return False to refuse a reliable message for now; it is then
    not acknowledged and the sender repeats it. on_sent(delivered) reports each reliable message.
    """

    def __init__(self, write: Callable[[bytes], None],
                 on_message: Callable[[bytes, bool], Optional[bool]],
                 on_sent: Optional[Callable[[bool], None]] = None,
                 session: Optional[int] = None, ack_timeout: float = 0.1, max_retries: int = 5) -> None:
        self._write = write
        self._on_message = on_message
        self._on_sent = on_sent
        self.session = int.from_bytes(os.urandom(2), "little") if session is None else session
        self.ack_timeout = ack_timeout
        self.max_retries = max_retries
        self._decoder = Decoder()
        self._tx = b""
        self._in_flight = False
        self._tx_seq = 0
        self._retries_left = 0
        self._deadline = 0.0
        self._peer: Optional[tuple[int, int]] = None  # session and seq of the last DATA delivered
        self._stats = LinkStats()

    @property
    def busy(self) -> bool:
        return self._in_flight

    @property
    def stats(self) -> LinkStats:
        self._stats.wire = self._decoder.stats
        return self._stats

    def send(self, payload: bytes, now: float) -> None:
        if len(payload) > MAX_PAYLOAD:
            raise ValueError(f"payload longer than {MAX_PAYLOAD} bytes")
        if self._in_flight:
            raise BusyError("a reliable message is still in flight")
        self._tx = encode_frame(Frame(KIND_DATA, self.session, self._tx_seq, bytes(payload)))
        self._in_flight = True
        self._retries_left = self.max_retries
        self._deadline = now + self.ack_timeout
        self._write(self._tx)

    def send_datagram(self, payload: bytes) -> None:
        self._write(encode_frame(Frame(KIND_DATAGRAM, self.session, 0, bytes(payload))))

    def receive(self, data: bytes, now: float) -> None:
        for frame in self._decoder.feed(data):
            self._on_frame(frame)
        self.poll(now)

    def poll(self, now: float) -> None:
        if not self._in_flight or now < self._deadline:
            return
        if self._retries_left == 0:
            self._finish(False)
            return
        self._retries_left -= 1
        self._stats.retransmits += 1
        self._deadline = now + self.ack_timeout
        self._write(self._tx)

    def time_to_deadline(self, now: float) -> Optional[float]:
        return max(0.0, self._deadline - now) if self._in_flight else None

    def _finish(self, delivered: bool) -> None:
        self._in_flight = False
        self._tx_seq = (self._tx_seq + 1) & 0xFF
        if delivered:
            self._stats.sent += 1
        else:
            self._stats.failed += 1
        if self._on_sent is not None:
            self._on_sent(delivered)

    def _deliver(self, frame: Frame, reliable: bool) -> bool:
        accepted = self._on_message(frame.payload, reliable) is not False
        if accepted:
            self._stats.received += 1
        else:
            self._stats.refused += 1
        return accepted

    def _ack(self, frame: Frame) -> None:
        self._write(encode_frame(Frame(KIND_ACK, frame.session, frame.seq)))

    def _on_frame(self, frame: Frame) -> None:
        if frame.kind == KIND_DATA:
            if self._peer == (frame.session, frame.seq):
                self._stats.duplicates += 1
                self._ack(frame)
            elif self._deliver(frame, True):
                self._peer = (frame.session, frame.seq)
                self._ack(frame)
        elif frame.kind == KIND_DATAGRAM:
            self._deliver(frame, False)
        elif (frame.kind == KIND_ACK and self._in_flight and frame.session == self.session
              and frame.seq == self._tx_seq and not frame.payload):
            self._finish(True)
        else:
            self._stats.ignored += 1


# --- command-line tool -----------------------------------------------------------------------

def _show(payload: bytes) -> str:
    try:
        text = payload.decode("utf-8")
        if text.isprintable():
            return text
    except UnicodeDecodeError:
        pass
    return payload.hex(" ")


class SerialLink:
    """A Link on a serial port, driven from the calling thread."""

    def __init__(self, port: str, baud: int, ack_timeout: float, retries: int,
                 on_message: Callable[[bytes, bool], Optional[bool]]) -> None:
        try:
            import serial  # pyserial
        except ImportError:
            sys.exit("the command-line tool needs pyserial: pip install pyserial")
        self.port = serial.Serial(port, baud, timeout=0)
        self.result: Optional[bool] = None
        self.link = Link(self.port.write, on_message, self._sent, ack_timeout=ack_timeout, max_retries=retries)

    def _sent(self, delivered: bool) -> None:
        self.result = delivered

    def run(self, seconds: float) -> None:
        """Reads and handles bytes for the given time, or until a reliable send has finished."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            data = self.port.read(4096)
            self.link.receive(data, time.monotonic())
            if self.result is not None:
                return
            if not data:
                time.sleep(0.002)

    def send(self, payload: bytes) -> bool:
        self.result = None
        self.link.send(payload, time.monotonic())
        while self.result is None:
            self.run(1.0)
        return self.result


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", required=True, help="serial port, for example COM5 or /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--ack-timeout", type=float, default=0.1, help="seconds")
    parser.add_argument("--retries", type=int, default=5)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("listen", help="print every message; acknowledge the reliable ones")
    send = commands.add_parser("send", help="send messages and print what comes back")
    send.add_argument("text")
    send.add_argument("--count", type=int, default=1)
    send.add_argument("--datagram", action="store_true", help="send without acknowledgement")
    args = parser.parse_args(argv)

    def on_message(payload: bytes, reliable: bool) -> bool:
        print(f"{'reliable' if reliable else 'datagram'}: {_show(payload)}", flush=True)
        return True

    serial_link = SerialLink(args.port, args.baud, args.ack_timeout, args.retries, on_message)
    try:
        if args.command == "listen":
            while True:
                serial_link.run(3600)
        delivered = 0
        for i in range(args.count):
            payload = f"{args.text} {i}".encode() if args.count > 1 else args.text.encode()
            if args.datagram:
                serial_link.link.send_datagram(payload)
                serial_link.run(0.05)
                continue
            start = time.monotonic()
            ok = serial_link.send(payload)
            delivered += ok
            elapsed_ms = (time.monotonic() - start) * 1000
            print(f"sent {_show(payload)}: {'acknowledged' if ok else 'not acknowledged'} after {elapsed_ms:.1f} ms")
        serial_link.run(0.3)  # answers that are still on the way
        if not args.datagram:
            print(f"{delivered} of {args.count} acknowledged; {serial_link.link.stats.retransmits} sent again")
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
