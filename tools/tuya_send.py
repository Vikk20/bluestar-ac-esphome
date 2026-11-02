#!/usr/bin/env python3
# Build Tuya module->MCU frames. Usage:
#   tuya_send.py rec KK V      -> cmd 0x06 DP101 Blue Star record (*LLKKV + CRC-16/MODBUS)
#   tuya_send.py dp N type HEX -> cmd 0x06 raw DP (type 1 bool, 2 value, 3 string, 4 enum)
#   tuya_send.py cmd C [HEX]   -> arbitrary command C with optional payload
import sys

def crc16_modbus(data):
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c

def frame(cmd, payload=b''):
    f = bytes([0x55, 0xAA, 0, cmd]) + len(payload).to_bytes(2, 'big') + payload
    return f + bytes([sum(f) & 0xFF])

def record(key, val):
    body = f'{key}{val}'.upper()
    s = f'*{len(body):02X}{body}'
    return f'{s}{crc16_modbus(s.encode()):04X}#'.encode()

def dp(n, t, v):
    return bytes([n, t]) + len(v).to_bytes(2, 'big') + v

a = sys.argv[1:]
if a[0] == 'rec':
    f = frame(6, dp(101, 3, record(a[1], a[2])))
elif a[0] == 'dp':
    f = frame(6, dp(int(a[1]), int(a[2]), bytes.fromhex(a[3])))
else:
    f = frame(int(a[1], 16), bytes.fromhex(a[2]) if len(a) > 2 else b'')
print(f.hex(' ').upper())
