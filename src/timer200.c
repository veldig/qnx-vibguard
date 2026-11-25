/*
 * timer200: 200 Hz (5 ms) periodic timer test for QNX 8.0.
 *
 * A POSIX timer on CLOCK_MONOTONIC delivers a pulse every period to a QNX
 * channel; the thread blocks in MsgReceivePulse() and timestamps each wakeup.
 * Timestamps are kept in memory during the run (so printing does not disturb
 * timing) and printed afterwards with interval/jitter statistics.
 *
 * Usage: timer200 [period_us=5000] [samples=1000] [fifo_priority=0 (0 = leave default)] [csv_path]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>

#define PULSE_CODE_TICK (_PULSE_CODE_MINAVAIL + 1)

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

int main(int argc, char **argv) {
    long period_us = argc > 1 ? atol(argv[1]) : 5000;
    int  samples   = argc > 2 ? atoi(argv[2]) : 1000;
    int  prio      = argc > 3 ? atoi(argv[3]) : 0;
    const char *csv = argc > 4 ? argv[4] : NULL;
    if (period_us <= 0 || samples < 2) { fprintf(stderr, "bad args\n"); return 1; }

    if (prio > 0) {
        struct sched_param sp = { .sched_priority = prio };
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rc != 0) fprintf(stderr, "warning: SCHED_FIFO %d failed: %s\n", prio, strerror(rc));
    }
    int policy; struct sched_param cur;
    pthread_getschedparam(pthread_self(), &policy, &cur);

    struct _clockperiod cp;
    if (ClockPeriod(CLOCK_REALTIME, NULL, &cp, 0) == 0)
        printf("System tick (ClockPeriod): %lu ns\n", (unsigned long)cp.nsec);
    printf("Period: %ld us (%.1f Hz), samples: %d, policy: %s, priority: %d\n",
           period_us, 1e6 / period_us, samples,
           policy == SCHED_FIFO ? "SCHED_FIFO" : policy == SCHED_RR ? "SCHED_RR" : "OTHER",
           cur.sched_priority);

    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (chid == -1 || coid == -1) { perror("channel"); return 1; }

    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, cur.sched_priority, PULSE_CODE_TICK, 0);
    timer_t tid;
    if (timer_create(CLOCK_MONOTONIC, &ev, &tid) == -1) { perror("timer_create"); return 1; }

    uint64_t *t = calloc((size_t)samples, sizeof *t);
    if (!t) { perror("calloc"); return 1; }

    struct itimerspec its = {0};
    its.it_value.tv_nsec    = period_us * 1000L;
    its.it_interval.tv_nsec = period_us * 1000L;
    if (period_us >= 1000000) {
        its.it_value.tv_sec = its.it_interval.tv_sec = period_us / 1000000;
        its.it_value.tv_nsec = its.it_interval.tv_nsec = (period_us % 1000000) * 1000L;
    }
    if (timer_settime(tid, 0, &its, NULL) == -1) { perror("timer_settime"); return 1; }

    struct _pulse pulse;
    for (int i = 0; i < samples; ) {
        if (MsgReceivePulse(chid, &pulse, sizeof pulse, NULL) == -1) {
            if (errno == EINTR) continue;
            perror("MsgReceivePulse"); return 1;
        }
        if (pulse.code == PULSE_CODE_TICK) t[i++] = now_ns();
    }
    timer_delete(tid);

    /* Print every timestamp (collected at 200 Hz), relative to the first tick. */
    FILE *f = csv ? fopen(csv, "w") : NULL;
    if (f) fprintf(f, "index,t_ms,interval_us\n");
    printf("index  t_ms        interval_us\n");
    int n = samples - 1;
    double *dt = malloc(sizeof(double) * n);
    double *jit = malloc(sizeof(double) * n);
    for (int i = 0; i < samples; i++) {
        double tms = (t[i] - t[0]) / 1e6;
        double iv = i ? (t[i] - t[i-1]) / 1e3 : 0.0;
        printf("%5d  %10.3f  %10.1f\n", i, tms, iv);
        if (f) fprintf(f, "%d,%.6f,%.3f\n", i, tms, iv);
        if (i) { dt[i-1] = iv; jit[i-1] = fabs(iv - period_us); }
    }
    if (f) fclose(f);

    double sum = 0, sq = 0, mn = dt[0], mx = dt[0];
    int late10 = 0;
    for (int i = 0; i < n; i++) {
        sum += dt[i]; sq += dt[i] * dt[i];
        if (dt[i] < mn) mn = dt[i];
        if (dt[i] > mx) mx = dt[i];
        if (jit[i] > 0.10 * period_us) late10++;
    }
    double mean = sum / n, sd = sqrt(fmax(0.0, sq / n - mean * mean));
    qsort(jit, n, sizeof(double), cmp_d);
    double total_s = (t[samples-1] - t[0]) / 1e9;

    printf("\n=== SUMMARY (measured on this run) ===\n");
    printf("intervals: %d, total %.3f s, effective rate %.2f Hz\n", n, total_s, n / total_s);
    printf("interval us: mean %.1f  sd %.1f  min %.1f  max %.1f\n", mean, sd, mn, mx);
    printf("|jitter| us: median %.1f  p99 %.1f  max %.1f\n", jit[n/2], jit[(int)(0.99 * (n-1))], jit[n-1]);
    printf("intervals off by >10%% of period: %d (%.2f%%)\n", late10, 100.0 * late10 / n);
    free(t); free(dt); free(jit);
    return 0;
}
