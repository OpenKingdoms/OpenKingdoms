#!/usr/bin/env python3
"""
HPI Archive Extractor for Total Annihilation: Kingdoms
Based on the HPI format specification by Joe D (joed@cws.org)
Supports HPI v1 (TA/TA:CC) and HPI v2 (TA:Kingdoms)

Usage:
    python hpi_extract.py <file.hpi> [-o output_dir] [-l] [-v]
    python hpi_extract.py --all <game_dir> [-o output_dir]

Options:
    -o   Output directory (default: current directory)
    -l   List files only, don't extract
    -v   Verbose output
    --all  Extract all .hpi files from a game directory
"""

import struct
import sys
import os
import zlib
import argparse
import time
from pathlib import Path

# Constants
HEX_HAPI = 0x49504148  # 'HAPI'
HEX_BANK = 0x4B4E4142  # 'BANK'
HEX_SQSH = 0x48535153  # 'SQSH'

VERSION_TA = 0x00010000
VERSION_TAK = 0x00020000


class HPIExtractor:
    """Extracts files from HPI archives (v1 and v2)."""

    def __init__(self, filepath, verbose=False):
        self.filepath = filepath
        self.verbose = verbose
        self.f = None
        self.key = 0
        self.file_list = []

    def open(self):
        self.f = open(self.filepath, 'rb')
        marker, version = struct.unpack('<II', self.f.read(8))
        if marker != HEX_HAPI:
            raise ValueError(f"Not an HPI file: {self.filepath}")
        if version == HEX_BANK:
            raise ValueError(f"TA save game file, not extractable: {self.filepath}")
        self.version = version
        return version

    def close(self):
        if self.f:
            self.f.close()
            self.f = None

    # --- HPI v1 (TA) methods ---

    def _read_and_decrypt(self, fpos, size):
        """Read and decrypt data from an HPI v1 file."""
        self.f.seek(fpos)
        data = bytearray(self.f.read(size))
        if self.key:
            for i in range(len(data)):
                tkey = (fpos + i) ^ self.key
                data[i] = tkey ^ (~data[i] & 0xFF)
        return bytes(data)

    def _lz77_decompress(self, data, encrypted):
        """LZ77 decompression used by HPI chunks."""
        data = bytearray(data)
        if encrypted:
            for i in range(len(data)):
                data[i] = ((data[i] - i) ^ i) & 0xFF

        out = bytearray()
        window = bytearray(4096)
        win_pos = 1
        in_pos = 0
        mask = 1
        tag = data[in_pos]
        in_pos += 1

        while in_pos < len(data):
            if (mask & tag) == 0:
                # Literal byte
                if in_pos >= len(data):
                    break
                b = data[in_pos]
                in_pos += 1
                out.append(b)
                window[win_pos] = b
                win_pos = (win_pos + 1) & 0xFFF
            else:
                # Reference pair
                if in_pos + 1 >= len(data):
                    break
                count = data[in_pos] | (data[in_pos + 1] << 8)
                in_pos += 2
                buf_pos = count >> 4
                if buf_pos == 0:
                    return bytes(out)
                length = (count & 0x0F) + 2
                for _ in range(length):
                    b = window[buf_pos]
                    out.append(b)
                    window[win_pos] = b
                    buf_pos = (buf_pos + 1) & 0xFFF
                    win_pos = (win_pos + 1) & 0xFFF

            mask <<= 1
            if mask & 0x100:
                mask = 1
                if in_pos >= len(data):
                    break
                tag = data[in_pos]
                in_pos += 1

        return bytes(out)

    def _zlib_decompress(self, data, encrypted):
        """ZLib decompression used by HPI chunks."""
        data = bytearray(data)
        if encrypted:
            for i in range(len(data)):
                data[i] = ((data[i] - i) ^ i) & 0xFF
        return zlib.decompress(bytes(data))

    def _decompress_chunk(self, chunk_data):
        """Decompress an SQSH chunk."""
        if len(chunk_data) < 18:
            return chunk_data

        marker = struct.unpack('<I', chunk_data[0:4])[0]
        if marker != HEX_SQSH:
            return chunk_data

        unknown1 = chunk_data[4]
        comp_method = chunk_data[5]
        encrypt = chunk_data[6]
        comp_size = struct.unpack('<I', chunk_data[7:11])[0]
        decomp_size = struct.unpack('<I', chunk_data[11:15])[0]
        checksum = struct.unpack('<I', chunk_data[15:19])[0]
        data = chunk_data[19:19 + comp_size]

        if self.verbose:
            print(f"    Chunk: method={comp_method} encrypt={encrypt} "
                  f"comp={comp_size} decomp={decomp_size}")

        if comp_method == 1:
            return self._lz77_decompress(data, encrypt)
        elif comp_method == 2:
            return self._zlib_decompress(data, encrypt)
        else:
            raise ValueError(f"Unknown compression method: {comp_method}")

    def _process_file_v1(self, data_offset, file_size, file_flag):
        """Extract a file from HPI v1 archive."""
        if file_flag == 0:
            # Not compressed
            return self._read_and_decrypt(data_offset, file_size)

        # Compressed - read chunk sizes
        num_chunks = file_size // 65536
        if file_size % 65536:
            num_chunks += 1

        chunk_sizes_data = self._read_and_decrypt(
            data_offset, num_chunks * 4)
        chunk_sizes = struct.unpack(f'<{num_chunks}I', chunk_sizes_data)
        offset = data_offset + num_chunks * 4

        result = bytearray()
        for size in chunk_sizes:
            chunk_data = self._read_and_decrypt(offset, size)
            result.extend(self._decompress_chunk(chunk_data))
            offset += size

        return bytes(result)

    def _traverse_v1(self, directory, offset, path=""):
        """Recursively traverse the HPI v1 directory tree."""
        num_entries = struct.unpack('<I', directory[offset:offset + 4])[0]
        entry_offset_val = struct.unpack('<I', directory[offset + 4:offset + 8])[0]

        for i in range(num_entries):
            entry_pos = entry_offset_val + i * 9
            name_offset = struct.unpack('<I', directory[entry_pos:entry_pos + 4])[0]
            data_offset = struct.unpack('<I', directory[entry_pos + 4:entry_pos + 8])[0]
            flag = directory[entry_pos + 8]

            # Read null-terminated name
            name_end = directory.index(b'\x00', name_offset)
            name = directory[name_offset:name_end].decode('ascii', errors='replace')

            full_path = f"{path}\\{name}" if path else name

            if flag == 1:
                # Subdirectory
                self._traverse_v1(directory, data_offset, full_path)
            else:
                # File
                file_data_offset = struct.unpack('<I', directory[data_offset:data_offset + 4])[0]
                file_size = struct.unpack('<I', directory[data_offset + 4:data_offset + 8])[0]
                file_flag = directory[data_offset + 8]
                self.file_list.append({
                    'path': full_path.replace('\\', '/'),
                    'offset': file_data_offset,
                    'size': file_size,
                    'flag': file_flag,
                    'version': 1,
                })

    def _extract_v1(self):
        """Extract all files from an HPI v1 archive."""
        dir_size, header_key, start = struct.unpack('<III', self.f.read(12))

        if header_key:
            self.key = ~((header_key * 4) | (header_key >> 6)) & 0xFFFFFFFF
        else:
            self.key = 0

        directory = bytearray(dir_size)
        decrypted = self._read_and_decrypt(start, dir_size - start)
        directory[start:] = decrypted

        self.file_list = []
        self._traverse_v1(bytes(directory), start)

    # --- HPI v2 (TAK) methods ---

    def _read_block_v2(self, offset, size):
        """Read a block from HPI v2, decompressing if needed."""
        self.f.seek(offset)
        data = self.f.read(size)

        # Check if compressed (starts with SQSH)
        if len(data) >= 4 and struct.unpack('<I', data[0:4])[0] == HEX_SQSH:
            return self._decompress_chunk(data)
        return data

    def _traverse_v2(self, dir_data, name_data, dir_offset=0, path=""):
        """Recursively traverse HPI v2 directory tree."""
        # Read HPIDIR2 structure (20 bytes)
        pos = dir_offset
        name_ptr = struct.unpack('<I', dir_data[pos:pos + 4])[0]
        first_subdir = struct.unpack('<I', dir_data[pos + 4:pos + 8])[0]
        sub_count = struct.unpack('<I', dir_data[pos + 8:pos + 12])[0]
        first_file = struct.unpack('<I', dir_data[pos + 12:pos + 16])[0]
        file_count = struct.unpack('<I', dir_data[pos + 16:pos + 20])[0]

        # Read directory name
        name_end = name_data.index(b'\x00', name_ptr)
        dir_name = name_data[name_ptr:name_end].decode('ascii', errors='replace')
        full_path = f"{path}{dir_name}/" if path or dir_name else ""

        # Process subdirectories
        for i in range(sub_count):
            sub_offset = first_subdir + i * 20  # sizeof(HPIDIR2) = 20
            self._traverse_v2(dir_data, name_data, sub_offset, full_path)

        # Process files
        for i in range(file_count):
            entry_offset = first_file + i * 24  # sizeof(HPIENTRY2) = 24
            e_name_ptr = struct.unpack('<I', dir_data[entry_offset:entry_offset + 4])[0]
            e_start = struct.unpack('<I', dir_data[entry_offset + 4:entry_offset + 8])[0]
            e_decomp_size = struct.unpack('<I', dir_data[entry_offset + 8:entry_offset + 12])[0]
            e_comp_size = struct.unpack('<I', dir_data[entry_offset + 12:entry_offset + 16])[0]
            e_date = struct.unpack('<I', dir_data[entry_offset + 16:entry_offset + 20])[0]
            e_checksum = struct.unpack('<I', dir_data[entry_offset + 20:entry_offset + 24])[0]

            fname_end = name_data.index(b'\x00', e_name_ptr)
            fname = name_data[e_name_ptr:fname_end].decode('ascii', errors='replace')

            self.file_list.append({
                'path': f"{full_path}{fname}",
                'offset': e_start,
                'decompressed_size': e_decomp_size,
                'compressed_size': e_comp_size,
                'date': e_date,
                'checksum': e_checksum,
                'version': 2,
            })

    def _process_file_v2(self, entry):
        """Extract a file from HPI v2 archive."""
        self.f.seek(entry['offset'])

        if entry['compressed_size'] == 0:
            # Not compressed
            return self.f.read(entry['decompressed_size'])

        # Compressed - read SQSH chunks until we have all data
        result = bytearray()
        while len(result) < entry['decompressed_size']:
            # Read chunk header
            chunk_header = self.f.read(19)
            if len(chunk_header) < 19:
                break

            marker = struct.unpack('<I', chunk_header[0:4])[0]
            if marker != HEX_SQSH:
                raise ValueError(f"Expected SQSH marker, got 0x{marker:08X}")

            comp_method = chunk_header[5]
            encrypt = chunk_header[6]
            comp_size = struct.unpack('<I', chunk_header[7:11])[0]
            decomp_size = struct.unpack('<I', chunk_header[11:15])[0]

            chunk_data = self.f.read(comp_size)

            if comp_method == 1:
                result.extend(self._lz77_decompress(chunk_data, encrypt))
            elif comp_method == 2:
                result.extend(self._zlib_decompress(chunk_data, encrypt))
            else:
                raise ValueError(f"Unknown compression: {comp_method}")

        return bytes(result[:entry['decompressed_size']])

    def _extract_v2(self):
        """Parse the directory of an HPI v2 archive."""
        header = self.f.read(24)
        dir_block, dir_size, name_block, name_size, data_start, last78 = \
            struct.unpack('<IIIIII', header)

        dir_data = self._read_block_v2(dir_block, dir_size)
        name_data = self._read_block_v2(name_block, name_size)

        self.file_list = []
        self._traverse_v2(dir_data, name_data)

    # --- Public API ---

    def scan(self):
        """Scan the archive and build file list."""
        version = self.open()
        if version == VERSION_TA:
            self._extract_v1()
        elif version == VERSION_TAK:
            self._extract_v2()
        else:
            raise ValueError(f"Unknown HPI version: 0x{version:08X}")
        return self.file_list

    def extract_all(self, output_dir):
        """Extract all files to output_dir."""
        if not self.file_list:
            self.scan()

        extracted = 0
        errors = 0
        for entry in self.file_list:
            rel_path = entry['path'].replace('/', os.sep)
            out_path = os.path.join(output_dir, rel_path)
            os.makedirs(os.path.dirname(out_path), exist_ok=True)

            try:
                if entry['version'] == 1:
                    data = self._process_file_v1(
                        entry['offset'], entry['size'], entry['flag'])
                else:
                    data = self._process_file_v2(entry)

                with open(out_path, 'wb') as out_f:
                    out_f.write(data)

                # Restore file date if available
                if entry.get('date') and entry['date'] > 0:
                    try:
                        os.utime(out_path, (entry['date'], entry['date']))
                    except (OSError, OverflowError):
                        pass

                extracted += 1
                if self.verbose:
                    size = entry.get('decompressed_size', entry.get('size', 0))
                    print(f"  {rel_path} ({size:,} bytes)")
            except Exception as e:
                errors += 1
                print(f"  ERROR extracting {rel_path}: {e}")

        return extracted, errors

    def list_files(self):
        """Print the file listing."""
        if not self.file_list:
            self.scan()

        # Collect stats by extension
        ext_stats = {}
        total_size = 0

        for entry in self.file_list:
            size = entry.get('decompressed_size', entry.get('size', 0))
            total_size += size
            ext = os.path.splitext(entry['path'])[1].lower()
            if ext not in ext_stats:
                ext_stats[ext] = {'count': 0, 'size': 0}
            ext_stats[ext]['count'] += 1
            ext_stats[ext]['size'] += size

            date_str = ""
            if entry.get('date') and entry['date'] > 0:
                try:
                    date_str = time.strftime('%Y-%m-%d', time.localtime(entry['date']))
                except (OSError, OverflowError):
                    date_str = "?"
            print(f"  {size:>10,}  {date_str:>10}  {entry['path']}")

        print(f"\n  Total: {len(self.file_list)} files, {total_size:,} bytes")
        print(f"\n  By extension:")
        for ext, stats in sorted(ext_stats.items(), key=lambda x: -x[1]['size']):
            print(f"    {ext or '(none)':>8}  {stats['count']:>5} files  {stats['size']:>12,} bytes")


