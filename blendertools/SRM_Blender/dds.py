"""Minimal DDS texture decoder (stdlib only, no PIL).

Decodes the top mip of DXT1 / DXT3 / DXT5 (and simple uncompressed RGB/RGBA)
DirectDraw Surface files to a flat RGBA byte buffer, so textures can be loaded
into Blender's bundled Python which has no Pillow.

Usage:
    img = DDS.read(path)          # -> DDS with .width/.height/.rgba (bytes, R,G,B,A per px, top-down)
    rgba = img.rgba
"""

import struct

DDSD_MAGIC = b'DDS '

# ddspf.dwFlags
DDPF_FOURCC = 0x4
DDPF_RGB = 0x40
DDPF_ALPHAPIXELS = 0x1

FOURCC_DXT1 = b'DXT1'
FOURCC_DXT2 = b'DXT2'
FOURCC_DXT3 = b'DXT3'
FOURCC_DXT4 = b'DXT4'
FOURCC_DXT5 = b'DXT5'


class DDS:
    __slots__ = ('width', 'height', 'rgba')

    def __init__(self, width, height, rgba):
        self.width = width
        self.height = height
        self.rgba = rgba  # bytearray, len = width*height*4, row-major top-down

    @staticmethod
    def read(path):
        with open(path, 'rb') as f:
            data = f.read()
        return DDS.from_bytes(data)

    @staticmethod
    def from_bytes(data):
        if data[:4] != DDSD_MAGIC:
            raise ValueError("Not a DDS file")
        height = struct.unpack_from('<I', data, 12)[0]
        width = struct.unpack_from('<I', data, 16)[0]
        pf_flags = struct.unpack_from('<I', data, 80)[0]
        fourcc = data[84:88]
        rgb_bit_count = struct.unpack_from('<I', data, 88)[0]
        rmask, gmask, bmask, amask = struct.unpack_from('<IIII', data, 92)

        pixel_off = 128  # magic(4) + 124-byte header

        if pf_flags & DDPF_FOURCC:
            if fourcc == FOURCC_DXT1:
                rgba = _decode_dxt(data, pixel_off, width, height, dxt1=True)
            elif fourcc in (FOURCC_DXT2, FOURCC_DXT3):
                rgba = _decode_dxt3(data, pixel_off, width, height)
            elif fourcc in (FOURCC_DXT4, FOURCC_DXT5):
                rgba = _decode_dxt5(data, pixel_off, width, height)
            else:
                raise ValueError(f"Unsupported DDS fourCC {fourcc!r}")
        elif pf_flags & DDPF_RGB:
            rgba = _decode_uncompressed(data, pixel_off, width, height,
                                        rgb_bit_count, rmask, gmask, bmask,
                                        amask, pf_flags)
        else:
            raise ValueError("Unsupported DDS pixel format")

        return DDS(width, height, rgba)


# --- block helpers ---

