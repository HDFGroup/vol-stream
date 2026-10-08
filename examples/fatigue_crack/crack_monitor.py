#!/usr/bin/env python3
# * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
# Copyright by The HDF Group.  All rights reserved.
# This file is part of vol-stream.  See the LICENSE file at the root of the
# source distribution, or https://www.hdfgroup.org/licenses.
# * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
"""Online crack tracking on a live micro-CT fatigue stream.

tomo_fatigue_writer replays the TomoBank Al7075 fatigue series one
checkpoint per step: every step rewrites a full DXchange scan, 1500
projections of 2160x2560 uint16 (15.7 GiB with darks and whites). This
consumer is what an online-analysis node at the beamline would run, and it
does not take the scan. It subscribes to a box:

  --mode crack    a band of detector rows across the crack (--rows A:B), every
                  projection. Per checkpoint it reconstructs --slices
                  horizontal slices through the band by filtered
                  back-projection and reports the damaged fraction of each
                  cross-section against the fatigue cycle.
  --mode preview  one projection (--projection K) and one dark and one white
                  frame, for a control-room picture of the specimen.

Either way the writer sends this subscriber only the elements in its box
(the rest of each H5Dwrite() never crosses the wire), and the report at the
end compares the bytes received with the bytes the writer wrote.

Needs only NumPy. With matplotlib installed it also writes a PNG per
checkpoint and a damage-versus-cycle plot into --out.
"""

import argparse
import os
import sys
import time

import numpy as np

import volstream

DATA, DARK, WHITE = "/exchange/data", "/exchange/data_dark", "/exchange/data_white"
THETA, CYCLE = "/exchange/theta", "/measurement/sample/fatigue_cycle"


# --- reconstruction (numpy only) -------------------------------------------

def highpass(x, w=25):
    """x minus its moving average along the last axis."""
    k = np.ones(w) / w
    return x - np.apply_along_axis(lambda r: np.convolve(r, k, mode="same"), -1, x)


def rotation_centre(p0, p180):
    """Centre of rotation from a 0/180 degree pair: p180 mirrored, cross-correlated with p0."""
    a, b = highpass(p0), highpass(p180[..., ::-1])
    a, b = a.mean(0) if a.ndim == 2 else a, b.mean(0) if b.ndim == 2 else b
    n = len(a)
    corr = np.fft.irfft(np.fft.rfft(a, 2 * n) * np.conj(np.fft.rfft(b, 2 * n)), 2 * n)
    lag = int(np.argmax(np.concatenate([corr[-(n - 1):], corr[:n]]))) - (n - 1)
    return (n - 1 + lag) / 2.0


def fbp(sino, theta_deg, centre):
    """Filtered back-projection of one sinogram (n_angles, n_det) about `centre` (detector px)."""
    nang, ndet = sino.shape
    pad = 1 << int(np.ceil(np.log2(2 * ndet)))
    freq = np.fft.rfftfreq(pad)
    ramp = 2 * freq * np.sinc(freq)                       # Shepp-Logan filtered ramp
    filt = np.fft.irfft(np.fft.rfft(sino, pad, axis=1) * ramp, pad, axis=1)[:, :ndet]
    half = ndet / 2.0
    xs = np.arange(ndet) - half + 0.5
    X, Y = np.meshgrid(xs, -xs)
    img = np.zeros(X.shape, dtype=np.float64)
    det = np.arange(ndet, dtype=np.float64)
    for th, row in zip(np.radians(theta_deg), filt):
        t = X * np.cos(th) + Y * np.sin(th) + centre
        img += np.interp(t.ravel(), det, row, left=0.0, right=0.0).reshape(X.shape)
    return img * np.pi / (2 * nang)