def extract_all_hpi(game_dir, output_dir, verbose=False, list_only=False,
                    skip_iron_plague=True):
    """Extract all HPI files from a game directory."""
    game_path = Path(game_dir)
    hpi_files = sorted(game_path.glob('*.hpi'))

    if skip_iron_plague:
        # Filter out Iron Plague expansion files
        ip_prefixes = ('ip', 'ironplague')
        hpi_files = [f for f in hpi_files
                     if not f.stem.lower().startswith(ip_prefixes)]

    print(f"Found {len(hpi_files)} HPI files in {game_dir}")
    if skip_iron_plague:
        print("(Skipping Iron Plague expansion files)")
    print()

    total_extracted = 0
    total_errors = 0

    for hpi_file in hpi_files:
        print(f"{'=' * 60}")
        print(f"Archive: {hpi_file.name}")
        print(f"{'=' * 60}")

        try:
            extractor = HPIExtractor(str(hpi_file), verbose=verbose)
            extractor.scan()

            version_str = "v1 (TA)" if extractor.version == VERSION_TA else "v2 (TAK)"
            print(f"  Format: HPI {version_str}")
            print(f"  Files: {len(extractor.file_list)}")

            if list_only:
                extractor.list_files()
            else:
                # Extract into a subdirectory named after the HPI file
                hpi_output = os.path.join(output_dir, hpi_file.stem)
                os.makedirs(hpi_output, exist_ok=True)
                extracted, errors = extractor.extract_all(hpi_output)
                total_extracted += extracted
                total_errors += errors
                print(f"  Extracted: {extracted} files, {errors} errors")

            extractor.close()
        except Exception as e:
            print(f"  ERROR: {e}")
            total_errors += 1

        print()

    print(f"{'=' * 60}")
    print(f"DONE: {total_extracted} files extracted, {total_errors} errors")
    return total_extracted, total_errors


