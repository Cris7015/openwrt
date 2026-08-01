#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build and fail-closed validate an XR500v slot-B image."""

import hashlib
import lzma
import struct
import sys
from pathlib import Path


HEADER_SIZE = 0x200
KERNEL_PARTITION_SIZE = 0x300000
KERNEL_PAYLOAD_LIMIT = KERNEL_PARTITION_SIZE - HEADER_SIZE
ROOTFS_PARTITION_SIZE = 0x1000000
KERNEL_LOADADDR = 0x80020000
FIRMWARE_PARTITIONS_SIZE = KERNEL_PARTITION_SIZE + ROOTFS_PARTITION_SIZE

# These values reproduce the opaque prefix emitted by the generator used for
# the hardware-tested image.  In particular, LEGACY_PREFIX_BOARD_WORD is not
# a confirmed TP-Link HWID and must not be exposed as one in the device profile.
LEGACY_PREFIX_BOARD_WORD = 0x0EC60001
LEGACY_PREFIX_REVISION = 1
LEGACY_PREPATCH_FW_LENGTH = 0x00FA0000
LEGACY_MD5_SALT = bytes.fromhex("dcd73aa5c39598fbdcf9e7f40eae4737")

TRENDCHIP_MAGIC = bytes.fromhex("4c3d2e1faa55aa55")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"XR500v image validation failed: {message}")


def build_header(payload: bytes) -> bytearray:
    """Reproduce the tested legacy prefix, then apply the TrendChip fields."""
    header = bytearray(b"\xff" * HEADER_SIZE)

    # Opaque compatibility prefix.  This first pass also preserves the legacy
    # salted digest bytes of the exact hardware-tested image pipeline.
    struct.pack_into("<I", header, 0x00, 3)
    header[0x04:0x0D] = b"ver. 2.0\0"
    struct.pack_into(">I", header, 0x34, LEGACY_PREFIX_BOARD_WORD)
    struct.pack_into(">I", header, 0x38, LEGACY_PREFIX_REVISION)
    struct.pack_into(">I", header, 0x3C, 0)
    header[0x40:0x50] = LEGACY_MD5_SALT
    struct.pack_into(">I", header, 0x50, 0)
    struct.pack_into(">I", header, 0x68, KERNEL_LOADADDR)
    struct.pack_into(">I", header, 0x6C, KERNEL_LOADADDR)
    struct.pack_into(">I", header, 0x70, LEGACY_PREPATCH_FW_LENGTH)
    struct.pack_into(">I", header, 0x74, HEADER_SIZE)
    struct.pack_into(">I", header, 0x78, len(payload))
    struct.pack_into(">I", header, 0x84, 0)
    struct.pack_into(">I", header, 0x88, 0)
    header[0x8C:0x8E] = bytes.fromhex("55aa")
    header[0x8E] = 1
    header[0x8F] = 0
    header[0x90] = 0xA5
    header[0x91:0x94] = bytes(3)
    header[0x40:0x50] = hashlib.md5(header + payload).digest()

    # TrendChip/Bootbase consumes this hybrid wrapper rather than a standard
    # TP-Link v2 header.  These writes intentionally follow the legacy digest.
    header[0x60:0x68] = TRENDCHIP_MAGIC
    struct.pack_into(">I", header, 0x70, FIRMWARE_PARTITIONS_SIZE)
    struct.pack_into(">I", header, 0x7C, KERNEL_PARTITION_SIZE)
    struct.pack_into(">I", header, 0x80, ROOTFS_PARTITION_SIZE)
    struct.pack_into(">I", header, 0x88, 0)
    header[0x8C:0x90] = bytes.fromhex("55aa0101")

    return header


def main() -> None:
    require(len(sys.argv) == 2, f"usage: {sys.argv[0]} IMAGE")
    image = Path(sys.argv[1])
    payload = image.read_bytes()
    rootfs_size = len(payload) - KERNEL_PARTITION_SIZE

    require(len(payload) >= KERNEL_PARTITION_SIZE + 96,
            "kernel/rootfs payload is truncated")
    require(rootfs_size <= ROOTFS_PARTITION_SIZE,
            "rootfs payload exceeds rootfs1")
    require(payload[KERNEL_PARTITION_SIZE - HEADER_SIZE:
                    KERNEL_PARTITION_SIZE] == bytes(HEADER_SIZE),
            "required 512-byte gap is not zero")
    require(payload[KERNEL_PARTITION_SIZE:
                    KERNEL_PARTITION_SIZE + 4] == b"hsqs",
            "SquashFS is not at raw payload offset 0x300000")

    decompressor = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
    try:
        kernel = decompressor.decompress(
            payload[:KERNEL_PAYLOAD_LIMIT]
        )
    except lzma.LZMAError as exc:
        raise SystemExit(
            f"XR500v image validation failed: invalid kernel LZMA stream: {exc}"
        ) from exc
    require(decompressor.eof, "kernel LZMA stream is truncated")
    require(not any(decompressor.unused_data),
            "nonzero bytes follow the kernel LZMA stream")
    require(len(kernel) <= KERNEL_PAYLOAD_LIMIT,
            "decompressed kernel exceeds the safe 0x2ffe00 limit")

    squashfs_size = struct.unpack_from(
        "<Q", payload, KERNEL_PARTITION_SIZE + 40
    )[0]
    require(squashfs_size == rootfs_size,
            "SquashFS bytes_used does not match the rootfs payload")

    data = build_header(payload) + payload
    require(len(data) == HEADER_SIZE + len(payload),
            "internal wrapper length mismatch")
    require(data[0x60:0x68] == TRENDCHIP_MAGIC,
            "internal TrendChip magic mismatch")
    require(data[0x90] == 0xA5, "internal legacy marker mismatch")

    temporary = image.with_name(image.name + ".new")
    temporary.write_bytes(data)
    temporary.replace(image)


if __name__ == "__main__":
    main()
