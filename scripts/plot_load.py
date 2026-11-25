#!/usr/bin/env python3
"""Noisy-neighbour chart: acquisition tick intervals with 2 CPU hogs, RT priorities vs default."""
import sys, os, csv
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
res = sys.argv[1]
def ticks(r):
    with open(os.path.join(res, f"load_{r}", "vibguard_ticks.csv")) as f:
        R = list(csv.DictReader(f))[1:]
    return [float(x["t_ms"]) / 1000 for x in R], [float(x["interval_us"]) / 1000 for x in R]
def line(r, key):
    for l in open(os.path.join(res, f"load_{r}", "vibguard_summary.txt")):
        if l.startswith(key): return l.strip()
    return ""
BG, FG, AMBER, GREY = "#0f1115", "#e6e6e6", "#ffb000", "#7a808a"
plt.rcParams.update({"font.family": "monospace", "text.color": FG, "axes.labelcolor": FG,
                     "xtick.color": GREY, "ytick.color": GREY, "axes.edgecolor": GREY})
fig, ax = plt.subplots(2, 1, figsize=(11, 6.5), sharex=True, sharey=True, facecolor=BG)
for a, r, col, lab in [(ax[0], "L1", AMBER, "L1  2 CPU hogs @ FIFO 15, vibguard @ FIFO 50/45/40"),
                       (ax[1], "L2", GREY,  "L2  2 CPU hogs @ FIFO 15, vibguard @ default priority 10")]:
    t, iv = ticks(r)
    a.set_facecolor(BG); a.grid(color="#262a31", lw=0.6)
    a.plot(t, iv, color=col, lw=0.6)
    a.axhline(7.5, color=AMBER, ls="--", lw=0.8, alpha=0.6)
    a.set_yscale("log"); a.set_ylabel("tick interval (ms)")
    late = line(r, "ticks later"); lag = line(r, "max analysis lag")
    a.set_title(lab, color=FG, loc="left", fontsize=9)
    a.text(0.99, 0.95, late + "\n" + lag, transform=a.transAxes, ha="right", va="top", color=GREY, fontsize=7.5)
ax[1].set_xlabel("time (s)   target 5 ms, dashed = 1.5x period")
fig.suptitle("// SYSTEM DESIGN NOTES   noisy neighbour on QNX 8.0: priorities protect the 200 Hz loop", color=FG, x=0.02, ha="left", fontsize=11)
fig.text(0.98, 0.01, "Martin Vizcaino  |  martinsvizcaino.com", color=GREY, ha="right", fontsize=8)
fig.text(0.02, 0.01, "QNX 8.0 VM, x86_64 under emulation: absolute timing is emulator-limited; compare L1 vs L2.", color=GREY, fontsize=7.5)
fig.tight_layout(rect=(0, 0.03, 1, 0.96))
out = os.path.join(res, "vibguard_noisy_neighbour.png"); fig.savefig(out, dpi=150, facecolor=BG); print("wrote", out)
