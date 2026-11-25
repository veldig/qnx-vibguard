#!/usr/bin/env python3
"""Noisy-neighbour chart from the fixed KVM load test: how far each sample was behind its 5 ms schedule."""
import sys, os, csv
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
res = sys.argv[1] if len(sys.argv) > 1 else "results/kvm_load"
def behind(r):
    t = [float(x["t_ms"]) for x in csv.DictReader(open(os.path.join(res, r, "vibguard_ticks.csv")))]
    return [i * 0.005 for i in range(len(t))], [(t[i] - i * 5.0) / 1000 for i in range(len(t))]
def alert_delay(r):
    d = 0
    for e in csv.DictReader(open(os.path.join(res, r, "vibguard_events.csv"))):
        d = max(d, float(e["t_s"]) - float(e["sample_t_s"]))
    return d
BG, FG, AMBER, GREY, DIM = "#0f1115", "#e6e6e6", "#ffb000", "#8a909a", "#262a31"
plt.rcParams.update({"font.family": "monospace", "text.color": FG, "axes.labelcolor": FG,
                     "xtick.color": GREY, "ytick.color": GREY, "axes.edgecolor": GREY})
fig, ax = plt.subplots(figsize=(10, 6.2), facecolor=BG)
ax.set_facecolor(BG); ax.grid(color=DIM, lw=0.6)
for s in ("top", "right"): ax.spines[s].set_visible(False)
x2, y2 = behind("L2"); x1, y1 = behind("L1")
ax.plot(x2, y2, color=GREY, lw=2)
ax.plot(x1, y1, color=AMBER, lw=2)
n1 = sum(1 for v in y1 if v > 0.0025); n2 = sum(1 for v in y2 if v > 0.0025)
ax.text(15.5, 29, f"default priority 10 (below the hogs)\n{n2:,} of {len(y2):,} samples late, worst {max(y2):.1f} s behind\nvibration alert issued {alert_delay('L2'):.1f} s late\n(loop blocked until the hogs finished)",
        color=FG, ha="left", va="top", fontsize=10)
ax.text(0.8, 1.0, f"SCHED_FIFO 50 (above the hogs)\n{n1} of {len(y1):,} samples late, worst {max(y1)*1000:.1f} ms",
        color=AMBER, ha="left", va="bottom", fontsize=10)
ax.set_xlabel("scheduled sample time (s)"); ax.set_ylabel("sample taken behind schedule (s)")
ax.set_xlim(0, 30.5); ax.set_ylim(-0.5, 31)
fig.text(0.02, 0.965, "// SYSTEM DESIGN NOTES", color=AMBER, fontsize=10, ha="left")
fig.text(0.02, 0.92, "200 Hz control loop vs two CPU-hog threads at SCHED_FIFO 15, QNX 8.0 under KVM", color=FG, fontsize=12, ha="left")
fig.text(0.02, 0.015, "Measured, 30 s per run, same image and load. Hogs overlapped the full run.", color=GREY, fontsize=8, ha="left")
fig.text(0.98, 0.015, "Martin Vizcaino  |  martinsvizcaino.com", color=GREY, fontsize=9, ha="right")
fig.tight_layout(rect=(0, 0.04, 1, 0.9))
out = os.path.join(os.path.dirname(res.rstrip("/")) or ".", "vibguard_noisy_neighbour_kvm.png")
fig.savefig(out, dpi=160, facecolor=BG); print("wrote", out)
