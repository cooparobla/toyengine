#!/usr/bin/env python3
"""Graph a toyengine profiling-mode CSV (see main.cpp: PROFILE=1).

    HEADLESS=1 PROFILE=1 MAX_FRAMES=600 ./build/toyengine terrain_demo
    python3 tools/plot_profile.py output/profile.csv            # -> output/profile.png
    python3 tools/plot_profile.py output/profile.csv --show     # also open a window
    python3 tools/plot_profile.py output/profile.csv --summary  # text table only (no matplotlib)

Two stacked panels over frame number:
  * GPU: per-feature pass time as stacked areas (the largest --top features, the rest
    folded into "other"), with gpu.total on top. The features sum to the total by
    construction (see toyengine/render/gpu_profiler.h).
  * CPU: frame time, with the CPU phases (input, scene update, gather, record, and the
    time blocked on the GPU in submit/present) as lines.

Needs matplotlib for the plot: pip install matplotlib
"""

import argparse
import csv
import os
import sys

WARMUP_FRAMES = 30  # matches FrameProfile::kWarmupFrames


def load(path):
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        cols = {name: [] for name in header}
        for row in reader:
            if len(row) != len(header):
                continue  # a partially-flushed final line
            for name, value in zip(header, row):
                cols[name].append(float(value))
    return header, cols


def summary(header, cols):
    frames = cols["frame"]
    keep = [i for i, f in enumerate(frames) if f >= WARMUP_FRAMES] or list(range(len(frames)))
    n = len(keep)
    avg = lambda name: sum(cols[name][i] for i in keep) / n
    gpu_total = avg("gpu.total")
    print(f"{n} frames (after {WARMUP_FRAMES} warm-up): frame {avg('cpu.frame'):.2f} ms, GPU {gpu_total:.2f} ms")
    rows = [(name[4:], avg(name)) for name in header if name.startswith("gpu.") and name != "gpu.total"]
    rows = [r for r in rows if r[1] > 0.0]
    rows.sort(key=lambda r: -r[1])
    print(f"  {'GPU scope':<22} {'avg ms':>8} {'% gpu':>7}")
    for name, ms in rows:
        print(f"  {name:<22} {ms:8.3f} {100.0 * ms / gpu_total if gpu_total else 0.0:6.1f}%")
    print(f"  {'CPU phase':<22} {'avg ms':>8}")
    for name in header:
        if name.startswith("cpu."):
            print(f"  {name[4:]:<22} {avg(name):8.3f}")


def plot(header, cols, out, show, top, source):
    try:
        import matplotlib
        if not show:
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is not installed -- run: pip install matplotlib", file=sys.stderr)
        print("(or use --summary for a text table)", file=sys.stderr)
        return 1

    frames = cols["frame"]
    gpu_names = [n for n in header if n.startswith("gpu.") and n != "gpu.total"]
    gpu_names = [n for n in gpu_names if max(cols[n], default=0.0) > 0.0]
    gpu_names.sort(key=lambda n: -sum(cols[n]))
    shown, rest = gpu_names[:top], gpu_names[top:]
    series = [cols[n] for n in shown]
    labels = [n[4:] for n in shown]
    if rest:
        series.append([sum(cols[n][i] for n in rest) for i in range(len(frames))])
        labels.append(f"other ({len(rest)})")

    fig, (ax_gpu, ax_cpu) = plt.subplots(2, 1, figsize=(14, 9), sharex=True,
                                         gridspec_kw={"height_ratios": [3, 2]})
    # tab20: distinct colours for up to 20 bands (the default cycle repeats after 10, which
    # would paint "other" the same colour as the largest feature).
    colors = [plt.get_cmap("tab20")(i % 20) for i in range(len(series))]
    ax_gpu.stackplot(frames, series, labels=labels, colors=colors, alpha=0.9)
    ax_gpu.plot(frames, cols["gpu.total"], color="black", linewidth=0.8, label="gpu.total")
    ax_gpu.set_ylabel("GPU ms")
    ax_gpu.set_title(f"GPU time per feature -- {os.path.basename(source)}")
    ax_gpu.legend(loc="upper left", bbox_to_anchor=(1.01, 1.0), fontsize=8)
    ax_gpu.grid(alpha=0.3)

    ax_cpu.plot(frames, cols["cpu.frame"], color="black", linewidth=1.0, label="frame")
    for name in ("cpu.render.submit_present", "cpu.render.gather", "cpu.render.record",
                 "cpu.scene_update", "cpu.input", "cpu.render.wait_fence"):
        if name in cols:
            ax_cpu.plot(frames, cols[name], linewidth=0.8, label=name[4:])
    ax_cpu.set_ylabel("CPU ms")
    ax_cpu.set_xlabel("frame")
    ax_cpu.set_title("CPU phases (submit_present includes waiting on the GPU)")
    ax_cpu.legend(loc="upper left", bbox_to_anchor=(1.01, 1.0), fontsize=8)
    ax_cpu.grid(alpha=0.3)

    fig.tight_layout()
    fig.savefig(out, dpi=110)
    print(f"wrote {out}")
    if show:
        plt.show()
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", help="profile CSV written by PROFILE=...")
    ap.add_argument("--out", help="output PNG (default: next to the CSV)")
    ap.add_argument("--show", action="store_true", help="also open an interactive window")
    ap.add_argument("--summary", action="store_true", help="print a text table instead of plotting")
    ap.add_argument("--top", type=int, default=10, help="GPU features drawn individually (default 10)")
    args = ap.parse_args()

    header, cols = load(args.csv)
    if not cols["frame"]:
        print("no frames in the CSV", file=sys.stderr)
        return 1
    if args.summary:
        summary(header, cols)
        return 0
    out = args.out or os.path.splitext(args.csv)[0] + ".png"
    return plot(header, cols, out, args.show, args.top, args.csv)


if __name__ == "__main__":
    sys.exit(main())
