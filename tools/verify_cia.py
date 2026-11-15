#!/usr/bin/env python3
"""Check CIA structure and SHA-256 hashes, without claiming RSA authenticity."""

import argparse
import hashlib
import struct
import sys
from pathlib import Path


class InvalidCia(Exception):
    pass


def aligned(value):
    return (value + 63) & ~63


def region(data, offset, size, label):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise InvalidCia(f"{label} extends beyond its containing region")
    return data[offset:offset + size]


def number(data, offset, size, endian="little"):
    return int.from_bytes(region(data, offset, size, "numeric field"), endian)


def signed_body(data, label):
    signature_type = number(data, 0, 4, "big")
    sizes = {0x10000: 0x240, 0x10001: 0x140, 0x10002: 0x80,
             0x10003: 0x240, 0x10004: 0x140, 0x10005: 0x80}
    if signature_type not in sizes:
        raise InvalidCia(f"{label}: unknown signature type 0x{signature_type:x}")
    return region(data, sizes[signature_type], len(data) - sizes[signature_type], label)


class Auditor:
    def __init__(self):
        self.checked = 0
        self.errors = []
        self.unsupported = []

    def require(self, condition, message):
        if not condition:
            self.errors.append(message)

    def digest(self, data, stored, label):
        self.checked += 1
        self.require(hashlib.sha256(data).digest() == stored,
                     f"{label}: SHA-256 mismatch")

    def ncch(self, data, label, title_id):
        if data[0x100:0x104] != b"NCCH":
            self.unsupported.append(f"{label}: content is not NCCH")
            return
        region(data, 0, 0x200, label)
        unit = 0x200 << data[0x18e]
        self.require(number(data, 0x104, 4) * unit == len(data),
                     f"{label}: NCCH size differs from content size")
        self.require(number(data, 0x118, 8) == title_id,
                     f"{label}: NCCH program ID differs from CIA title ID")
        exheader_size = number(data, 0x180, 4)
        encrypted = not (data[0x18f] & 4)
        if exheader_size:
            exheader = region(data, 0x200, exheader_size, label + " exheader")
            if encrypted:
                self.unsupported.append(f"{label}: encrypted NCCH exheader/ExeFS/RomFS")
            else:
                self.digest(exheader, data[0x160:0x180], label + " exheader")
        for name, field, hash_field in (("logo", 0x198, 0x130),
                                       ("ExeFS", 0x1a0, 0x1c0),
                                       ("RomFS", 0x1b0, 0x1e0)):
            start = number(data, field, 4) * unit
            size = number(data, field + 4, 4) * unit
            if not size:
                continue
            section = region(data, start, size, label + " " + name)
            if encrypted and name != "logo":
                if not exheader_size:
                    self.unsupported.append(f"{label}: encrypted NCCH {name}")
                continue
            hash_size = size if name == "logo" else number(data, field + 8, 4) * unit
            self.require(hash_size > 0, f"{label} {name}: empty hash region")
            self.digest(region(section, 0, hash_size, label + " hash region"),
                        data[hash_field:hash_field + 32], label + " " + name)
            if name == "ExeFS":
                region(section, 0, 0x200, label + " ExeFS header")
                for index in range(10):
                    entry = section[index * 16:(index + 1) * 16]
                    filename = entry[:8].split(b"\0", 1)[0]
                    if not filename:
                        continue
                    filename = filename.decode("ascii", errors="replace")
                    offset = number(entry, 8, 4) + 0x200
                    length = number(entry, 12, 4)
                    payload = region(section, offset, length, label + " " + filename)
                    hash_offset = 0x1e0 - index * 32
                    self.digest(payload, section[hash_offset:hash_offset + 32],
                                label + " ExeFS " + filename)

    def cia(self, data, expected_title_id=None, expected_version=None):
        region(data, 0, 0x2020, "CIA header")
        header, _, _, cert_size, ticket_size, tmd_size, meta_size, content_size = \
            struct.unpack_from("<IHHIIIIQ", data)
        self.require(header >= 0x2020, "CIA header is smaller than its content index")
        cert_offset = aligned(header)
        ticket_offset = aligned(cert_offset + cert_size)
        tmd_offset = aligned(ticket_offset + ticket_size)
        content_offset = aligned(tmd_offset + tmd_size)
        meta_offset = aligned(content_offset + content_size)
        region(data, cert_offset, cert_size, "certificate chain")
        ticket = signed_body(region(data, ticket_offset, ticket_size, "ticket"), "ticket")
        tmd = signed_body(region(data, tmd_offset, tmd_size, "TMD"), "TMD")
        region(data, content_offset, content_size, "CIA content")
        if meta_size:
            region(data, meta_offset, meta_size, "CIA metadata")
        end = meta_offset + meta_size if meta_size else content_offset + content_size
        self.require(len(data) == end or
                     (end < len(data) <= aligned(end) and not any(data[end:])),
                     "CIA file length differs from its declared sections")
        title_id = number(tmd, 0x4c, 8, "big")
        version = number(tmd, 0x9c, 2, "big")
        count = number(tmd, 0x9e, 2, "big")
        self.require(number(ticket, 0x9c, 8, "big") == title_id,
                     "ticket title ID differs from TMD")
        self.require(number(ticket, 0xa6, 2, "big") == version,
                     "ticket version differs from TMD")
        if expected_title_id is not None:
            self.require(title_id == expected_title_id,
                         f"title ID {title_id:016X} differs from expected {expected_title_id:016X}")
        if expected_version is not None:
            self.require(version == expected_version,
                         f"title version {version} differs from expected {expected_version}")
        infos = region(tmd, 0xc4, 64 * 0x24, "TMD content info table")
        chunks = region(tmd, 0x9c4, count * 0x30, "TMD chunk table")
        self.require(len(tmd) == 0x9c4 + count * 0x30,
                     "TMD length differs from its content count")
        self.digest(infos, tmd[0xa4:0xc4], "TMD content info table")
        coverage = [0] * count
        for index in range(64):
            entry = infos[index * 0x24:(index + 1) * 0x24]
            offset = number(entry, 0, 2, "big")
            length = number(entry, 2, 2, "big")
            if not length:
                continue
            records = region(chunks, offset * 0x30, length * 0x30, "TMD info record")
            self.digest(records, entry[4:36], f"TMD info record {index}")
            for chunk in range(offset, offset + length):
                coverage[chunk] += 1
        self.require(all(value == 1 for value in coverage),
                     "TMD info records do not cover every content exactly once")
        position = content_offset
        seen = set()
        content_ids = []
        selected = set()
        for ordinal in range(count):
            entry = chunks[ordinal * 0x30:(ordinal + 1) * 0x30]
            content_id = number(entry, 0, 4, "big")
            index = number(entry, 4, 2, "big")
            flags = number(entry, 6, 2, "big")
            size = number(entry, 8, 8, "big")
            self.require(index not in seen, f"duplicate content index {index}")
            seen.add(index)
            if not data[0x20 + index // 8] & (1 << (7 - index % 8)):
                continue
            selected.add(index)
            content_ids.append(f"{index}:{content_id:08X}")
            content = region(data, position, size, f"content {index}")
            position += size
            if flags & 1:
                self.unsupported.append(f"content {index}: CIA encryption requires a title key")
                continue
            self.digest(content, entry[0x10:0x30], f"content {index}")
            self.ncch(content, f"content {index}", title_id)
        self.require(position == content_offset + content_size,
                     "included content sizes differ from CIA content size")
        bitmap = data[0x20:0x2020]
        bitmap_count = sum(value.bit_count() for value in bitmap)
        self.require(bitmap_count == len(selected),
                     "CIA content index includes an index absent from the TMD")
        print(f"Title ID: {title_id:016X}; title version: {version}; content IDs: {', '.join(content_ids)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cia", type=Path)
    parser.add_argument("--title-id", type=lambda value: int(value, 16), help="expected hexadecimal title ID")
    parser.add_argument("--version", type=lambda value: int(value, 0), help="expected numeric TMD/ticket version")
    args = parser.parse_args()
    audit = Auditor()
    try:
        data = args.cia.read_bytes()
        audit.cia(data, args.title_id, args.version)
    except (OSError, InvalidCia, struct.error) as error:
        audit.errors.append(str(error))
    for message in audit.errors:
        print("FAIL: " + message, file=sys.stderr)
    for message in audit.unsupported:
        print("UNSUPPORTED: " + message, file=sys.stderr)
    if audit.errors:
        return 1
    print(f"Verified {audit.checked} SHA-256 hashes and CIA section sizes.")
    print("File SHA-256: " + hashlib.sha256(data).hexdigest())
    return 2 if audit.unsupported else 0


if __name__ == "__main__":
    sys.exit(main())
