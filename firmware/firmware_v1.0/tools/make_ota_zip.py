#!/usr/bin/env python3
"""zephyr.hex から、もみじ録音の無線更新（旧式 Nordic DFU）用の zip を作る。

`adafruit-nrfutil dfu genpkg --dev-type 0x0052 --dev-revision 0xCE68 --application zephyr.hex out.zip`
と同じ中身（app.bin・app.dat・manifest.json）を、標準ライブラリだけで作る。
adafruit-nrfutil はビルド用コンテナの Python と相性が悪く入らないことがあるため。

使い方: python3 make_ota_zip.py zephyr.hex pendant-ota.zip
"""

import json
import struct
import sys
import zipfile

DEVICE_TYPE = 0x0052  # Adafruit nRF52 Bootloader が見る値（dfu_init.c の ADAFRUIT_DEVICE_TYPE）
DEVICE_REVISION = 0xCE68  # 本家 readme の値（アプリだけの更新ではブートローダは見ない）
APPLICATION_VERSION = 0xFFFFFFFF  # genpkg の既定
SOFTDEVICE_ANY = 0xFFFE  # genpkg の既定（どの SoftDevice でも可）
MBR_END = 0x1000  # これより下は MBR（nrfutil の nRFHex.minaddr と同じ扱い）
UICR_START = 0x10000000  # UICR の記録は送らない（nrfutil の nRFHex も読み込み時に捨てる）


def read_hex(path):
    """Intel HEX を {アドレス: バイト} にする（型 00・01・02・04 だけを使う）。"""
    data = {}
    base = 0
    with open(path, encoding="ascii") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            if not line.startswith(":"):
                raise ValueError(f"line {lineno}: not an Intel HEX record")
            raw = bytes.fromhex(line[1:])
            if sum(raw) & 0xFF:
                raise ValueError(f"line {lineno}: checksum mismatch")
            count, addr, rtype = raw[0], (raw[1] << 8) | raw[2], raw[3]
            payload = raw[4:4 + count]
            if rtype == 0x00:
                for i, b in enumerate(payload):
                    data[base + addr + i] = b
            elif rtype == 0x01:
                break
            elif rtype == 0x02:
                base = ((payload[0] << 8) | payload[1]) << 4
            elif rtype == 0x04:
                base = ((payload[0] << 8) | payload[1]) << 16
            # 03・05（開始アドレス）は使わない
    return data


def to_bin(data):
    data = {a: b for a, b in data.items() if a < UICR_START}
    start = max(min(data), MBR_END)
    end = max(data)
    size = (end - start + 1 + 3) // 4 * 4  # 4 の倍数へ切り上げ（ブートローダが 4 の倍数しか受けない）
    return bytes(data.get(start + i, 0xFF) for i in range(size))


def crc16(data):
    """Nordic の crc16_compute（CRC-CCITT、初期値 0xFFFF）。"""
    crc = 0xFFFF
    for b in data:
        crc = ((crc >> 8) & 0xFF) | ((crc << 8) & 0xFFFF)
        crc ^= b
        crc ^= (crc & 0xFF) >> 4
        crc ^= (crc << 12) & 0xFFFF
        crc ^= ((crc & 0xFF) << 5) & 0xFFFF
    return crc


def make(hex_path, zip_path):
    image = to_bin(read_hex(hex_path))
    crc = crc16(image)
    init = struct.pack("<HHIHHH", DEVICE_TYPE, DEVICE_REVISION, APPLICATION_VERSION, 1, SOFTDEVICE_ANY, crc)
    manifest = {
        "manifest": {
            "application": {
                "bin_file": "app.bin",
                "dat_file": "app.dat",
                "init_packet_data": {
                    "application_version": APPLICATION_VERSION,
                    "device_revision": DEVICE_REVISION,
                    "device_type": DEVICE_TYPE,
                    "firmware_crc16": crc,
                    "softdevice_req": [SOFTDEVICE_ANY],
                },
            },
            "dfu_version": 0.5,
        }
    }
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=4))
        z.writestr("app.dat", init)
        z.writestr("app.bin", image)
    print(f"{zip_path}: app.bin {len(image)} bytes, crc16 0x{crc:04X}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    make(sys.argv[1], sys.argv[2])
