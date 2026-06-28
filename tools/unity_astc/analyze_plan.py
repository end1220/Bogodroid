#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
"""
Scan a Unity game's texture set ONCE, then analytically compute the VRAM of
every compression plan in a full sweep — read-only, never writes a bundle.

Because resident VRAM of a texture at any ASTC block is pure arithmetic from
(width, height, mips), one metadata scan lets us price the WHOLE grid with no
re-encoding:

  Mode A (block-only, 分辨率不变, 无运行时解码峰值)
      every non-font texture -> ASTC {4x4,6x6,8x8,10x10,12x12}

  Mode B (中段 ASTC + 巨图交引擎运行时 cap=textureMaxDim)
      long side <= 768          -> small block  ∈ {4x4,6x6,8x8}
      768 < long side <= maxsize-> big block    ∈ {4x4,6x6,8x8}
      long side  > maxsize      -> runtime cap  ∈ {384,512,768}
                                   (maxsize paired 1024/1280/2048)
      peak = resident + 巨图一次性 RGBA 解码缓冲 (only Mode B pays this)

Handles multiple resource files: point it at a dir and it aggregates every
data.unity3d / *.bundle / *.assets under it, de-duped by (file, path_id).

Optional --deep PSNR-probes the coarsest acceptable block per texture (adds a
"A 深探测" row that keeps detailed art finer and only pushes tolerant textures
to coarse blocks). Everything else is encode-free.

Usage
-----
    python3 tools/unity_astc/analyze_plan.py /path/to/assets/bin/Data
    python3 tools/unity_astc/analyze_plan.py data.unity3d --budgets 300,700,1100
    python3 tools/unity_astc/analyze_plan.py <dir> --deep --jobs 8

Requirements
------------
    pip install UnityPy Pillow            # astc-encoder-py only for --deep
"""

import argparse
import math
import sys
import time
from pathlib import Path

try:
    import UnityPy
    from UnityPy.enums import TextureFormat as TF
    from PIL import Image, ImageChops, ImageStat
except ImportError as e:
    sys.exit(f"missing dependency: {e}\ninstall:  pip install UnityPy Pillow")

MB = 1048576
RAW_FORMATS = {TF.RGBA32, TF.RGB24, TF.BGRA32, TF.ARGB32}
COMPRESSED_PREFIXES = ("ETC", "DXT", "BC", "PVRTC")


# ── VRAM arithmetic (no encoding) ─────────────────────────────────────────

