"""Sizes and encoding errors of a relit level's lightmaps if stored compressed or at half
size, and a contact sheet of them decoded to light (gh-60, 2026-10-30).

    python tools/night_report.py <folder of the relighter DDS> <contact sheet .png>

The lightmaps are A8R8G8B8 with mips; what is stored is the square root of the light's
share of eight whites (Relighter.cpp), so light (1 = white) is 8 * stored^2.
"""

import io
import os
import sys

import numpy as np
from PIL import Image

HEADER = 128


def read_base(path):
    data = open(path, "rb").read()
    h = int.from_bytes(data[12:16], "little")
    w = int.from_bytes(data[16:20], "little")
    mips = int.from_bytes(data[28:32], "little")
    raw = np.frombuffer(data, np.uint8, w * h * 4, HEADER).reshape(h, w, 4)
    # A8R8G8B8 little endian: bytes are B, G, R, A
    return w, h, mips, raw[:, :, [2, 1, 0, 3]].copy(), len(data)


def mip_bytes(w, h, mips, per_block, block=True):
    total = 0
    for _ in range(max(mips, 1)):
        total += (
            (max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * per_block)
            if block
            else w * h * per_block
        )
        w, h = max(1, w // 2), max(1, h // 2)
    return total


def roundtrip(rgba, fmt):
    img = Image.fromarray(rgba, "RGBA")
    buf = io.BytesIO()
    img.save(buf, format="DDS", pixel_format=fmt)
    buf.seek(0)
    back = np.array(Image.open(buf).convert("RGBA"))
    return back, len(buf.getvalue())


def decode(stored):
    s = stored.astype(np.float32) / 255.0
    return 8.0 * s * s


def tonemap(light, exposure):
    x = light * exposure
    x = x / (1.0 + x)
    return (np.clip(x, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)


def main(folder, sheet):
    files = sorted(f for f in os.listdir(folder) if f.lower().endswith(".dds"))
    tot = {"raw": 0, "dxt1": 0, "dxt5": 0, "half": 0, "half_dxt1": 0}
    rows = []
    maps = []
    for name in files:
        w, h, mips, rgba, size = read_base(os.path.join(folder, name))
        tot["raw"] += size
        tot["dxt1"] += HEADER + mip_bytes(w, h, mips, 8)
        tot["dxt5"] += HEADER + mip_bytes(w, h, mips, 16)
        tot["half"] += HEADER + mip_bytes(
            max(1, w // 2), max(1, h // 2), max(mips - 1, 1), 4, block=False
        )
        tot["half_dxt1"] += HEADER + mip_bytes(
            max(1, w // 2), max(1, h // 2), max(mips - 1, 1), 8
        )
        stored = rgba[:, :, :3]
        ref = decode(stored)
        out = [name, w, h]
        for fmt in ("DXT1", "DXT5"):
            back, _ = roundtrip(rgba, fmt)
            got = decode(back[:, :, :3])
            out += [
                float(np.abs(back[:, :, :3].astype(float) - stored).mean())
                / 255.0,  # stored-domain MAE
                float(np.abs(got - ref).mean()),  # light-domain MAE
                float(ref.mean()),
            ]
        maps.append((name, ref))
        rows.append(out)
    print("totals (bytes, headers included):", tot)
    print(
        "name w h | DXT1 stored-MAE light-MAE mean-light | DXT5 stored-MAE light-MAE mean-light"
    )
    for r in rows:
        print(
            r[0],
            r[1],
            r[2],
            "|",
            " ".join(f"{v:.5f}" for v in r[3:6]),
            "|",
            " ".join(f"{v:.5f}" for v in r[6:9]),
        )
    allmean = np.mean([m.mean() for _, m in maps])
    # Contact sheet: every map in a 5-wide grid at 128 px, one exposure for all.
    tile = 128
    cols = 5
    rows_n = (len(maps) + cols - 1) // cols
    canvas = Image.new("RGB", (cols * tile, rows_n * tile), (20, 20, 20))
    exposure = 0.5 / max(allmean, 1e-6)
    for i, (_, light) in enumerate(maps):
        t = Image.fromarray(tonemap(light, exposure), "RGB").resize(
            (tile, tile), Image.BILINEAR
        )
        canvas.paste(t, ((i % cols) * tile, (i // cols) * tile))
    canvas.save(sheet)
    print("mean light over all maps", allmean, "exposure", exposure, "sheet", sheet)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
