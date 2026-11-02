#!/usr/bin/env python3
"""Write small deterministic RNG1 fixtures; not a high-throughput DMA producer.

Each epoch contains a 32-byte little-endian header followed by block-major
Philox4x32-10 words. Include warmup epochs in --frames when replaying a file.
The wire protocol is driver-agnostic; XDMA devices may require a separate
board-specific bridge for alignment, DMA descriptors and control/completion.
"""

import argparse
import struct

MASK = (1 << 32) - 1
MAGIC = 0x31474E52  # little-endian bytes b"RNG1"
VERSION = 1


def philox(counter, key):
    """Philox4x32-10; unsigned 32-bit arithmetic, no third-party dependency."""
    c = tuple(counter)
    k = tuple(key)
    for _ in range(10):
        a = 0xD2511F53 * c[0]
        b = 0xCD9E8D57 * c[2]
        c = ((b >> 32) ^ c[1] ^ k[0], b & MASK,
             (a >> 32) ^ c[3] ^ k[1], a & MASK)
        k = ((k[0] + 0x9E3779B9) & MASK, (k[1] + 0xBB67AE85) & MASK)
    return c


def write_stream(output, count, blocks, frames, seed, workload):
    header = struct.Struct("<8I")
    vector = struct.Struct("<4I")
    for epoch in range(frames):
        output.write(header.pack(MAGIC, VERSION, count, blocks, epoch, seed, workload, 0))
        for block in range(blocks):
            for entity in range(count):
                output.write(vector.pack(*philox((entity, block, epoch, workload), (seed, 0))))


def self_test():
    # Published Random123 known-answer vectors (Philox4x32, 10 rounds).
    # https://github.com/DEShawResearch/random123/blob/main/tests/kat_vectors
    assert philox((0, 0, 0, 0), (0, 0)) == (0x6627E8D5, 0xE169C58D, 0xBC57AC4C, 0x9B00DBD8)
    assert philox((MASK,) * 4, (MASK,) * 2) == (0x408F276D, 0x41C83B0E, 0xA20BC7C6, 0x6D5451FD)
    assert philox((0x243F6A88, 0x85A308D3, 0x13198A2E, 0x03707344), (0xA4093822, 0x299F31D0)) == (
        0xD16CFE09, 0x94FDCCEB, 0x5001E420, 0x24126EA1)
    import io
    first = io.BytesIO()
    second = io.BytesIO()
    write_stream(first, 3, 2, 2, 42, 1)
    write_stream(second, 3, 2, 2, 42, 1)
    data = first.getvalue()
    assert data == second.getvalue() and len(data) == 2 * (32 + 3 * 2 * 16)
    assert data[:4] == b"RNG1"
    assert struct.unpack_from("<8I", data, 128) == (MAGIC, VERSION, 3, 2, 1, 42, 1, 0)
    for epoch in range(2):
        for block in range(2):
            for entity in range(3):
                offset = epoch * 128 + 32 + (block * 3 + entity) * 16
                assert struct.unpack_from("<4I", data, offset) == philox((entity, block, epoch, 1), (42, 0))
    print("PASS Philox known answers and deterministic RNG1 framing/layout")


def unsigned(text):
    if not text.isascii() or not text.isdecimal():
        raise argparse.ArgumentTypeError("expected an unsigned decimal integer")
    value = int(text)
    if value > MASK:
        raise argparse.ArgumentTypeError("value exceeds uint32")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", help="output framed binary file")
    parser.add_argument("--count", type=unsigned, default=259)
    parser.add_argument("--blocks", type=unsigned, default=7)
    parser.add_argument("--frames", type=unsigned, default=3, help="all epochs, including warmup")
    parser.add_argument("--seed", type=unsigned, default=42)
    parser.add_argument("--workload", choices=("pi", "walk"), default="pi")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    if not args.output:
        if args.self_test:
            return
        parser.error("--output is required unless --self-test is specified")
    if not args.count or not args.blocks or not args.frames:
        parser.error("count, blocks and frames must be positive")
    total_bytes = args.frames * (32 + args.count * args.blocks * 16)
    print(f"Writing {args.frames} epochs, {total_bytes} bytes to {args.output}")
    with open(args.output, "wb") as output:
        write_stream(output, args.count, args.blocks, args.frames, args.seed, int(args.workload == "walk"))


if __name__ == "__main__":
    main()
