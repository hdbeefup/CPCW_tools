#!/usr/bin/env python3
"""Extract and repack .pak files for Codename: Panzers Cold War."""

import argparse
import dataclasses
import os
import struct
import sys
import zlib

ROTATION = 0xBD
VER = 3
CHUNK_SIZE = 1024 * 1024  # 1 MB for streaming large files

DECRYPT_TABLE = bytes((i + ROTATION) & 0xFF for i in range(256))
ENCRYPT_TABLE = bytes((i - ROTATION) & 0xFF for i in range(256))

# Extensions stored uncompressed (large media files)
UNCOMPRESSED_EXTS = {'.avi', '.wav', '.ogg', '.bik', '.mp3', '.wmv'}

# Windows FILETIME epoch offset: 100-ns ticks between 1601-01-01 and 1970-01-01
FILETIME_EPOCH_DIFF = 116444736000000000


@dataclasses.dataclass
class FileEntry:
    name: str
    offset: int
    dummy: int
    zsize: int
    type: int       # 1=uncompressed, 2=zlib, 3=folder marker
    timestamp: bytes  # 8 raw bytes (Windows FILETIME)
    size: int


@dataclasses.dataclass
class FolderEntry:
    name: str
    files: list  # list[FileEntry]


def parse_index(pak_path):
    """Parse the pak file index and return (version, list of FolderEntry)."""
    with open(pak_path, 'rb') as f:
        f.seek(0, 2)
        file_size = f.tell()

        # Read and decrypt the 8-byte footer: INFO_SIZE + VER
        f.seek(-8, 2)
        raw_footer = f.read(8)
        footer = raw_footer.translate(DECRYPT_TABLE)
        info_size, ver = struct.unpack('<II', footer)

        if ver != VER:
            raise ValueError(f"Unexpected version {ver}, expected {VER}")

        # Read and decrypt the INFO section
        info_off = file_size - 8 - info_size
        f.seek(info_off)
        raw_info = f.read(info_size)
        info = raw_info.translate(DECRYPT_TABLE)

    pos = 0
    folders_count = struct.unpack_from('<I', info, pos)[0]
    pos += 4

    folders = []
    for _ in range(folders_count):
        namesz = struct.unpack_from('<H', info, pos)[0]
        pos += 2
        folder_name = info[pos:pos + namesz].decode('ascii', errors='replace')
        pos += namesz
        file_count = struct.unpack_from('<I', info, pos)[0]
        pos += 4

        files = []
        for _ in range(file_count):
            fnsz = struct.unpack_from('<H', info, pos)[0]
            pos += 2
            fname = info[pos:pos + fnsz].decode('ascii', errors='replace')
            pos += fnsz
            offset, dummy, zsize, ftype = struct.unpack_from('<IIII', info, pos)
            pos += 16
            timestamp = info[pos:pos + 8]
            pos += 8
            size = struct.unpack_from('<I', info, pos)[0]
            pos += 4
            files.append(FileEntry(fname, offset, dummy, zsize, ftype, timestamp, size))

        folders.append(FolderEntry(folder_name, files))

    return ver, folders


def cmd_list(pak_path, verbose=False):
    """List contents of a pak file."""
    ver, folders = parse_index(pak_path)
    type_names = {1: 'store', 2: 'zlib ', 3: 'dir  '}

    total_files = 0
    total_size = 0
    total_zsize = 0

    print(f"{'Type':5s}  {'Compressed':>12s}  {'Size':>12s}  {'Ratio':>6s}  Name")
    print("-" * 80)

    for folder in folders:
        if verbose:
            print(f"[{folder.name}]")
        for fe in folder.files:
            tname = type_names.get(fe.type, '??? ')
            if fe.type == 3 and not verbose:
                continue
            ratio = f"{fe.zsize / fe.size * 100:.1f}%" if fe.size > 0 else "  -  "
            print(f"{tname}  {fe.zsize:>12,}  {fe.size:>12,}  {ratio:>6s}  {fe.name}")
            total_files += 1
            total_size += fe.size
            total_zsize += fe.zsize

    print("-" * 80)
    print(f"{total_files} files, {total_zsize:,} bytes compressed, {total_size:,} bytes uncompressed")
    print(f"{len(folders)} folders, version {ver}")


