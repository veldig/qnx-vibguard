# vibguard: a real-time vibration guard for a SCARA robot on QNX 8.0

vibguard watches a robot arm's IMU at 200 Hz, detects vibration in real time, and closes the loop: when vibration crosses a threshold it lowers the jerk limit of the motion profile, then restores it once the arm settles. It is written in C on QNX 8.0 using QNX timers, pulses and priority scheduling, and it runs on a QNX 8.0 virtual machine built from scratch with `mkqnximage`.

Built for the QNX Global Hackathon 2026 (Robotics & Medical).

![vibguard on QNX 8.0: guard off vs guard on](results/vibguard_before_after.png)

## Why

I research vibration suppression on a 6-DOF SCARA robot (ESP32, C++), using jerk-limited command shaping. That work happens offline: record IMU data, analyse it in MATLAB, change the motion profile, repeat. vibguard moves the loop onto a real-time OS so the robot can protect itself while it moves: measure, decide, and command a gentler profile within milliseconds, with timing guarantees that a general-purpose OS does not give.

## What it does

| Thread | Priority | Job |
|---|---|---|
| `acq` | SCHED_FIFO 50 | QNX timer delivers a pulse to a channel every 5 ms. Each tick takes one IMU sample (replayed from a recording, or from the simulated joint) into a lock-protected ring buffer and timestamps it. |
| `ctl` | SCHED_FIFO 45 | Closed loop with hysteresis. RMS above 30 mg halves the jerk limit (again every 0.5 s while high, floor 25 %). After 1 s below 10 mg it restores in x1.5 steps. The command output is simulated (logged with timestamps). |
| `ana` | SCHED_FIFO 40 | Every 10 samples (50 ms): 3-axis vibration RMS over a 256-sample (1.28 s) window, sqrt(var_x + var_y + var_z), and the FFT peak frequency of the dominant axis (Hann window, radix-2 FFT). |
| `main` | default | Console dashboard. Writes logs and timing statistics after the run. |

The real-time threads do no I/O. Everything is stored in preallocated arrays and written when the run ends, so logging cannot disturb the timing.

```mermaid
flowchart LR
    T[QNX timer, 5 ms] -- pulse --> ACQ[acq, FIFO 50]
    SRC[IMU source: WT901 replay or simulated SCARA joint] --> ACQ
    ACQ --> RB[(ring buffer)]
    RB --> ANA[ana, FIFO 40: RMS + FFT]
    ANA --> CTL[ctl, FIFO 45: jerk limit]
    CTL -- jerk limit --> SRC
    ANA --> LOG[(logs, written after run)]
    CTL --> LOG
```

Measured per run: tick jitter, ticks later than 1.5x the period, analysis time, alert latency (acquisition of the newest sample to the jerk command), and how far analysis lags acquisition.

## Input data: what is real and what is simulated

- **Real sensor data.** Three recordings from my WitMotion WT901 IMU (`data/`), logged at about 200 Hz from MATLAB. The vibration bursts in them are me moving the sensor by hand during setup, not robot motion. They are used for replay testing and as the sensor-noise source for the simulation.
- **Simulated SCARA joint** (`-s`). I did not have access to the robot during the hackathon, so the closed-loop demo uses a physics model: a jerk-limited 7-segment move profile (peak accel 3 m/s^2, nominal jerk 150 m/s^3 = 100 %) drives a lightly damped flexible mode (12 Hz, damping ratio 0.03). The IMU reading is the residual acceleration (measured minus commanded) plus real sensor noise taken from a quiet stretch of the WT901 recording. A new jerk limit applies from the next move, as a trajectory planner would. Scenario: idle 0 to 5 s, drawing 5 to 35 s, idle 35 to 45 s, drawing 45 to 60 s.

## Results

All numbers below were measured by vibguard on QNX 8.0 in real time (200 Hz). The QNX target is an x86_64 VM running under QEMU emulation on an Apple Silicon Mac, so absolute timing is limited by the emulator. Compare runs against each other, not against bare metal.

### Closed loop: guard off vs guard on (simulated joint, 60 s each)

| | Guard off (open loop) | vibguard on | Change |
|---|---|---|---|
| Mean vibration RMS while drawing | 160.7 mg | 26.4 mg | -84 % |
| Mean move time | 1.090 s | 1.204 s | +10 % |
| Moves completed | 41 | 37 | |
| Alert latency (sample to jerk command) | | 0.53 to 0.70 ms | |

This is the same trade-off my research studies: a gentler profile costs a little cycle time and removes most of the vibration.

### Noisy neighbour: do priorities protect the 200 Hz loop? (30 s each)

Two CPU-hog threads at SCHED_FIFO 15 saturate both of the VM's CPUs.

