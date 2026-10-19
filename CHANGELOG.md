# Changelog

## 1.0.0 (2026-10-19)

First release.

- Portable protocol core in C11: COBS framing, CRC-16, frame decoder with error counters, and a stop-and-wait link with acknowledgements, retries, duplicate filtering, sessions and back-pressure
- ESP-IDF component: UART driver and one task per link, blocking reliable send, datagrams and a receive queue
- Python implementation and command-line tool for the PC side
- Examples: echo, and a sensor stream from two producer tasks on two cores
- Unity tests with a simulated lossy line, Python tests and a byte-for-byte cross-check against the C core, a libFuzzer target and cppcheck; CI builds for ESP-IDF 5.5 and 6.0 (ESP32, ESP32-S3, ESP32-C3)