def cmd_extract(pak_path, output_dir, verbose=False):
    """Extract all files from a pak archive."""
    ver, folders = parse_index(pak_path)

    total_files = sum(len(f.files) for f in folders)
    extracted = 0

    with open(pak_path, 'rb') as f:
        for folder in folders:
            for fe in folder.files:
                if fe.type == 3:
                    # Folder marker — ensure directory exists
                    dir_path = os.path.join(output_dir, fe.name)
                    os.makedirs(dir_path, exist_ok=True)
                    extracted += 1
                    continue

                out_path = os.path.join(output_dir, fe.name)
                os.makedirs(os.path.dirname(out_path), exist_ok=True)

                extracted += 1
                if verbose or extracted % 50 == 0 or extracted == total_files:
                    print(f"[{extracted}/{total_files}] {fe.name}")

                f.seek(fe.offset)

                if fe.type == 1:
                    # Uncompressed — stream in chunks
                    with open(out_path, 'wb') as out:
                        remaining = fe.size
                        while remaining > 0:
                            chunk = min(CHUNK_SIZE, remaining)
                            raw = f.read(chunk)
                            if not raw:
                                break
                            out.write(raw.translate(DECRYPT_TABLE))
                            remaining -= len(raw)

                elif fe.type == 2:
                    # Zlib compressed — read all compressed bytes, decompress
                    if fe.zsize == 0:
                        data = b''
                    else:
                        raw = f.read(fe.zsize)
                        decrypted = raw.translate(DECRYPT_TABLE)
                        data = zlib.decompress(decrypted)
                    if len(data) != fe.size:
                        print(f"  WARNING: size mismatch for {fe.name}: "
                              f"got {len(data)}, expected {fe.size}", file=sys.stderr)
                    with open(out_path, 'wb') as out:
                        out.write(data)
                else:
                    print(f"  WARNING: unknown type {fe.type} for {fe.name}", file=sys.stderr)

    print(f"Extracted {extracted} entries to {output_dir}")