| | L0: no load | L1: hogs, vibguard at FIFO 50/45/40 | L2: hogs, vibguard at default priority 10 |
|---|---|---|---|
| Ticks later than 7.5 ms | 2 of 5999 | 0 of 5999 | 52 of 5999 |
| Longest tick interval | 12.5 ms | 2.2 ms | 36.0 ms |
| Max analysis lag | 1.6 ms | 2.1 ms | 40.1 ms |
| Alert latency | 0.68 to 0.70 ms | 0.75 to 1.00 ms | 0.87 to 1.22 ms |

![noisy neighbour](results/vibguard_noisy_neighbour.png)

Notes:
- Vibration was 25.8 mg in all three runs. The simulated moves take about 1.2 s, so stalls of tens of milliseconds did not change the control outcome here. The test shows protected timing, not a control failure.
- L2 degraded but was not fully starved, even though higher-priority hogs ran on both CPUs. I have not confirmed why; the emulated scheduler is a likely factor.
- A QNX detail I hit along the way: the receiver of a pulse runs at the pulse's priority. My first L2 run created the timer pulse at priority 50, so the acquisition thread was boosted even at default priority and stayed on time. L2 above uses `SIGEV_PULSE_PRIO_INHERIT` so the thread really runs at its own priority.

### Timing on the emulated target

| | Median | p99 |
|---|---|---|
| Tick jitter (guard on, 60 s) | 0.33 ms | 1.74 ms |
| Analysis time (RMS + FFT, 256 samples) | 0.33 ms | 0.56 ms |

QNX's real strength is deterministic timing, and an emulated VM hides it. A run under KVM hardware virtualization is planned for more representative numbers.

## Repository layout

```
src/vibguard.c          the application (QNX 8.0; also builds on Linux for logic tests)
src/timer200.c          200 Hz QNX timer test (pulse + MsgReceivePulse)
src/hello.c             hello world
data/                   WT901 IMU recordings (CSV)
results/                measured outputs, charts and summaries
docker/Dockerfile       x86_64 Linux build container for the QNX host tools
scripts/                build steps run inside the container, chart scripts
go.sh                   one-shot setup: QEMU, container, minimal QNX VM, hello + timer tests
run_vibguard.sh         build vibguard, run replay + guard off + guard on on the VM
run_loadtest.sh         noisy-neighbour test
go_arm.sh               aarch64 QNX VM (boots under emulation; see notes)
```

## Reproduce

Requirements: a free [QNX Everywhere](https://www.qnx.com/getqnx) non-commercial license, QNX SDP 8.0 with the QNX SDP Host Tools (Linux 64-bit) installed to `~/qnx800` through QNX Software Center, Docker Desktop and Homebrew. No QNX files are included in this repository.

On an Apple Silicon Mac (the setup I used):

```bash
bash go.sh            # QEMU, build container, minimal QNX VM, boot, hello + 200 Hz timer test
bash run_vibguard.sh  # build vibguard with qcc, run replay, guard off, guard on
bash run_loadtest.sh  # noisy-neighbour test
python3 scripts/plot_compare.py results/vibguard_runS0 results/vibguard_runS1 results/vibguard_before_after.png
```

QNX SDP 8.0 host tools are Linux x86_64 only, so on a Mac they run inside an x86_64 Docker container, following QNX's own macOS guide. On an x86_64 Linux host you can call `qcc` and `mkqnximage` directly and run QEMU with KVM.

Run it by hand on the QNX target:

```bash
./vibguard -s -c 1 -N data/wt901_20261005_121751.csv -n 12000 -o out   # simulated joint, guard on
./vibguard -s -c 0 -N data/wt901_20261005_121751.csv -n 12000 -o out   # guard off baseline
./vibguard -f data/wt901_20261005_121751.csv -n 8000 -o out            # replay a recording
./vibguard -s -l 2 -g 15 -o out                                         # with 2 CPU hogs
```

## Limitations and next steps

- The robot loop is simulated. The next step is the real SCARA robot: live WT901 input and a real jerk-limit command to the ESP32 motion controller.
- Timing numbers come from an emulated VM. KVM or real hardware (Raspberry Pi 5) will give representative jitter.
- An aarch64 image boots under emulation, but not yet under Apple's hardware virtualization: QNX's `startup-qemu-virt` asserts in its GICv3 code under HVF, and QEMU's HVF backend has no GICv2.

## Author

Martin Vizcaino, University of Indianapolis. [martinsvizcaino.com](https://martinsvizcaino.com), [LinkedIn](https://www.linkedin.com/in/martinvizcaino), [GitHub](https://github.com/veldig)

QNX is a trademark of BlackBerry Limited. This is an independent hackathon project.
