# CPCW .pak Archive Format

Codename: Panzers Cold War archive format used for `main1.pak`, `main2.pak`, `enUS.pak`.

## Encryption

Every byte in the file is rotated:
- **Decrypt:** `plain = (encrypted + 0xBD) & 0xFF`
- **Encrypt:** `encrypted = (plain - 0xBD) & 0xFF`

All offsets and fields below describe the **decrypted** data.

## File Layout

```
[File Data Blocks] [INFO Section (INFO_SIZE bytes)] [INFO_SIZE: u32 LE] [VER: u32 LE]
```

| Region | Description |
|--------|-------------|
| File Data | Concatenated file contents (compressed or raw), at arbitrary offsets |
| INFO Section | Directory of folders and files |
| INFO_SIZE | 4 bytes, little-endian u32 — size of INFO section in bytes |
| VER | 4 bytes, little-endian u32 — format version, always `3` |

## Reading the Footer

```
seek(file_size - 4):  read VER      (u32 LE) = 3
seek(file_size - 8):  read INFO_SIZE (u32 LE)
INFO_OFF = file_size - 8 - INFO_SIZE
```

## INFO Section Structure

Starting at `INFO_OFF`:

```
FOLDERS_COUNT: u32 LE

For each folder (FOLDERS_COUNT times):
    FOLDER_NAME_LEN: u16 LE
    FOLDER_NAME:     bytes[FOLDER_NAME_LEN]   (lowercase, e.g. "maps/teammatch" or "." for root)
    FILE_COUNT:      u32 LE

    For each file (FILE_COUNT times):
        FILE_NAME_LEN: u16 LE
        FILE_NAME:     bytes[FILE_NAME_LEN]   (original case, e.g. "Maps/TeamMatch/file.tga")
        OFFSET:        u32 LE                 (absolute byte offset in archive to file data)
        DUMMY:         u32 LE                 (always 0)
        ZSIZE:         u32 LE                 (compressed size; equals SIZE for type 1)
        TYPE:          u32 LE                 (see below)
        TIMESTAMP:     8 bytes                (Windows FILETIME, 100ns ticks since 1601-01-01)
        SIZE:          u32 LE                 (uncompressed size)
```

## File Types

| Type | Meaning | Storage |
|------|---------|---------|
| 1 | Uncompressed | Raw bytes at OFFSET, ZSIZE == SIZE |
| 2 | Zlib compressed | Standard zlib (header `78 9C`, wbits=15) at OFFSET |
| 3 | Directory marker | No data (OFFSET may be non-zero, ZSIZE=0, SIZE=0) |

## Folder Grouping

- Folder names are **lowercase** versions of the path (e.g. `maps/teammatch`)
- The root folder is named `"."`
- Each folder group contains regular file entries (type 1/2) **and** type 3 entries for immediate child subdirectories
- File names within entries preserve original case

## Timestamps

Windows FILETIME format: 64-bit little-endian count of 100-nanosecond intervals since January 1, 1601 UTC.

Convert to Unix timestamp: `unix_seconds = (filetime - 116444736000000000) / 10000000`

## Observed Values

| File | Size | Folders | Files | INFO_SIZE |
|------|------|---------|-------|-----------|
| main1.pak | 1.79 GB | 8 | 227 | 13,558 |
| main2.pak | 1.81 GB | 907 | ~22,000 | 1,822,077 |
| enUS.pak | 606 MB | varies | varies | varies |

## Compression Details

- Type 2 files use standard zlib with default compression level
- Compressed data starts with bytes `78 9C` (zlib default)
- Large media files (.avi, .wav, .ogg) are stored as type 1 (uncompressed)
- Smaller assets (.map, .tga, .mat, .xml) are stored as type 2 (zlib)

## Notes

- The field read at `file_size - 12` in the QuickBMS script overlaps with the last 4 bytes of the INFO section (typically the SIZE field of the last file entry). It is not a separate footer field.
- Maximum file/archive size is limited by u32 offsets (~4 GB).