def cmd_pack(input_dir, output_pak, no_compress=False, verbose=False):
    """Pack a directory into a pak archive."""
    input_dir = os.path.normpath(input_dir)

    # Phase 1: Scan files and build folder groups
    # folder_key (lowercase) -> list of (relative_path_with_fwd_slashes, abs_path)
    folder_groups = {}
    # Track all directory relative paths (for TYPE 3 entries)
    all_dirs = set()

    for dirpath, dirnames, filenames in os.walk(input_dir):
        rel_dir = os.path.relpath(dirpath, input_dir).replace('\\', '/')
        if rel_dir == '.':
            rel_dir_fwd = ''
        else:
            rel_dir_fwd = rel_dir
            all_dirs.add(rel_dir_fwd)

        for fn in filenames:
            abs_path = os.path.join(dirpath, fn)
            if rel_dir_fwd:
                rel_path = rel_dir_fwd + '/' + fn
            else:
                rel_path = fn

            # Folder key is the parent directory, lowercased
            folder_key = rel_dir_fwd.lower() if rel_dir_fwd else '.'
            if folder_key not in folder_groups:
                folder_groups[folder_key] = []
            folder_groups[folder_key].append((rel_path, abs_path))

    # Ensure parent directories that contain only subdirs also get folder groups
    for d in sorted(all_dirs):
        parent = os.path.dirname(d).replace('\\', '/')
        folder_key = parent.lower() if parent else '.'
        if folder_key not in folder_groups:
            folder_groups[folder_key] = []

    # Phase 2: Determine child subdirectories for TYPE 3 entries per folder group
    # For each folder_key, find immediate child directories
    folder_child_dirs = {}
    for d in sorted(all_dirs):
        parent = os.path.dirname(d).replace('\\', '/')
        parent_key = parent.lower() if parent else '.'
        if parent_key not in folder_child_dirs:
            folder_child_dirs[parent_key] = []
        # Use the original-case directory name for the TYPE 3 entry
        folder_child_dirs[parent_key].append(d)

    # Phase 3: Write file data and record metadata
    file_meta = {}  # rel_path -> (offset, zsize, size, type, timestamp_bytes)

    with open(output_pak, 'wb') as f:
        # Write file data sequentially
        total_files = sum(len(files) for files in folder_groups.values())
        written = 0

        for folder_key in sorted(folder_groups.keys()):
            for rel_path, abs_path in folder_groups[folder_key]:
                written += 1
                if verbose or written % 100 == 0:
                    print(f"[{written}/{total_files}] packing {rel_path}")

                file_data = open(abs_path, 'rb').read()
                file_size = len(file_data)

                # Get timestamp
                try:
                    stat = os.stat(abs_path)
                    mtime_ns = int(stat.st_mtime * 10_000_000)
                    filetime = mtime_ns + FILETIME_EPOCH_DIFF
                    timestamp = struct.pack('<Q', filetime)
                except Exception:
                    timestamp = b'\x00' * 8

                # Decide compression
                ext = os.path.splitext(rel_path)[1].lower()
                if no_compress or ext in UNCOMPRESSED_EXTS:
                    # Type 1: uncompressed
                    offset = f.tell()
                    f.write(file_data.translate(ENCRYPT_TABLE))
                    file_meta[rel_path] = (offset, file_size, file_size, 1, timestamp)
                else:
                    # Type 2: zlib compressed
                    compressed = zlib.compress(file_data)
                    offset = f.tell()
                    f.write(compressed.translate(ENCRYPT_TABLE))
                    file_meta[rel_path] = (offset, len(compressed), file_size, 2, timestamp)

        # Phase 4: Build INFO section
        info = bytearray()

        # Count actual folder entries we'll write
        all_folder_keys = sorted(folder_groups.keys())
        info += struct.pack('<I', len(all_folder_keys))

        for folder_key in all_folder_keys:
            # Folder name
            folder_name_bytes = folder_key.encode('ascii')
            info += struct.pack('<H', len(folder_name_bytes))
            info += folder_name_bytes

            # Collect entries for this folder: files + TYPE 3 subdirs
            entries = []

            # TYPE 3 child directory entries
            for child_dir in folder_child_dirs.get(folder_key, []):
                entries.append(FileEntry(
                    name=child_dir,
                    offset=0,
                    dummy=0,
                    zsize=0,
                    type=3,
                    timestamp=b'\x00' * 8,
                    size=0,
                ))

            # Regular file entries
            for rel_path, abs_path in folder_groups[folder_key]:
                offset, zsize, size, ftype, timestamp = file_meta[rel_path]
                entries.append(FileEntry(
                    name=rel_path,
                    offset=offset,
                    dummy=0,
                    zsize=zsize,
                    type=ftype,
                    timestamp=timestamp,
                    size=size,
                ))

            info += struct.pack('<I', len(entries))

            for entry in entries:
                name_bytes = entry.name.encode('ascii')
                info += struct.pack('<H', len(name_bytes))
                info += name_bytes
                info += struct.pack('<IIII', entry.offset, entry.dummy, entry.zsize, entry.type)
                info += entry.timestamp
                info += struct.pack('<I', entry.size)

        # Write encrypted INFO section
        info_size = len(info)
        f.write(bytes(info).translate(ENCRYPT_TABLE))

        # Write footer: INFO_SIZE + VER
        footer = struct.pack('<II', info_size, VER)
        f.write(footer.translate(ENCRYPT_TABLE))

    print(f"Packed {total_files} files into {output_pak} ({os.path.getsize(output_pak):,} bytes)")


def main():
    parser = argparse.ArgumentParser(
        description='Extract and repack .pak files for Codename: Panzers Cold War')
    parser.add_argument('-v', '--verbose', action='store_true')
    sub = parser.add_subparsers(dest='command', required=True)

    p_list = sub.add_parser('list', help='List archive contents')
    p_list.add_argument('pak_file')

    p_extract = sub.add_parser('extract', help='Extract archive contents')
    p_extract.add_argument('pak_file')
    p_extract.add_argument('output_dir')

    p_pack = sub.add_parser('pack', help='Pack directory into archive')
    p_pack.add_argument('input_dir')
    p_pack.add_argument('output_pak')
    p_pack.add_argument('--no-compress', action='store_true',
                        help='Store all files uncompressed')

    args = parser.parse_args()

    if args.command == 'list':
        cmd_list(args.pak_file, args.verbose)
    elif args.command == 'extract':
        cmd_extract(args.pak_file, args.output_dir, args.verbose)
    elif args.command == 'pack':
        cmd_pack(args.input_dir, args.output_pak, args.no_compress, args.verbose)


if __name__ == '__main__':
    main()