def specimen_disk(sino, theta_deg, centre):
    """The specimen's cross-section as (x0, y0, radius) in the reconstruction's pixel frame.

    Its left/right edges in each projection trace mid(theta) = centre + x0*cos - y0*sin;
    a least-squares fit gives the disk's offset and the median half-width its radius.
    Returns None when there is no material in this slice (a fully separated crack).
    """
    s = np.convolve(sino.mean(0), np.ones(9) / 9, mode="same")
    if s.max() < 0.2:
        return None
    mids, halfs = [], []
    for row in sino:
        r = np.convolve(row, np.ones(9) / 9, mode="same")
        above = np.nonzero(r > 0.5 * r.max())[0]
        mids.append((above[0] + above[-1]) / 2.0)
        halfs.append((above[-1] - above[0]) / 2.0)
    th = np.radians(theta_deg)
    A = np.stack([np.cos(th), -np.sin(th)], axis=1)
    (x0, y0), *_ = np.linalg.lstsq(A, np.asarray(mids) - centre, rcond=None)
    return x0, y0, float(np.median(halfs))


def box_blur(img, k):
    """Mean over a (2k+1)^2 box, from a summed-area table."""
    c = np.cumsum(np.cumsum(np.pad(img, ((k + 1, k), (k + 1, k)), mode="edge"), 0), 1)
    return (c[2 * k + 1:, 2 * k + 1:] - c[:-2 * k - 1, 2 * k + 1:]
            - c[2 * k + 1:, :-2 * k - 1] + c[:-2 * k - 1, :-2 * k - 1]) / (2 * k + 1) ** 2


def disk_mask(img, disk, frac=0.92):
    x0, y0, r = disk
    n = img.shape[0]
    xs = np.arange(n) - n / 2.0 + 0.5
    X, Y = np.meshgrid(xs, -xs)
    return (X - x0) ** 2 + (Y - y0) ** 2 < (frac * r) ** 2


def damage(img, disk, mu_ref):
    """Fraction of the specimen cross-section that is not metal: crack, pore, or open gap.

    A pixel counts once its 7x7 neighbourhood mean falls below half of mu_ref, the
    alloy's attenuation measured on the first checkpoint. The smoothing is what
    keeps noise, rings and phase-contrast fringes out of it: unsmoothed, an
    intact cross-section scores ~0.15; smoothed, ~0.01.
    """
    if disk is None:
        return 1.0
    inside = disk_mask(img, disk)
    if inside.sum() < 100:
        return 1.0
    return float((box_blur(img, 3)[inside] < 0.5 * mu_ref).mean())


# --- the two subscribers ---------------------------------------------------

def expectations(args):
    n, h, w = args.shape
    return {
        DATA: ((n, h, w), "uint16"),
        DARK: ((args.flats, h, w), "uint16"),
        WHITE: ((args.flats, h, w), "uint16"),
        THETA: ((n,), "float64"),
        CYCLE: ((), "int64"),
    }


def subscribe(f, args):
    n, h, w = args.shape
    if args.mode == "crack":
        a, b = args.rows
        box = {DATA: ((0, a, 0), (n, b - a, w)),
               DARK: ((0, a, 0), (args.flats, b - a, w)),
               WHITE: ((0, a, 0), (args.flats, b - a, w))}
    else:
        k = args.projection
        box = {DATA: ((k, 0, 0), (1, h, w)),
               DARK: ((0, 0, 0), (1, h, w)),
               WHITE: ((0, 0, 0), (1, h, w))}
    # Only the image stacks are worth compressing in transit; the angles and
    # the cycle count are small, and a scalar cannot be deflated at all.
    images = {p: box[p] for p in (DATA, DARK, WHITE)}
    f.subscribe(images, deflate=args.deflate, expect=expectations(args))
    f.subscribe({THETA: None, CYCLE: None}, expect=expectations(args))


def transmission(data, dark, white):
    """-log of flat- and dark-corrected transmission (the line integral of attenuation)."""
    d, wh = dark.astype(np.float32).mean(0), white.astype(np.float32).mean(0)
    return -np.log(np.clip((data.astype(np.float32) - d) / np.maximum(wh - d, 1.0), 1e-3, 2.0))


