# Devpost draft: vibguard

## Project name
vibguard: real-time vibration guard for a SCARA robot

## Elevator pitch (max 200 characters)
A QNX 8.0 app that watches a robot arm's IMU at 200 Hz and closes the loop: when vibration spikes it lowers the jerk limit within a millisecond. Simulated arm: 84% less vibration.

## Category
Robotics & Medical

## Built with
c, qnx, qnx-sdp-8.0, qcc, mkqnximage, qemu, posix, docker, python, matplotlib

## Links
- Code: https://github.com/veldig/qnx-vibguard
- Video: (YouTube link, to add)

---

## About the project

### Inspiration
I research vibration suppression on a 6-DOF SCARA robot I built (ESP32, C++). The idea is simple: an arm that accelerates too abruptly rings like a bell, and the vibration shows up as wobbly lines when it draws. Limiting jerk (the rate of change of acceleration) and shaping the motion commands cut that vibration, but my workflow was offline: record the IMU, analyse it in MATLAB, adjust the motion profile, run again. I wanted the robot to protect itself while it moves. That needs measurement, analysis and a decision inside a fixed time budget, every time, which is exactly what a real-time OS is for.

### What it does
vibguard reads a 3-axis IMU at 200 Hz, computes vibration in real time, and closes the loop on the motion planner:

- Every 5 ms a QNX timer wakes the acquisition thread, which takes one IMU sample.
- Every 50 ms the analysis thread computes the 3-axis vibration RMS over the last 1.28 s and the dominant vibration frequency with an FFT.
- When RMS crosses 30 mg, the control thread halves the jerk limit of the motion profile (down to a 25% floor). When the arm stays below 10 mg for a second, it restores the limit in steps.
- It measures itself: tick jitter, late ticks, analysis time, alert latency (newest sample to jerk command), and how far analysis lags acquisition. All of it is logged and plotted.

Input can be a replay of real WT901 IMU recordings, or a simulated SCARA joint whose vibration actually responds to the jerk limit, so the loop is genuinely closed.

### How I built it
- **QNX 8.0 target:** a minimal QNX 8.0 system image built from scratch with `mkqnximage` (x86_64, QEMU), booted on my Mac.
- **Toolchain:** QNX SDP 8.0 `qcc` (gcc_ntox86_64, also builds for aarch64le). QNX host tools are Linux x86_64 only, so I followed QNX's macOS guide and run them in an x86_64 Docker container. Everything is scripted: one command sets up the toolchain, builds the image, boots it and runs the tests.
- **Architecture:** three real-time threads plus a non-real-time dashboard. The acquisition thread blocks in `MsgReceivePulse()` on a private channel; a `timer_create()` POSIX timer on `CLOCK_MONOTONIC` delivers a pulse every 5 ms. Threads run under `SCHED_FIFO` at 50 (acquisition), 45 (control) and 40 (analysis). The real-time threads never do I/O: samples, results and events go into preallocated arrays and are written after the run.
- **Simulated SCARA joint:** a jerk-limited 7-segment move profile drives a 12 Hz, lightly damped flexible mode. The simulated IMU adds real sensor noise taken from my WT901 recordings. New jerk limits apply from the next move, like a real trajectory planner.

### Where QNX is used (for reviewers)
- `src/vibguard.c`, `acq_thread()`: `ChannelCreate`, `ConnectAttach`, `SIGEV_PULSE_INIT`, `timer_create`/`timer_settime`, `MsgReceivePulse` drive the 200 Hz loop.
- `src/vibguard.c`, `set_prio()` and the thread functions: `SCHED_FIFO` priorities 50/45/40; the summary prints each thread's effective policy and priority.
- `src/vibguard.c`: `SIGEV_PULSE_PRIO_INHERIT` for the default-priority experiment (see Challenges).
- `src/timer200.c`: 200 Hz pulse-timer test, reads the system tick with `ClockPeriod()`.
- `go.sh`, `scripts/build_in_container.sh`: QNX image built with `mkqnximage --type=qemu`; `scripts/build_vibguard.sh`: built with `qcc`.
- `results/`: logs and charts from runs on the QNX 8.0 target.

### Results (measured on QNX 8.0, QEMU x86_64 under emulation)
**Closed loop, simulated joint, 60 s each:**

| | Guard off | vibguard on |
|---|---|---|
| Vibration RMS while drawing | 160.7 mg | 26.4 mg (-84%) |
| Mean move time | 1.090 s | 1.204 s (+10%) |
| Alert latency | | 0.53 to 0.70 ms |