def astc_bytes(w, h, block, mips):
    bx, by = block
    total = 0
    for _ in range(max(1, mips)):
        total += math.ceil(w / bx) * math.ceil(h / by) * 16
        w, h = max(1, w // 2), max(1, h // 2)
    return total


def astc_block_of(fmt):
    name = TF(fmt).name
    if name.startswith(("ASTC_RGB_", "ASTC_RGBA_")):
        bx, by = name.rsplit("_", 1)[1].split("x")
        return int(bx), int(by)
    return None


def capped_dims(w, h, cap):
    ls = max(w, h)
    if cap <= 0 or ls <= cap:
        return w, h
    s = cap / ls
    return max(1, round(w * s)), max(1, round(h * s))


def area(b):
    return b[0] * b[1]


# ── PSNR for --deep (PIL, no numpy) ───────────────────────────────────────

def psnr(a_rgba, b_rgba):
    rms = ImageStat.Stat(ImageChops.difference(a_rgba, b_rgba)).rms
    mse = sum(r * r for r in rms) / len(rms)
    return 99.0 if mse <= 1e-9 else 10 * math.log10(255 * 255 / mse)


def coarsest_block(img, blocks, thr, probe_max):
    import astc_encoder as ae
    src = img.convert("RGBA")
    if max(src.size) > probe_max:
        s = probe_max / max(src.size)
        src = src.resize((max(1, round(src.size[0] * s)),
                          max(1, round(src.size[1] * s))), Image.BILINEAR)
    w, h = src.size
    raw = src.tobytes("raw", "RGBA")
    sw = ae.ASTCSwizzle.from_str("RGBA")
    for bx, by in blocks:                       # coarse -> fine
        ctx = ae.ASTCContext(ae.ASTCConfig(ae.ASTCProfile.LDR_SRGB, bx, by))
        comp = ctx.compress(ae.ASTCImage(ae.ASTCType.U8, w, h, 1, raw), sw)
        dst = ae.ASTCImage(ae.ASTCType.U8, w, h, 1)
        ctx.decompress(comp, dst, sw)
        if psnr(src, Image.frombytes("RGBA", (w, h), bytes(dst.data))) >= thr:
            return (bx, by)
    return blocks[-1]


# ── texture record ────────────────────────────────────────────────────────

class Tex:
    __slots__ = ("w", "h", "mips", "fmt", "block", "cur", "is_font", "ls",
                 "name", "detected", "obj")

    def __init__(self, obj, t, is_font):
        self.obj = obj
        self.name = t.m_Name
        self.w, self.h = t.m_Width, t.m_Height
        self.mips = t.m_MipCount or 1
        self.fmt = t.m_TextureFormat
        self.block = astc_block_of(t.m_TextureFormat)
        self.is_font = is_font
        self.ls = max(self.w, self.h)
        self.detected = None
        if self.block is not None:
            self.cur = astc_bytes(self.w, self.h, self.block, self.mips)
        elif t.m_TextureFormat in RAW_FORMATS:
            self.cur = self.w * self.h * (3 if t.m_TextureFormat == TF.RGB24 else 4)
        elif TF(self.fmt).name.startswith(COMPRESSED_PREFIXES):
            self.cur = (t.m_CompleteImageSize or self.w * self.h)
        else:
            self.cur = self.w * self.h * 4


def font_keys(env):
    """(assets_file, path_id) of textures referenced by legacy Font objects."""
    keys = set()
    for obj in env.objects:
        if obj.type.name != "Font":
            continue
        try:
            d = obj.read()
        except Exception:
            continue
        tex = getattr(d, "m_Texture", None)
        pid = getattr(tex, "path_id", 0) or getattr(tex, "m_PathID", 0)
        if pid:
            keys.add((obj.assets_file.name, pid))
    return keys


def load_textures(path, log):
    """Aggregate Texture2D across all serialized containers under `path`,
    de-duped by (assets_file, path_id) so multiple resource files / a bundle
    plus its loose .assets never double-count."""
    p = Path(path).resolve()
    if p.is_dir():
        files = sorted({f for pat in ("data.unity3d", "*.bundle", "*.assets",
                                      "*.unity3d", "globalgamemanagers")
                        for f in p.rglob(pat) if f.is_file()})
    elif p.is_file():
        files = [p]
    else:
        sys.exit(f"not found: {p}")
    if not files:
        sys.exit(f"no Unity containers under {p}")

    seen = {}
    stats = []   # (filename, new_texture_count, new_vram_bytes)
    log(f"Analyzing {len(files)} container(s)")
    for f in files:
        t0 = time.time()
        try:
            env = UnityPy.load(str(f))
        except Exception as e:
            log(f"  skip {f.name}: {e}")
            continue
        fk = font_keys(env)
        n = 0
        vram = 0
        for obj in env.objects:
            if obj.type.name != "Texture2D":
                continue
            key = (obj.assets_file.name, obj.path_id)
            if key in seen:
                continue
            try:
                t = obj.read()
            except Exception:
                continue
            is_font = t.m_Name.endswith(" Atlas") or key in fk
            tx = Tex(obj, t, is_font)
            seen[key] = tx
            n += 1
            vram += tx.cur
        stats.append((f.name, n, vram))
        log(f"  {f.name}: +{n} textures ({time.time()-t0:.1f}s)")
    return list(seen.values()), stats


# ── plan pricing (analytic) ────────────────────────────────────────────────

def mode_a(texs, block, keep_cap=256, detected=False):
    """Resident bytes; Mode A keeps fonts + <=keep_cap, ASTCs the rest."""
    tot = 0
    for tx in texs:
        if tx.is_font or tx.ls <= keep_cap:
            tot += tx.cur
        else:
            b = tx.detected if (detected and tx.detected) else block
            if tx.block is not None and area(tx.block) >= area(b):
                tot += tx.cur
            else:
                tot += astc_bytes(tx.w, tx.h, b, tx.mips)
    return tot


def mode_b(texs, small_block, big_block, cap, maxsize, small_threshold=768):
    """(resident, decode_transient). <=768 small_block, 768..maxsize big_block,
    >maxsize runtime cap (RGBA resident + one full-decode transient)."""
    tot = 0
    decode = 0
    for tx in texs:
        if tx.is_font:
            tot += tx.cur
            continue
        if tx.ls > maxsize:
            cw, ch = capped_dims(tx.w, tx.h, cap)
            tot += cw * ch * 4
            decode = max(decode, tx.w * tx.h * 4)
        else:
            b = small_block if tx.ls <= small_threshold else big_block
            if tx.block is not None and area(tx.block) >= area(b):
                tot += tx.cur
            else:
                tot += astc_bytes(tx.w, tx.h, b, tx.mips)
    return tot, decode


BANDS = (256, 768, 1280)         # band edges; bands: (256,768],(768,1280],(1280,∞)
LADDER = [(4, 4), (6, 6), (8, 8), (10, 10), (12, 12)]


def banded(texs, blocks, giant_cap=None):
    """One pass: (total, decode_transient, [band0,band1,band2 VRAM], kept).
    blocks=[b_lo,b_mid,b_hi]; if giant_cap set, >1280 uses runtime cap not b_hi."""
    band = [0, 0, 0]
    dec = kept = 0
    for tx in texs:
        if tx.is_font or tx.ls <= BANDS[0]:
            kept += tx.cur
            continue
        if tx.ls <= BANDS[1]:
            k, b = 0, blocks[0]
        elif tx.ls <= BANDS[2]:
            k, b = 1, blocks[1]
        else:
            k = 2
            if giant_cap is not None:
                cw, ch = capped_dims(tx.w, tx.h, giant_cap)
                band[2] += cw * ch * 4
                dec = max(dec, tx.w * tx.h * 4)
                continue
            b = blocks[2]
        if tx.block is not None and area(tx.block) >= area(b):
            band[k] += tx.cur
        else:
            band[k] += astc_bytes(tx.w, tx.h, b, tx.mips)
    return kept + sum(band), dec, band, kept


def smart_plan(texs, budget_mb, mode, cap=384):
    """Greedy per-band plan: start every band at the FINEST block, then coarsen
    the band currently eating the most VRAM until peak<=budget. mode 'A' all
    bands ASTC; mode 'B' giants(>1280) go to runtime cap, only the two ASTC
    bands coarsen. Returns (blocks, resident, decode) or None if even all-12x12
    can't fit."""
    i = [0, 0, 0]
    nb = 2 if mode == "B" else 3            # how many bands are coarsenable
    gc = cap if mode == "B" else None
    while True:
        blocks = [LADDER[i[0]], LADDER[i[1]], LADDER[i[2]]]
        tot, dec, band, _ = banded(texs, blocks, gc)
        if (tot + dec) / MB <= budget_mb:
            return blocks, tot, dec
        cand = [k for k in range(nb) if i[k] < len(LADDER) - 1]
        if not cand:
            return None                      # maxed out, still over budget
        i[max(cand, key=lambda k: band[k])] += 1   # coarsen biggest band


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.split("Usage", 1)[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="data.unity3d / *.bundle / a Data dir")
    ap.add_argument("--budgets", default="300,700,1100",
                    help="texture-RAM MB for 1G,2G,3G (default 300,700,1100)")
    ap.add_argument("--a-blocks", default="4x4,6x6,8x8,10x10,12x12")
    ap.add_argument("--b-small-blocks", default="4x4,6x6,8x8",
                    help="block candidates for ≤768 textures in Mode B")
    ap.add_argument("--b-big-blocks", default="6x6,8x8,10x10,12x12",
                    help="block candidates for 768~maxsize textures in Mode B")
    ap.add_argument("--b-caps", default="384,512,768")
    ap.add_argument("--deep", action="store_true",
                    help="PSNR-probe per-texture coarsest block (adds A 深探测)")
    ap.add_argument("--deep-psnr", type=float, default=40.0)
    ap.add_argument("--deep-blocks", default="12x12,10x10,8x8,6x6,5x5,4x4")
    ap.add_argument("--probe-max", type=int, default=512)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    def log(m): print(m, flush=True)

    budgets = sorted(int(x) for x in args.budgets.split(","))
    # name each tier by its budget; dynamic count so --budgets can list any
    tiers = [(f"≤{b}MB", b) for b in budgets]
    pb = lambda s: tuple(int(v) for v in s.split("x"))
    a_blocks = [pb(b) for b in args.a_blocks.split(",")]
    b_small = [pb(b) for b in args.b_small_blocks.split(",")]
    b_big = [pb(b) for b in args.b_big_blocks.split(",")]
    caps = [int(c) for c in args.b_caps.split(",")]
    cap_maxsize = {384: 1024, 512: 1280, 768: 2048}

    t_start = time.time()
    texs, fstats = load_textures(args.path, log)
    if not texs:
        sys.exit("no textures")
    total_vram = sum(t.cur for t in texs)
    budget_str = " ".join(nm for nm, bd in tiers)

    def fits(peak):
        """smallest tier name a peak fits, else '需>maxMB'."""
        for nm, bd in tiers:
            if peak / MB <= bd:
                return nm
        return f"需>{budgets[-1]}MB"

    # ── §1 resources & resolution distribution ───────────────────────────
    log("\n" + "=" * 72)
    log("§1 资源概览(每个资源文件的纹理数 / 显存)")
    log("=" * 72)
    log(f"  {'资源文件':<26}{'纹理':>7}{'VRAM':>10}")
    for name, n, v in fstats:
        log(f"  {name:<26}{n:>7}{v/MB:>8.0f}MB")
    log(f"  {'合计':<26}{len(texs):>7}{total_vram/MB:>8.0f}MB")
    log("\n  按分辨率档(非字体;占比基于当前 VRAM):")
    log(f"  {'分辨率档':<12}{'数量':>7}{'VRAM':>10}{'占比':>8}")
    for lab, lo, hi in (("≤256", 0, 256), ("257–768", 256, 768),
                        ("769–1280", 768, 1280), (">1280(巨图)", 1280, 1 << 30)):
        g = [t for t in texs if not t.is_font and lo < t.ls <= hi]
        v = sum(t.cur for t in g)
        log(f"  {lab:<12}{len(g):>7}{v/MB:>8.0f}MB{100*v/total_vram:>7.0f}%")
    fonts = [t for t in texs if t.is_font]
    fv = sum(t.cur for t in fonts)
    log(f"  {'字体(保留)':<12}{len(fonts):>7}{fv/MB:>8.0f}MB{100*fv/total_vram:>7.0f}%")
    log(f"  {'合计':<12}{len(texs):>7}{total_vram/MB:>8.0f}MB")
    big = max((t for t in texs if not t.is_font), key=lambda t: t.w * t.h)
    log(f"  最大非字体图: {big.name!r} {big.w}x{big.h} "
        f"→ 运行时 cap 需解码 {big.w*big.h*4/MB:.0f}MB = Mode B 峰值来源")
    log(f"  预算档: {budget_str}")

    # ── deep probe (optional) ─────────────────────────────────────────────
    if args.deep:
        blocks = [pb(b) for b in args.deep_blocks.split(",")]
        cands = [t for t in texs if not t.is_font and t.ls > 256]
        if args.limit:
            cands = cands[:args.limit]
        log(f"\nDeep probe {len(cands)} textures (PSNR≥{args.deep_psnr}, "
            f"{args.jobs} jobs)…")
        import concurrent.futures as cf

        def probe(tx):
            try:
                img = tx.obj.read().image
            except Exception:
                return tx, None
            if img is None or img.size == (0, 0):
                return tx, None
            return tx, coarsest_block(img, blocks, args.deep_psnr, args.probe_max)

        t0 = time.time()
        with cf.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            for i, (tx, b) in enumerate(pool.map(probe, cands), 1):
                tx.detected = b
                if i % 200 == 0 or i == len(cands):
                    r = i / (time.time() - t0 + 1e-6)
                    log(f"  [{i}/{len(cands)}] {r:.1f}/s")

    # ── §2 Mode A ──────────────────────────────────────────────────────────
    log("\n" + "=" * 72)
    log("§2 Mode A(只改 ASTC 块粗细,分辨率不变,无运行时解码峰值)")
    log("=" * 72)
    log(f"  {'block':<12}{'常驻VRAM':>10}{'最低可用':>10}")
    a_rows = []
    for b in a_blocks:
        r = mode_a(texs, b)
        a_rows.append((f"A {b[0]}x{b[1]}", r, 0))
        log(f"  {f'全 {b[0]}x{b[1]}':<12}{r/MB:>8.0f}MB{fits(r):>10}")
    if args.deep:
        r = mode_a(texs, (8, 8), detected=True)
        a_rows.append(("A 深探测", r, 0))
        log(f"  {'深探测(逐图)':<12}{r/MB:>8.0f}MB{fits(r):>10}   PSNR≥{args.deep_psnr:.0f} 选最粗块")

    # ── §3 Mode B (exhaustive flat: cap × small × big) ──────────────────────
    log("\n" + "=" * 72)
    log("§3 Mode B(≤768→小块,768~maxsize→大块,>maxsize→引擎cap;全配置)")
    log("   峰值 = 常驻 + 巨图一次性RGBA解码;最低可用按【峰值】判定")
    log("=" * 72)
    log(f"  {'引擎cap':>7}{'小块':>6}{'大块':>6}{'常驻':>9}{'+解码':>7}{'峰值':>8}{'最低可用':>9}")
    b_rows = []
    for cap in caps:
        ms = cap_maxsize.get(cap, cap * 3)
        for sb in b_small:
            for bb in b_big:
                r, dec = mode_b(texs, sb, bb, cap, ms)
                peak = r + dec
                b_rows.append((cap, sb, bb, r, dec))
                log(f"  {f'{cap}':>7}{f'{sb[0]}x{sb[1]}':>6}{f'{bb[0]}x{bb[1]}':>6}"
                    f"{r/MB:>7.0f}MB{dec/MB:>6.0f}MB{peak/MB:>7.0f}MB{fits(peak):>9}")
        log("")

    # ── §4 smart per-band plan (distribution-driven) ────────────────────────
    def bstr(b):
        return f"{b[0]}x{b[1]}"

    log("=" * 72)
    log("§4 智能建议(按上面占比,贪心:每档起步最细块,谁占显存大就先粗化,直到卡进预算)")
    log("=" * 72)
    for nm, bd in tiers:
        log(f"\n  【{nm}】")
        pa = smart_plan(texs, bd, "A")
        if pa:
            bl, r, _ = pa
            log(f"    Mode A(不动分辨率,无峰值): "
                f"257-768→{bstr(bl[0])}  769-1280→{bstr(bl[1])}  >1280→{bstr(bl[2])}"
                f"   常驻{r/MB:.0f}MB")
        else:
            log("    Mode A: 连全12x12也超预算 → 需音频流式/字体子集,或最后手段物理缩分辨率")
        # Mode B: lowest cap frees most budget for finer ASTC → best quality fit
        pb_best = None
        for cap in sorted(caps):
            res = smart_plan(texs, bd, "B", cap=cap)
            if res:
                pb_best = (cap,) + res
                break
        if pb_best:
            cap, bl, r, dec = pb_best
            log(f"    Mode B(巨图交引擎cap{cap},+解码{dec/MB:.0f}峰值): "
                f"257-768→{bstr(bl[0])}  769-1280→{bstr(bl[1])}  >1280→cap{cap}"
                f"   常驻{r/MB:.0f}/峰值{(r+dec)/MB:.0f}MB")
        else:
            log("    Mode B: 巨图解码峰值已超预算")
        # verdict
        if pa and pb_best:
            if pa[1] <= pb_best[2] + pb_best[3]:
                log(f"    → 取 A:常驻{pa[1]/MB:.0f}≤B峰值{(pb_best[2]+pb_best[3])/MB:.0f},无峰值波动最稳")
            else:
                log("    → A 稳(无峰值);B 常驻更低但有巨图解码峰值,看引擎是否顺序加载")
        elif pa:
            log("    → 取 A(B 峰值超预算)")
        elif pb_best:
            log("    → 只能 B(注意巨图解码峰值)")

    log(f"\n说明:A=纯改块不动分辨率(无解码峰值,最稳);B=巨图交引擎运行时cap(常驻更低,")
    log("但加载要解码巨图→+峰值)。§3 是全配置参考,§4 是按本包分布算出的最终建议。")
    log(f"\n[执行 {time.time()-t_start:.1f}s;扫描占大头,§2/§3/§4 全是解析计算≈0 开销]")


if __name__ == "__main__":
    main()
