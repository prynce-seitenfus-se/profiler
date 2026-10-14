#!/usr/bin/env python3
"""Parse PROF-BIN binary profiler dumps and report execution statistics."""

import argparse
import csv
import re
import struct
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, NamedTuple, Optional, Sequence, TextIO, Tuple


HEADER_V2_FORMAT = "<4sHHIHH"
HEADER_V2_SIZE = struct.calcsize(HEADER_V2_FORMAT)
HEADER_V3_FORMAT = "<4sHHIHHHHHBB"
HEADER_V3_SIZE = struct.calcsize(HEADER_V3_FORMAT)
HEADER_V4_FORMAT = HEADER_V3_FORMAT + "HH"
HEADER_V4_SIZE = struct.calcsize(HEADER_V4_FORMAT)
HEADER_PREFIX_FORMAT = "<4sH"
SUPPORTED_VERSIONS = {2: HEADER_V2_SIZE, 3: HEADER_V3_SIZE, 4: HEADER_V4_SIZE}
TIME_MODES = {0: "wall", 1: "active"}
RECORD_FORMAT = "<IIQII"
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)
TRAILER_FORMAT = "<I"
TRAILER_SIZE = struct.calcsize(TRAILER_FORMAT)

OUTPUT_FIELDS = (
    "function",
    "function_address",
    "calls",
    "total_cycles",
    "min_cycles",
    "max_cycles",
    "avg_cycles",
    "avg_us",
    "min_us",
    "max_us",
    "source",
)


class ProfilerHeader(NamedTuple):
    magic: bytes
    version: int
    record_count: int
    frequency: int
    dropped_functions: int
    stack_overflows: int
    unmatched_exits: int = 0
    stale_frames: int = 0
    context_overflows: int = 0
    time_mode: int = 0
    header_size: int = HEADER_V2_SIZE
    probe_overhead: int = 0
    nested_overhead: int = 0

    @property
    def time_mode_name(self) -> str:
        return TIME_MODES.get(self.time_mode, f"unknown({self.time_mode})")


class MetricRecord(NamedTuple):
    fn_address: int
    call_count: int
    total_cycles: int
    min_cycles: int
    max_cycles: int


def calculate_crc32(seed: int, data: bytes) -> int:
    crc = seed
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xEDB88320
            else:
                crc >>= 1
    return crc


def parse_header(data: bytes) -> ProfilerHeader:
    if len(data) < struct.calcsize(HEADER_PREFIX_FORMAT):
        raise ValueError("dump file is smaller than minimum binary packet size")

    magic, version = struct.unpack_from(HEADER_PREFIX_FORMAT, data, 0)
    if magic != b"PROF":
        raise ValueError(f"invalid magic signature: {magic!r} (expected b'PROF')")
    if version not in SUPPORTED_VERSIONS:
        raise ValueError(f"unsupported PROF-BIN protocol version: {version}")

    header_size = SUPPORTED_VERSIONS[version]
    if len(data) < header_size + TRAILER_SIZE:
        raise ValueError("dump file is smaller than minimum binary packet size")

    if version == 2:
        _, _, count, frequency, dropped, stack_of = struct.unpack_from(
            HEADER_V2_FORMAT, data, 0
        )
        return ProfilerHeader(
            magic=magic,
            version=version,
            record_count=count,
            frequency=frequency,
            dropped_functions=dropped,
            stack_overflows=stack_of,
            header_size=header_size,
        )

    probe, nested = 0, 0
    if version == 3:
        fields = struct.unpack_from(HEADER_V3_FORMAT, data, 0)
    else:
        fields = struct.unpack_from(HEADER_V4_FORMAT, data, 0)
        probe, nested = fields[11], fields[12]
    (_, _, count, frequency, dropped, stack_of, unmatched, stale, ctx_of, mode, _) = fields[:11]
    return ProfilerHeader(
        magic=magic,
        version=version,
        record_count=count,
        frequency=frequency,
        dropped_functions=dropped,
        stack_overflows=stack_of,
        unmatched_exits=unmatched,
        stale_frames=stale,
        context_overflows=ctx_of,
        time_mode=mode,
        header_size=header_size,
        probe_overhead=probe,
        nested_overhead=nested,
    )


