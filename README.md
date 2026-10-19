# esp32-uart-link

[![CI](https://github.com/PATEmbedded/esp32-uart-link/actions/workflows/ci.yml/badge.svg)](https://github.com/PATEmbedded/esp32-uart-link/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Messages over a plain UART, between an ESP32 and a PC, another microcontroller or a second ESP32. Every message is framed and checked with a CRC. Reliable messages are acknowledged, repeated when they get lost, and delivered exactly once and in order.

```c
#include "uart_link.h"

uart_link_config_t config = UART_LINK_CONFIG_DEFAULT(UART_NUM_1, 4, 5);  // TX on GPIO4, RX on GPIO5
uart_link_handle_t link;
ESP_ERROR_CHECK(uart_link_open(&config, &link));

const char hello[] = "hello";
if (uart_link_send(link, hello, sizeof(hello), portMAX_DELAY) == ESP_OK) {
    // the other side has it
}

uart_link_message_t message;
if (uart_link_receive(link, &message, pdMS_TO_TICKS(1000)) == ESP_OK) {
    // message.data, message.len
}
```

On the PC, the same protocol in Python:

```sh
python tools/uart_link.py --port COM5 send "hello" --count 10
```

## Why

A UART carries bytes, not messages. Text lines break as soon as the data holds a newline, one lost byte shifts everything after it, and nothing tells the sender whether the other side got the message. Most projects grow their own start byte, length field and timeout, usually without tests.

This library gives a UART message boundaries, integrity checks and delivery confirmation. The protocol core is a few hundred lines of portable C without I/O, so the unit tests run it on a PC over a simulated line that loses and damages frames.

## Features

- COBS framing: a zero byte ends every frame, so a receiver that starts in the middle of the stream or loses bytes finds the next frame at once
- CRC-16 on every frame
- Reliable messages: acknowledged, repeated after a timeout, delivered exactly once and in order
- Datagrams for data that is useless when late, such as a sensor stream
- Back-pressure: while the receive queue is full, reliable messages are not acknowledged, and the sender repeats them later instead of losing them
- Sessions: a sender that restarts is recognised, so its first message is not taken for a repeat of an old one
- Counters for every kind of error, from CRC failures to UART overflows
- Thread-safe ESP-IDF API: any task can send; one task per link serves the UART
- A Python implementation for the PC side, checked against the C code byte for byte

## How it works

A reliable message whose acknowledgement gets lost:

```mermaid
sequenceDiagram
    participant A as Sender
    participant B as Receiver
    A-xB: DATA seq 7
    Note over A: no ACK within ack_timeout_ms
    A->>B: DATA seq 7, again
    Note over B: delivered
    B-xA: ACK seq 7
    Note over A: still no ACK
    A->>B: DATA seq 7, again
    Note over B: seen already, not delivered twice
    B->>A: ACK seq 7
    Note over A: done; the next message is seq 8
```

Every frame on the wire:

| Field | Bytes | |
|---|---|---|
| kind | 1 | 1 = DATA, 2 = ACK, 3 = DATAGRAM |
| session | 2 | little endian; chosen at random on every start |
| seq | 1 | sequence number of a DATA frame, repeated in its ACK |
| payload | 0 to 256 | the message; ACK frames have none |
| CRC | 2 | CRC-16/CCITT-FALSE of all bytes before it, little endian |

The frame is then COBS-encoded, which adds one byte per started 254 bytes, and followed by a zero byte. An ACK for session 0x1234, sequence number 0x56 is `07 02 34 12 56 EF 2F 00` on the wire. Frames of unknown kinds are counted and ignored, so a newer peer can add kinds.

Each direction has one reliable message in flight at a time (stop-and-wait). That keeps the protocol small and the order guaranteed. Data that needs more throughput than that, and can live with losses, goes as datagrams.

## Repository layout

| Path | |
|---|---|
| [core/](core/) | the protocol: COBS, CRC, frames and the link, in portable C11 without I/O |
| [include/](include/), [src/](src/) | the ESP-IDF component: UART driver, task, queues |
| [tools/uart_link.py](tools/uart_link.py) | the Python implementation and a command-line tool |
| [examples/echo](examples/echo) | sends every message back |
| [examples/sensor_stream](examples/sensor_stream) | two producer tasks on two cores stream readings through a queue |
| [test/host](test/host), [test/python](test/python), [fuzz](fuzz) | unit tests, the Python tests and the cross-check, the fuzz target |

## Installation

**ESP-IDF**: add the repository as a component, for example as a submodule:

```sh
git submodule add https://github.com/PATEmbedded/esp32-uart-link.git components/esp32-uart-link
```

The largest payload is set in menuconfig under *UART link* (256 bytes by default). Both ends must use the same value; for the Python side it is `MAX_PAYLOAD` in `uart_link.py`.

**PC**: `tools/uart_link.py` needs Python 3.9 or newer. The command-line tool also needs pyserial (`pip install pyserial`).

## Usage on the ESP32

| Field of `uart_link_config_t` | Default | |
|---|---|---|
| `port`, `tx_pin`, `rx_pin` | from the macro | `UART_PIN_NO_CHANGE` keeps a pin |
| `baud_rate` | 115200 | 8 data bits, no parity, one stop bit |
| `ack_timeout_ms` | 100 | wait this long for an ACK before sending again |
| `max_retries` | 5 | repeats after the first attempt |
| `rx_queue_length` | 8 | received messages waiting for `uart_link_receive()` |
| `task_priority`, `task_stack_size`, `task_core` | 10, 4096, any | the link task |

| Call | Returns |
|---|---|
| `uart_link_send()` | `ESP_OK` once acknowledged, `ESP_FAIL` when the retries ran out, `ESP_ERR_TIMEOUT` when another task kept the turn longer than `wait` |
| `uart_link_send_datagram()` | at once; the message is not acknowledged |
| `uart_link_receive()` | the next message, or `ESP_ERR_TIMEOUT` |
| `uart_link_get_stats()` | delivery, framing and UART counters |

`uart_link_send()` blocks for at most about `ack_timeout_ms × (max_retries + 1)`. Several tasks may send; they take turns. UART0 carries the log on most boards, so use another UART for the link.

## Usage on the PC

```sh
python tools/uart_link.py --port /dev/ttyUSB0 listen                # print messages, acknowledge reliable ones
python tools/uart_link.py --port COM5 send "ping" --count 20        # send, wait for each acknowledgement
python tools/uart_link.py --port COM5 send "tick" --count 5 --datagram
```

In your own Python code, `Link` is the same state machine as the C core. You pass in bytes and the time, and it calls back with bytes to write and messages received:

```python
from uart_link import Link

link = Link(write=port.write, on_message=lambda payload, reliable: print(payload))
link.send(b"hello", time.monotonic())
link.receive(port.read(4096), time.monotonic())  # in your read loop; also repeats lost messages
```

## Examples

Both examples use UART1 with TX on GPIO4 and RX on GPIO5; menuconfig changes that under *Example*. For a PC, connect a 3.3 V USB serial adapter: adapter TX to GPIO5, adapter RX to GPIO4, and GND to GND.

- [echo](examples/echo) sends every message back, reliable ones reliably, and logs the counters while idle. Try it with `send "hello" --count 10`.
- [sensor_stream](examples/sensor_stream) runs two producer tasks, one on each core, that put readings into a FreeRTOS queue. A third task sends them on as reliable messages. Watch them with `listen`.

## Using the core on other microcontrollers

`core/` has no dependencies beyond the C standard library. Give `ul_link_init()` a `write` callback that sends bytes, call `ul_link_receive()` with every chunk of received bytes, and call `ul_link_poll()` when `ul_link_time_to_deadline()` says so. The ESP-IDF component in `src/uart_link.c` does exactly this and shows one way to share the link between tasks.

## Tests

- Unit tests (Unity) for COBS, with the examples of the Wikipedia article on COBS, the CRC check value, frames and the decoder, and the link. The link tests join two links over a simulated line that drops 20 % of the frames and flips a bit in 5 %, and check that 300 messages arrive exactly once and in order. They also cover lost acknowledgements, refused messages, restarts and a cut line.
- Python tests for `uart_link.py`, and a cross-check against the C core through `ul_tool`: random frames, encoded by C and by Python, must match byte for byte, and damaged streams must give the same counters.
- A libFuzzer target for the receive path runs for two minutes in CI, and cppcheck checks the C code.
- CI runs everything with AddressSanitizer and UBSan under GCC and Clang, and builds both examples with ESP-IDF 5.5 and 6.0 for ESP32, ESP32-S3 and ESP32-C3.

```sh
cmake -S . -B build -DUL_SANITIZE=ON
cmake --build build
ctest --test-dir build --output-on-failure
UL_TOOL=build/ul_tool python -m pytest test/python
```

## Limits

- One reliable message in flight per direction.
- The byte stream must keep its order, as a UART, USB CDC or TCP does.
- The session number is 16 bits and random. If a restart happens to pick the same session as before, and the receiver's last message had the same sequence number as the first new one, that first message is taken for a repeat.
- The CRC detects noise, not tampering. There is no encryption or authentication.

## License

MIT. See [LICENSE](LICENSE).
