#!/usr/bin/env python3
"""Before/after chart: simulated SCARA joint, open loop (S0) vs vibguard closed loop (S1)."""
import sys, os, csv
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
s0, s1, out = sys.argv[1], sys.argv[2], sys.argv[3]
def rows(d, n):
    with open(os.path.join(d, n)) as f: return list(csv.DictReader(f))
def series(d):
    A = rows(d, "vibguard_analysis.csv")
    return [float(r["sample_t_s"]) for r in A], [float(r["rms_mg"]) for r in A]
def jerk(d, tend):
    jt, jv = [0.0], [100.0]
    for e in rows(d, "vibguard_events.csv"):
        jt += [float(e["sample_t_s"])] * 2; jv += [jv[-1], float(e["jerk_to_pct"])]
    jt.append(tend); jv.append(jv[-1]); return jt, jv
def summ(d):
    for line in open(os.path.join(d, "vibguard_summary.txt")):
        if line.startswith("sim:"): return line.strip()
t0, r0 = series(s0); t1, r1 = series(s1)
BG, FG, AMBER, GREY = "#0f1115", "#e6e6e6", "#ffb000", "#7a808a"
plt.rcParams.update({"font.family": "monospace", "text.color": FG, "axes.labelcolor": FG,
                     "xtick.color": GREY, "ytick.color": GREY, "axes.edgecolor": GREY})
fig, ax = plt.subplots(2, 1, figsize=(11, 6.5), sharex=True, facecolor=BG, gridspec_kw={"height_ratios": [3, 1.3]})
for a in ax:
    a.set_facecolor(BG); a.grid(color="#262a31", lw=0.6)
    for lo, hi in [(5, 35), (45, 60)]: a.axvspan(lo, hi, color="#1b1f26", zorder=0)
ax[0].plot(t0, r0, color=GREY, lw=1.3, label="guard OFF (open loop)")
ax[0].plot(t1, r1, color=AMBER, lw=1.6, label="vibguard ON (closed loop, QNX)")
ax[0].axhline(30, color=AMBER, ls="--", lw=0.8, alpha=0.6)
ax[0].set_ylabel("3-axis vibration RMS (mg)"); ax[0].legend(facecolor=BG, edgecolor=GREY, loc="upper right", fontsize=8)
jt, jv = jerk(s1, t1[-1]); ax[1].step(jt, jv, where="post", color=AMBER, lw=1.6)
ax[1].axhline(100, color=GREY, lw=1.0, ls=":"); ax[1].set_ylim(0, 110); ax[1].set_ylabel("jerk limit %")
ax[1].set_xlabel("time (s)   shaded = arm drawing, clear = idle")
fig.suptitle("// SYSTEM DESIGN NOTES   vibguard on QNX 8.0: simulated SCARA joint, before vs after", color=FG, x=0.02, ha="left", fontsize=11)
fig.text(0.02, 0.01, "OFF  " + summ(s0)[5:] + "\nON   " + summ(s1)[5:], color=GREY, fontsize=7.5, ha="left")
fig.text(0.98, 0.01, "Martin Vizcaino  |  martinsvizcaino.com", color=GREY, ha="right", fontsize=8)
fig.tight_layout(rect=(0, 0.06, 1, 0.96)); fig.savefig(out, dpi=150, facecolor=BG); print("wrote", out)