def main():
    parser = argparse.ArgumentParser(
        description='HPI Archive Extractor for Total Annihilation: Kingdoms')
    parser.add_argument('input', help='HPI file or game directory (with --all)')
    parser.add_argument('-o', '--output', default='.',
                        help='Output directory (default: current dir)')
    parser.add_argument('-l', '--list', action='store_true',
                        help='List files only, don\'t extract')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Verbose output')
    parser.add_argument('--all', action='store_true',
                        help='Extract all .hpi files from game directory')
    parser.add_argument('--include-ip', action='store_true',
                        help='Include Iron Plague expansion files')
    args = parser.parse_args()

    if args.all:
        extract_all_hpi(
            args.input, args.output,
            verbose=args.verbose, list_only=args.list,
            skip_iron_plague=not args.include_ip)
    else:
        extractor = HPIExtractor(args.input, verbose=args.verbose)
        extractor.scan()

        version_str = "v1 (TA)" if extractor.version == VERSION_TA else "v2 (TAK)"
        print(f"Archive: {args.input}")
        print(f"Format: HPI {version_str}")
        print(f"Files: {len(extractor.file_list)}")
        print()

        if args.list:
            extractor.list_files()
        else:
            os.makedirs(args.output, exist_ok=True)
            extracted, errors = extractor.extract_all(args.output)
            print(f"\nExtracted: {extracted} files, {errors} errors")

        extractor.close()


if __name__ == '__main__':
    main()