def read_binary_dump(path: Path) -> Tuple[ProfilerHeader, List[MetricRecord], int]:
    data = path.read_bytes()
    header = parse_header(data)
    count = header.record_count

    expected_len = header.header_size + (count * RECORD_SIZE) + TRAILER_SIZE
    if len(data) < expected_len:
        raise ValueError(
            f"truncated binary dump: expected {expected_len} bytes, got {len(data)} bytes"
        )

    records: List[MetricRecord] = []
    offset = header.header_size
    for _ in range(count):
        fn_addr, calls, total_cyc, min_cyc, max_cyc = struct.unpack_from(
            RECORD_FORMAT, data, offset
        )
        offset += RECORD_SIZE
        records.append(
            MetricRecord(
                fn_address=fn_addr,
                call_count=calls,
                total_cycles=total_cyc,
                min_cycles=min_cyc,
                max_cycles=max_cyc,
            )
        )

    (trailer_crc,) = struct.unpack_from(TRAILER_FORMAT, data, offset)

    # Validate CRC-32 over header and records payload
    crc = calculate_crc32(0xFFFFFFFF, data[:offset]) ^ 0xFFFFFFFF
    if crc != trailer_crc:
        raise ValueError(
            f"CRC-32 checksum mismatch: calculated 0x{crc:08X}, expected 0x{trailer_crc:08X}"
        )

    return header, records, trailer_crc

