# /// script
# dependencies = ["cryptography"]
# ///
"""Extract single files from Nightreign's dataN.bhd/.bdt archives.

    uv run bhd_extract.py <Game dir> <ArchiveKeys.cs from Smithbox> <out dir> /script/talk/m10_00_00_00.talkesdbnd.dcx [...]

Format and public keys come from the open-source Smithbox / SoulsFormats (GPL-3.0).
"""
import re, struct, sys
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.serialization import load_pem_public_key


def keys(cs_path):
    """Archive name -> (n, e) for the NightreignKeys table."""
    block = Path(cs_path).read_text().split("NightreignKeys = new")[1]
    out = {}
    for name, pem in re.findall(r'\["([^"]+)"\]\s*=\s*@"(.*?)"', block, re.S):
        nums = load_pem_public_key(pem.encode()).public_numbers()
        out[name.replace("\\\\", "/")] = (nums.n, nums.e)
    return out


def rsa_decrypt(data, n, e):
    in_s, out_s = (n.bit_length() + 7) // 8, (n.bit_length() - 1) // 8
    return b"".join(pow(int.from_bytes(data[i:i + in_s], "big"), e, n).to_bytes(out_s, "big")
                    for i in range(0, len(data) - len(data) % in_s, in_s))


def path_hash(path):
    h = 0
    for c in path.lower().replace("\\", "/"):
        h = (h * 0x85 + ord(c)) & 0xFFFFFFFFFFFFFFFF
    return h


def lookup(bhd, want):
    """Return (padded size, size, offset, aes key, ranges) for a path hash, or None."""
    assert bhd[:4] == b"BHD5", bhd[:4]
    bucket_count, buckets_off = struct.unpack_from("<ii", bhd, 0x10)
    count, off = struct.unpack_from("<ii", bhd, buckets_off + (want % bucket_count) * 8)
    for i in range(count):
        h, padded, size, file_off, _sha_off, aes_off = struct.unpack_from("<Qiiqqq", bhd, off + i * 0x28)
        if h != want:
            continue
        key, ranges = None, []
        if aes_off:
            key = bhd[aes_off:aes_off + 16]
            n, = struct.unpack_from("<i", bhd, aes_off + 16)
            ranges = [struct.unpack_from("<qq", bhd, aes_off + 20 + j * 16) for j in range(n)]
        return padded, size, file_off, key, ranges
    return None


def main(game_dir, keys_cs, out_dir, *paths):
    game, out = Path(game_dir), Path(out_dir)
    headers = {}
    for name, (n, e) in keys(keys_cs).items():
        if not (game / f"{name}.bhd").exists():
            continue
        try:
            headers[name] = rsa_decrypt((game / f"{name}.bhd").read_bytes(), n, e)
            assert headers[name][:4] == b"BHD5"
        except (OverflowError, AssertionError):  # key does not match this archive (game updated?)
            headers.pop(name, None)
            print(f"skipping {name}: known key does not decrypt it")
    for path in paths:
        for name, bhd in headers.items():
            hit = lookup(bhd, path_hash(path))
            if not hit:
                continue
            padded, size, off, key, ranges = hit
            with open(game / f"{name}.bdt", "rb") as f:
                f.seek(off)
                data = bytearray(f.read(padded))
            if key:
                dec = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
                for start, end in ranges:
                    if start != -1 and end != -1 and start != end:
                        data[start:end] = dec.update(bytes(data[start:end]))
            dest = out / path.lstrip("/")
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data[:size] if size else data)
            print(f"{path}: {name}, {len(data[:size] if size else data)} bytes, magic {bytes(data[:4])}")
            break
        else:
            print(f"{path}: NOT FOUND")


if __name__ == "__main__":
    main(*sys.argv[1:])