That is the trade-off my research studies: a slightly slower, gentler profile removes most of the vibration.

**Noisy neighbour (KVM, 30 s each):** two CPU-hog threads at FIFO 15 spin on both CPUs for the whole run.

| | No load | Hogs, vibguard at FIFO 50/45/40 | Hogs, vibguard at default priority | Hogs at FIFO 60, above vibguard |
|---|---|---|---|---|
| Samples more than 2.5 ms behind schedule | 0 of 6000 | 0 of 6000 | 5998 of 6000 | 5998 of 6000 |
| Worst sample behind schedule | 0.1 ms | 0.1 ms | 30.0 s | 30.0 s |
| Vibration alert after the sample's scheduled time | same as no load | same as no load | 24.9 s late | 23.4 s late |

**Timing, same image, emulated vs KVM (Google Cloud, nested virtualization):** tick jitter p99 1.74 ms vs 100.5 us; timer intervals off by more than 10%: 42.9% vs 0%; analysis p99 0.56 ms vs 41.6 us; sample-to-command latency 0.53 to 0.70 ms vs 44 to 93 us.

### Challenges I ran into
- **No native macOS host tools for QNX SDP 8.0.** I set up QNX Software Center on macOS, installed the Linux host tools, and ran `qcc` and `mkqnximage` inside an x86_64 Docker container.
- **Apple Silicon hardware virtualization.** I built an aarch64 QNX image to run it on Apple's hypervisor. Under HVF, `startup-qemu-virt` asserts in its GICv3 code, and QEMU's HVF backend has no GICv2, so the target runs under emulation for now.
- **My load test lied, twice.** First, vibguard's default-priority main thread created the FIFO hogs before the control threads, got starved, and only started the loop after the hogs finished, so every load run actually had no load. A `pidin` snapshot showing the hogs already dead gave it away. Second, my interval-based "late tick" metric reported 1 late tick for a run that was blind for 30 s, because queued timer pulses drained in a burst. I fixed the ordering, print each hog's time window as proof of overlap, and now measure samples and alerts against their schedule.
- **Pulse priority.** In QNX, a thread that receives a pulse runs at the pulse's priority. My timer pulse carried priority 50, which would boost the acquisition thread even in the "default priority" experiment, so that experiment uses `SIGEV_PULSE_PRIO_INHERIT`.
- **Measuring the wrong thing.** My first analysis used the magnitude of the acceleration vector, which barely moves when a joint vibrates sideways to gravity. I switched to a 3-axis variance-based RMS.
- **No robot access during the hackathon.** I built a physics-based joint simulation, calibrated with real sensor noise, so the closed loop has a plant that actually responds.

### Accomplishments that I'm proud of
- A working closed loop on QNX 8.0 that cuts vibration by 84% in simulation for a 10% cycle-time cost.
- A measured demonstration that QNX priorities protect the 200 Hz loop under CPU load.
- A fully scripted, reproducible setup on a Mac: from a clean machine to a booted QNX 8.0 target running the tests.

### What I learned
How QNX's message-passing primitives (channels, pulses, priority inheritance through pulses) shape real-time design, how to build a QNX system image from scratch with `mkqnximage`, and how much measurement design matters: an emulator, the wrong metric, or a boosted thread can all hide what you are trying to show.

### What's next for vibguard
- Run it on the real SCARA robot: live WT901 input and a real jerk-limit command to the ESP32 motion controller.
- Bare-metal timing on a Raspberry Pi 5 or x86 board.
- Per-axis and frequency-aware control, for example notch-shaping the profile around the detected resonance from the FFT.

---

## Build and run instructions (submission field)
- QNX version: QNX SDP 8.0 (QNX OS 8.0.0), free QNX Everywhere non-commercial license.
- Target: QEMU x86_64 system image built with `mkqnximage --type=qemu --arch=x86_64` (2 vCPUs, 1 GB).
- Toolchain: QNX SDP 8.0 `qcc -Vgcc_ntox86_64` (GCC 12.2.0, QNX SDP Host Tools Linux 64-bit), run inside an x86_64 Ubuntu 22.04 Docker container on macOS (Apple Silicon).
- Steps: install SDP 8.0 + Linux host tools to `~/qnx800`, then `bash go.sh` (toolchain, image, boot, tests), `bash run_vibguard.sh` (vibguard runs), `bash run_loadtest.sh` (noisy neighbour). Details in the README.