def analyse_crack(step, args, out):
    """Reconstruct --slices slices through the band; return (rows, damage, centre, images, disks)."""
    data, dark, white = (np.ma.getdata(step[p]) for p in (DATA, DARK, WHITE))
    theta = np.ma.getdata(step[THETA])
    a, b = args.rows
    nb = b - a
    B = args.bin
    # Centre from the first and last projection (0 and ~180 deg) across the whole band.
    p0 = transmission(data[0], dark, white)
    p180 = transmission(data[-1], dark, white)
    centre = rotation_centre(p0, p180)
    rows, imgs, disks = [], [], []
    for r in np.linspace(4, nb - 5, args.slices).astype(int):
        sl = slice(r - 4, r + 4)
        sino = transmission(data[:, sl, :].mean(1), dark[:, sl, :].mean(1), white[:, sl, :].mean(1))  # (n, w)
        W = sino.shape[1] // B * B
        sb = sino[::B, :W].reshape(-1, W // B, B).mean(2)
        th = theta[::B][: sb.shape[0]]
        img = fbp(sb, th, centre / B)
        disk = specimen_disk(sb, th, centre / B)
        rows.append(a + r)
        imgs.append(img)
        disks.append(disk)
    if args.mu_ref is None:
        # Calibrate on the first checkpoint: the median attenuation of the alloy itself.
        vals = [img[disk_mask(img, d)] for img, d in zip(imgs, disks) if d is not None]
        args.mu_ref = float(np.median(np.concatenate(vals)))
    dmg = [damage(img, d, args.mu_ref) for img, d in zip(imgs, disks)]
    return rows, dmg, centre, imgs, disks


def save_png(path, title, imgs, labels):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return False
    fig, ax = plt.subplots(1, len(imgs), figsize=(3.6 * len(imgs), 3.9), squeeze=False)
    for a, img, lab in zip(ax[0], imgs, labels):
        n = img.shape[0]
        # A reconstruction is square with the specimen in its middle third or so;
        # a projection is shown whole.
        crop = img[n // 6: 5 * n // 6, n // 6: 5 * n // 6] if img.shape[0] == img.shape[1] else img
        a.imshow(crop, cmap="gray", vmin=np.percentile(crop, 1), vmax=np.percentile(crop, 99.5))
        a.set_title(lab, fontsize=9)
        a.axis("off")
    fig.suptitle(title)
    fig.tight_layout()
    fig.savefig(path, dpi=80)
    plt.close(fig)
    return True


def save_curve(path, history):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return False
    cyc = [h[0] for h in history]
    fig, ax = plt.subplots(figsize=(6, 4))
    for i in range(len(history[0][1])):
        ax.plot(cyc, [h[1][i] for h in history], "o-", label=f"row {history[0][2][i]}")
    ax.plot(cyc, [max(h[1]) for h in history], "k--", lw=2, label="worst slice")
    ax.set_xlabel("fatigue cycle")
    ax.set_ylabel("damaged fraction of cross-section")
    ax.set_title("Al7075 fatigue crack, from the live stream")
    ax.legend(fontsize=7)
    fig.tight_layout()
    fig.savefig(path, dpi=90)
    plt.close(fig)
    return True


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("file", help="the writer's file")
    p.add_argument("--mode", choices=("crack", "preview"), default="crack")
    p.add_argument("--rows", default="400:960", help="crack mode: detector rows A:B to subscribe to (default 400:960)")
    p.add_argument("--slices", type=int, default=8, help="crack mode: slices reconstructed per checkpoint (default 8)")
    p.add_argument("--bin", type=int, default=2, help="crack mode: angle and pixel binning for reconstruction (default 2)")
    p.add_argument("--projection", type=int, default=0, help="preview mode: which projection (default 0)")
    p.add_argument("--shape", default="1500,2160,2560", help="projections,rows,cols of /exchange/data (default TomoBank's)")
    p.add_argument("--flats", type=int, default=10, help="dark/white frames per scan (default 10)")
    p.add_argument("--deflate", type=int, default=None,
                   help="have the writer deflate this subscriber's images in transit (level 0-9; default none)")
    p.add_argument("--out", default=None, help="directory for PNGs and the summary CSV")
    p.add_argument("--timeout", type=float, default=3600.0, help="seconds to wait for the writer (default 3600)")
    p.add_argument("--expect-steps", type=int, default=0, help="fail unless exactly N checkpoints arrive")
    args = p.parse_args()
    args.shape = tuple(int(x) for x in args.shape.split(","))
    args.rows = tuple(int(x) for x in args.rows.split(":"))
    args.mu_ref = None  # set from the first checkpoint
    if args.out:
        os.makedirs(args.out, exist_ok=True)

    n, h, w = args.shape
    full_step = (n + 2 * args.flats) * h * w * 2
    tag = f"{args.mode}"
    f = volstream.open(args.file)
    subscribe(f, args)
    sel = (f"rows {args.rows[0]}:{args.rows[1]} of every projection" if args.mode == "crack"
           else f"projection {args.projection} plus one dark and one white frame")
    print(f"{tag}: subscribed to {sel}" + (f", deflated at level {args.deflate} in transit" if args.deflate is not None else ""), flush=True)

    history, got, received_total = [], 0, 0
    try:
        for step in f.steps(timeout=args.timeout):
            if DATA not in step:
                continue
            t0 = time.monotonic()
            got += 1
            cycle = int(np.ma.getdata(step[CYCLE])) if CYCLE in step else -1
            received = sum(np.ma.getdata(step[q]).nbytes for q in (DATA, DARK, WHITE) if q in step)
            received_total += received
            exact = "exact" if not step.delivery.get(DATA) else f"delivery bits {step.delivery[DATA]:#x}"
            print(f"{tag}: step {step.phys}  fatigue cycle {cycle:6d}  received {received / 2**20:8.1f} MiB "
                  f"of {full_step / 2**30:.1f} GiB written ({100.0 * received / full_step:.2f}%, {exact})",
                  flush=True)
            if args.mode == "crack":
                rows, dmg, centre, imgs, _ = analyse_crack(step, args, args.out)
                history.append((cycle, dmg, rows))
                print(f"{tag}:   centre {centre:.1f}  damaged fraction by row: "
                      + "  ".join(f"{r}:{d:.3f}" for r, d in zip(rows, dmg))
                      + f"  ({time.monotonic() - t0:.0f}s)", flush=True)
                if args.out:
                    save_png(os.path.join(args.out, f"crack_{got - 1:02d}_cycle{cycle:05d}.png"),
                             f"fatigue cycle {cycle}", imgs, [f"row {r}  damage {d:.3f}" for r, d in zip(rows, dmg)])
            else:
                data, dark, white = (np.ma.getdata(step[q]) for q in (DATA, DARK, WHITE))
                t = transmission(data[0], dark, white)
                if args.out:
                    save_png(os.path.join(args.out, f"preview_{got - 1:02d}_cycle{cycle:05d}.png"),
                             f"fatigue cycle {cycle}, projection {args.projection}", [t], ["-log T"])
    finally:
        f.close()

    print(f"{tag}: {got} checkpoint(s), {received_total / 2**30:.2f} GiB received of "
          f"{got * full_step / 2**30:.1f} GiB written ({100.0 * received_total / max(got * full_step, 1):.2f}%)")
    if args.mode == "crack" and history and args.out:
        with open(os.path.join(args.out, "damage.csv"), "w") as out:
            out.write("fatigue_cycle," + ",".join(f"row{r}" for r in history[0][2]) + "\n")
            for cyc, dmg, _ in history:
                out.write(f"{cyc}," + ",".join(f"{d:.4f}" for d in dmg) + "\n")
        save_curve(os.path.join(args.out, "damage_vs_cycle.png"), history)
    if args.expect_steps and got != args.expect_steps:
        print(f"{tag}: FAIL expected {args.expect_steps} checkpoint(s), got {got}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
