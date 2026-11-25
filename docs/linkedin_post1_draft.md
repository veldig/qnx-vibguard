# LinkedIn post 1 (draft, ready). Image: results/vibguard_noisy_neighbour_kvm.png

QNX just announced Physical AI and robotics demos for CES 2027. Most physical AI talk is about models. This week I worked on the layer underneath: does the control loop still run on time when the CPU is busy?

For the QNX Global Hackathon I built vibguard, a real-time vibration guard for the SCARA robot from my research: a 200 Hz IMU loop, RMS + FFT analysis, and a controller that lowers the motion's jerk limit when vibration climbs. On a simulated joint it cut vibration 84% for about 10% longer moves.

Then I tried to break it. Two threads spinning on both CPUs at a higher priority than normal work, QNX 8.0 under KVM:

Loop at priority 50, above the load: 0 of 6,000 samples late, worst 0.1 ms
Loop at default priority, below the load: blind for the full 30 s, vibration alert issued 24.9 s late

My test lied to me twice before I got there. First, my program's main thread started the CPU hogs before the control loop, got starved, and only launched the loop after the load was gone. Every "under load" result I had was really no load. Second, my late-tick metric counted that 30 s blackout as 1 late tick, because the queued timer events fired in a burst afterwards. Now every run proves the load overlapped, and every sample is checked against its schedule instead of against the previous one.

If your test can't fail, it isn't testing anything.

For people building robots: how do you prove your load tests actually load the thing you care about?

#RealTimeSystems #Robotics #EmbeddedSystems #QNX

---
First comment:
QNX CES 2027 announcement: https://www.webdisclosure.com/press-release/qnx-brings-physical-ai-and-next-generation-software-defined-vehicle-innovation-to-ces-2027-6hnwU99nx2H
Code and every measured result: https://github.com/veldig/qnx-vibguard
