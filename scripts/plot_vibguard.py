#!/usr/bin/env python3
"""Plot vibguard output: RMS vs thresholds, jerk limit, FFT peak, and tick jitter histogram."""
import sys, os, csv
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

d = sys.argv[1]; title = sys.argv[2] if len(sys.argv) > 2 else os.path.basename(d)
hi, lo = 30.0, 10.0
def rows(name):
    with open(os.path.join(d, name)) as f: return list(csv.DictReader(f))
A = rows("vibguard_analysis.csv"); E = rows("vibguard_events.csv"); T = rows("vibguard_ticks.csv")
t = [float(r["sample_t_s"]) for r in A]; rms = [float(r["rms_mg"]) for r in A]; pk = [float(r["peak_hz"]) for r in A]
# jerk limit as a step function in sample time
jt, jv = [0.0], [100.0]
for e in E:
    jt += [float(e["sample_t_s"])] * 2; jv += [jv[-1], float(e["jerk_to_pct"])]
jt.append(t[-1] if t else 0); jv.append(jv[-1])

BG, FG, AMBER, GREY = "#0f1115", "#e6e6e6", "#ffb000", "#7a808a"
plt.rcParams.update({"font.family": "monospace", "text.color": FG, "axes.labelcolor": FG,
                     "xtick.color": GREY, "ytick.color": GREY, "axes.edgecolor": GREY})
fig, ax = plt.subplots(3, 1, figsize=(11, 8), sharex=True, facecolor=BG,
                       gridspec_kw={"height_ratios": [3, 1.6, 1.2]})
for a in ax: a.set_facecolor(BG); a.grid(color="#262a31", lw=0.6)
ax[0].plot(t, rms, color=FG, lw=1.2, label="window RMS (mg)")
ax[0].axhline(hi, color=AMBER, ls="--", lw=1, label=f"alert {hi:.0f} mg")
ax[0].axhline(lo, color=GREY, ls=":", lw=1, label=f"restore {lo:.0f} mg")
for e in E:
    if e["kind"] != "restore_step": ax[0].axvline(float(e["sample_t_s"]), color=AMBER, lw=0.8, alpha=0.6)
ax[0].set_yscale("symlog", linthresh=10); ax[0].set_ylim(bottom=0); ax[0].set_ylabel("vibration RMS (mg)")
ax[0].legend(facecolor=BG, edgecolor=GREY, loc="upper right", fontsize=8)
ax[1].step(jt, jv, where="post", color=AMBER, lw=1.6); ax[1].set_ylim(0, 110); ax[1].set_ylabel("jerk limit %")
ax[2].plot(t, pk, color=GREY, lw=0.8); ax[2].set_ylabel("FFT peak Hz"); ax[2].set_xlabel("time in recording (s)")
fig.suptitle(f"// SYSTEM DESIGN NOTES   vibguard: {title}", color=FG, x=0.02, ha="left", fontsize=11)
fig.text(0.98, 0.01, "Martin Vizcaino  |  martinsvizcaino.com", color=GREY, ha="right", fontsize=8)
fig.tight_layout(rect=(0, 0.02, 1, 0.96)); fig.savefig(os.path.join(d, "vibguard_timeline.png"), dpi=150, facecolor=BG)

iv = [float(r["interval_us"]) for r in T[1:]]
fig, a = plt.subplots(figsize=(8, 4), facecolor=BG); a.set_facecolor(BG); a.grid(color="#262a31", lw=0.6)
a.hist(iv, bins=80, color=AMBER); a.set_yscale("log")
a.set_xlabel("acquisition tick interval (us), target 5000"); a.set_ylabel("count")
a.set_title(f"// tick intervals: {title}", color=FG, loc="left", fontsize=10)
fig.tight_layout(); fig.savefig(os.path.join(d, "vibguard_jitter.png"), dpi=150, facecolor=BG)
print("wrote", os.path.join(d, "vibguard_timeline.png"), os.path.join(d, "vibguard_jitter.png"))