def resolve_symbols(
    addresses: Sequence[int], elf_path: Optional[Path], addr2line: str
) -> Dict[int, Tuple[str, str]]:
    if not addresses:
        return {}

    if elf_path is None:
        return {address: ("0x{:08x}".format(address), "") for address in addresses}

    with elf_path.open("rb") as elf:
        elf_header = elf.read(20)
    if len(elf_header) < 20 or elf_header[:4] != b"\x7fELF" or elf_header[5] not in (1, 2):
        raise ValueError("symbol file is not a valid ELF")
    byte_order = "little" if elf_header[5] == 1 else "big"
    is_arm = int.from_bytes(elf_header[18:20], byte_order) == 40
    command = [
        addr2line,
        "-f",
        "-C",
        "-e",
        str(elf_path),
    ] + ["0x{:08x}".format(address & ~1 if is_arm else address) for address in addresses]

    process = subprocess.run(
        command,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    lines = [line.strip() for line in process.stdout.splitlines()]
    if len(lines) != len(addresses) * 2:
        raise ValueError("addr2line returned an unexpected number of lines")

    symbols: Dict[int, Tuple[str, str]] = {}
    for index, address in enumerate(addresses):
        function_name = lines[index * 2]
        source_location = lines[index * 2 + 1]
        if function_name == "??":
            function_name = "0x{:08x}".format(address)
        symbols[address] = (function_name, source_location)
    return symbols


def resolve_text_log(text: str, elf_path: Path, addr2line: str) -> str:
    """Resolve the first address field of CSV/TOP rows, preserving other output."""
    pattern = re.compile(r"^(0x[0-9a-fA-F]{8})(?=,|\s)", re.MULTILINE)
    addresses = list(dict.fromkeys(int(match.group(1), 16) for match in pattern.finditer(text)))
    if not addresses:
        raise ValueError("text log contains no address-based profiler rows")
    symbols = resolve_symbols(addresses, elf_path, addr2line)
    return pattern.sub(lambda match: symbols[int(match.group(1), 16)][0], text)


def write_metrics_csv(
    output: TextIO,
    records: Sequence[MetricRecord],
    symbols: Dict[int, Tuple[str, str]],
    frequency: int,
) -> None:
    writer = csv.DictWriter(output, fieldnames=OUTPUT_FIELDS)
    writer.writeheader()
    for record in records:
        func_name, source = symbols.get(
            record.fn_address, ("0x{:08x}".format(record.fn_address), "")
        )
        avg_cycles = (
            record.total_cycles / record.call_count if record.call_count > 0 else 0
        )
        avg_us = (avg_cycles * 1000000.0) / frequency if frequency > 0 else 0.0
        min_us = (record.min_cycles * 1000000.0) / frequency if frequency > 0 else 0.0
        max_us = (record.max_cycles * 1000000.0) / frequency if frequency > 0 else 0.0

        writer.writerow(
            {
                "function": func_name,
                "function_address": "0x{:08x}".format(record.fn_address),
                "calls": record.call_count,
                "total_cycles": record.total_cycles,
                "min_cycles": record.min_cycles,
                "max_cycles": record.max_cycles,
                "avg_cycles": f"{avg_cycles:.1f}",
                "avg_us": f"{avg_us:.3f}",
                "min_us": f"{min_us:.3f}",
                "max_us": f"{max_us:.3f}",
                "source": source,
            }
        )


def write_summary(
    output: TextIO,
    header: ProfilerHeader,
    records: Sequence[MetricRecord],
    symbols: Dict[int, Tuple[str, str]],
) -> None:
    writer = csv.writer(output)
    writer.writerow(("frequency_hz", header.frequency))
    writer.writerow(("unique_functions", header.record_count))
    writer.writerow(("dropped_functions", header.dropped_functions))
    writer.writerow(("stack_overflows", header.stack_overflows))
    writer.writerow(("protocol_version", header.version))
    if header.version >= 3:
        writer.writerow(("unmatched_exits", header.unmatched_exits))
        writer.writerow(("stale_frames", header.stale_frames))
        writer.writerow(("context_overflows", header.context_overflows))
        writer.writerow(("time_mode", header.time_mode_name))
    if header.version >= 4:
        writer.writerow(("probe_overhead_cycles", header.probe_overhead))
        writer.writerow(("nested_overhead_cycles", header.nested_overhead))
    writer.writerow(
        ("function", "calls", "total_cycles", "min_cycles", "avg_cycles", "max_cycles", "avg_us")
    )
    for record in sorted(records, key=lambda r: r.total_cycles, reverse=True):
        func_name = symbols.get(
            record.fn_address, ("0x{:08x}".format(record.fn_address), "")
        )[0]
        avg_cycles = (
            record.total_cycles / record.call_count if record.call_count > 0 else 0
        )
        avg_us = (avg_cycles * 1000000.0) / header.frequency if header.frequency > 0 else 0.0
        writer.writerow(
            (
                func_name,
                record.call_count,
                record.total_cycles,
                record.min_cycles,
                f"{avg_cycles:.1f}",
                record.max_cycles,
                f"{avg_us:.3f}",
            )
        )


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Decode and resolve execution statistics from a PROF-BIN binary profiler dump."
    )
    parser.add_argument("dump_file", type=Path, help="PROF-BIN binary dump file")
    parser.add_argument("-e", "--elf", type=Path, help="ELF file used by arm-none-eabi-addr2line")
    parser.add_argument(
        "--text",
        action="store_true",
        help="resolve an address-based CSV/TOP text log (requires --elf)",
    )
    parser.add_argument(
        "--addr2line",
        default="arm-none-eabi-addr2line",
        help="addr2line executable (default: arm-none-eabi-addr2line)",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="write metrics CSV to this file (default: input filename with .csv suffix)",
    )
    parser.add_argument(
        "--summary",
        action="store_true",
        help="print aggregate summary table to stderr",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_argument_parser()
    args = parser.parse_args(argv)
    try:
        if args.text:
            if args.elf is None:
                raise ValueError("--text requires --elf")
            text = resolve_text_log(args.dump_file.read_text(encoding="utf-8"), args.elf, args.addr2line)
            output_path = args.output or args.dump_file.with_name(args.dump_file.stem + "_resolved.txt")
            if output_path.resolve() == args.dump_file.resolve():
                raise ValueError("text output must not overwrite the input log")
            output_path.write_text(text, encoding="utf-8")
            return 0
        header, records, _ = read_binary_dump(args.dump_file)
        if header.version >= 3:
            print(
                f"PROF-BIN v{header.version}: time_mode={header.time_mode_name} "
                f"unmatched_exits={header.unmatched_exits} stale_frames={header.stale_frames} "
                f"context_overflows={header.context_overflows} "
                f"stack_overflows={header.stack_overflows} "
                f"dropped_functions={header.dropped_functions}"
                + (
                    f" probe_overhead={header.probe_overhead} "
                    f"nested_overhead={header.nested_overhead}"
                    if header.version >= 4
                    else ""
                ),
                file=sys.stderr,
            )
        addresses = [record.fn_address for record in records]
        symbols = resolve_symbols(addresses, args.elf, args.addr2line)

        output_path = (
            args.output if args.output is not None else args.dump_file.with_suffix(".csv")
        )
        with output_path.open("w", encoding="utf-8", newline="") as csv_out:
            write_metrics_csv(csv_out, records, symbols, header.frequency)

        if args.summary:
            write_summary(sys.stderr, header, records, symbols)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