def _rgb565(c):
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def _lerp(c0, c1, w0, w1):
    t = w0 + w1
    return ((c0[0] * w0 + c1[0] * w1) // t,
            (c0[1] * w0 + c1[1] * w1) // t,
            (c0[2] * w0 + c1[2] * w1) // t)


def _color_palette(data, off, punchthrough):
    """Return (palette[4] rgb, alpha[4]) for a DXT color block at off."""
    c0_raw, c1_raw = struct.unpack_from('<HH', data, off)
    c0 = _rgb565(c0_raw)
    c1 = _rgb565(c1_raw)
    if punchthrough and c0_raw <= c1_raw:
        pal = [c0, c1, _lerp(c0, c1, 1, 1), (0, 0, 0)]
        alpha = [255, 255, 255, 0]
    else:
        pal = [c0, c1, _lerp(c0, c1, 2, 1), _lerp(c0, c1, 1, 2)]
        alpha = [255, 255, 255, 255]
    return pal, alpha


def _blit(out, width, height, bx, by, pal, color_idx, alpha):
    """Write a decoded 4x4 color block into the RGBA output buffer."""
    for pi in range(16):
        px = bx * 4 + (pi & 3)
        py = by * 4 + (pi >> 2)
        if px >= width or py >= height:
            continue
        ci = (color_idx >> (2 * pi)) & 0x3
        r, g, b = pal[ci]
        o = (py * width + px) * 4
        out[o] = r
        out[o + 1] = g
        out[o + 2] = b
        out[o + 3] = alpha[pi]


def _decode_dxt(data, off, width, height, dxt1):
    out = bytearray(width * height * 4)
    bw = (width + 3) // 4
    bh = (height + 3) // 4
    for by in range(bh):
        for bx in range(bw):
            pal, a = _color_palette(data, off, punchthrough=dxt1)
            color_idx = struct.unpack_from('<I', data, off + 4)[0]
            alpha = [a[(color_idx >> (2 * pi)) & 0x3] for pi in range(16)]
            _blit(out, width, height, bx, by, pal, color_idx, alpha)
            off += 8
    return out


def _decode_dxt3(data, off, width, height):
    out = bytearray(width * height * 4)
    bw = (width + 3) // 4
    bh = (height + 3) // 4
    for by in range(bh):
        for bx in range(bw):
            alpha_bits = int.from_bytes(data[off:off + 8], 'little')
            alpha = [((alpha_bits >> (4 * pi)) & 0xF) * 17 for pi in range(16)]
            pal, _ = _color_palette(data, off + 8, punchthrough=False)
            color_idx = struct.unpack_from('<I', data, off + 12)[0]
            _blit(out, width, height, bx, by, pal, color_idx, alpha)
            off += 16
    return out


def _decode_dxt5(data, off, width, height):
    out = bytearray(width * height * 4)
    bw = (width + 3) // 4
    bh = (height + 3) // 4
    for by in range(bh):
        for bx in range(bw):
            a0 = data[off]
            a1 = data[off + 1]
            ap = [a0, a1]
            if a0 > a1:
                for i in range(2, 8):
                    ap.append((a0 * (8 - i) + a1 * (i - 1)) // 7)
            else:
                for i in range(2, 6):
                    ap.append((a0 * (6 - i) + a1 * (i - 1)) // 5)
                ap.append(0)
                ap.append(255)
            abits = int.from_bytes(data[off + 2:off + 8], 'little')
            alpha = [ap[(abits >> (3 * pi)) & 0x7] for pi in range(16)]
            pal, _ = _color_palette(data, off + 8, punchthrough=False)
            color_idx = struct.unpack_from('<I', data, off + 12)[0]
            _blit(out, width, height, bx, by, pal, color_idx, alpha)
            off += 16
    return out


def _mask_shift(mask):
    if mask == 0:
        return 0, 0
    shift = 0
    m = mask
    while not (m & 1):
        m >>= 1
        shift += 1
    bits = 0
    while m & 1:
        m >>= 1
        bits += 1
    return shift, bits


def _scale(val, bits):
    if bits == 0:
        return 255
    return (val * 255) // ((1 << bits) - 1)


def _decode_uncompressed(data, off, width, height, bpp, rmask, gmask, bmask,
                         amask, pf_flags):
    out = bytearray(width * height * 4)
    bytes_pp = bpp // 8
    rs, rb = _mask_shift(rmask)
    gs, gb = _mask_shift(gmask)
    bs, bb = _mask_shift(bmask)
    as_, ab = _mask_shift(amask)
    has_alpha = bool(pf_flags & DDPF_ALPHAPIXELS) and amask != 0
    for i in range(width * height):
        px = int.from_bytes(data[off:off + bytes_pp], 'little')
        off += bytes_pp
        o = i * 4
        out[o] = _scale((px & rmask) >> rs, rb)
        out[o + 1] = _scale((px & gmask) >> gs, gb)
        out[o + 2] = _scale((px & bmask) >> bs, bb)
        out[o + 3] = _scale((px & amask) >> as_, ab) if has_alpha else 255
    return out
