/*
 * vibguard: real-time vibration guard for a SCARA robot, targeting QNX 8.0.
 *
 * Threads (highest priority first):
 *   acq  (SCHED_FIFO 50): periodic timer (QNX pulse on a channel), replays one IMU
 *        sample per tick from a recorded WT901 CSV into a lock-protected ring buffer.
 *   ctl  (SCHED_FIFO 45): closed loop. Lowers the motion profile's jerk limit when
 *        vibration RMS crosses a high threshold, restores it after it stays below a
 *        low threshold (hysteresis). Command output is simulated (logged).
 *   ana  (SCHED_FIFO 40): every HOP samples, computes the 3-axis vibration RMS
 *        (sqrt of summed per-axis variance) and the FFT peak frequency of the dominant
 *        axis over a sliding window.
 *   main (default prio): console dashboard, then writes logs and timing statistics.
 *
 * Nothing in the real-time threads does I/O; everything is stored in preallocated
 * arrays and written after the run. All reported numbers are measured by this program.
 *
 * Build (QNX):  qcc -Vgcc_ntox86_64 -O2 -o vibguard vibguard.c -lm
 * Build (Linux, for logic testing): gcc -O2 -o vibguard vibguard.c -lm -lpthread
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <signal.h>
#ifdef __QNX__
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#endif

/* ---------------- configuration ---------------- */
typedef struct {
    const char *csv;
    const char *outdir;
    double rate_hz;      /* replay rate */
    double speedup;      /* >1 replays faster than real time (logic testing only) */
    int    win;          /* analysis window (samples, power of 2) */
    int    hop;          /* run analysis every hop samples */
    double hi_mg, lo_mg; /* thresholds on RMS (milli-g) */
    double hold_s;       /* time below lo before restoring */
    double step_s;       /* min time between jerk changes */
    double jerk_min_pct; /* floor for jerk limit */
    long   max_samples;  /* 0 = whole file */
    int    prio_acq, prio_ctl, prio_ana;
    int    quiet;
    int    sim;          /* 1 = simulated SCARA joint instead of CSV replay */
    int    closed_loop;  /* 0 = controller only observes (baseline) */
    const char *noise_csv; /* optional: real WT901 recording used as sensor-noise bank */
    int    hogs;         /* noisy-neighbour CPU hog threads */
    int    hog_prio;     /* their SCHED_FIFO priority (0 = default policy) */
    int    rt;           /* 1 = vibguard threads use SCHED_FIFO priorities, 0 = default policy */
} cfg_t;

static cfg_t C = {
    .csv = NULL, .outdir = ".", .rate_hz = 200.0, .speedup = 1.0,
    .win = 256, .hop = 10, .hi_mg = 30.0, .lo_mg = 10.0, .hold_s = 1.0, .step_s = 0.5,
    .jerk_min_pct = 25.0, .max_samples = 0, .prio_acq = 50, .prio_ctl = 45, .prio_ana = 40,
    .quiet = 0, .sim = 0, .closed_loop = 1, .noise_csv = NULL, .hogs = 0, .hog_prio = 20, .rt = 1,
};

/* ---------------- data ---------------- */
static float *g_ax, *g_ay, *g_az;   /* replay source (g) */
static long   g_nrows;

#define RING 8192                   /* power of 2, > win */
typedef struct { float a[3]; uint64_t t_ns; long idx; } sample_t;
static sample_t ring[RING];
static long     ring_count;         /* total samples written */
static pthread_mutex_t ring_mx = PTHREAD_MUTEX_INITIALIZER;

static sem_t sem_ana, sem_ctl;
static volatile int g_done;

/* timing records (preallocated) */
static uint64_t *acq_t;             /* timestamp of every tick */
static long      acq_n;

typedef struct {
    double t_s;          /* time of analysis (s from start) */
    long   last_idx;     /* newest sample index in window */
    uint64_t t_last_ns;  /* acquisition time of newest sample */
    uint64_t t_done_ns;  /* analysis finished */
    float  rms_mg, peak_hz, ana_us;
} ana_rec_t;
static ana_rec_t *ana;
static long ana_n, ana_cap;
static long ana_consumed;           /* next result for ctl */
static pthread_mutex_t ana_mx = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    double t_s; long sample_idx; float rms_mg; float jerk_from, jerk_to;
    int kind;            /* 1 = alert/lower, 2 = further lower, 3 = restore step */
    double latency_us;   /* newest sample acquired -> command issued */
} evt_t;
static evt_t evts[4096];
static long nevt;

static volatile float g_jerk_pct = 100.0f;
static volatile int   g_state;      /* 0 normal, 1 alert, 2 recovering */
static volatile float g_rms, g_peak;
static uint64_t t0_ns;
static volatile int g_eff_prio[3], g_eff_pol[3];

static inline uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---------------- CSV ---------------- */
static int load_csv(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    char line[1024];
    if (!fgets(line, sizeof line, f)) { fclose(f); return -1; }
    /* find columns */
    int cax = -1, cay = -1, caz = -1, col = 0;
    for (char *tok = strtok(line, ",\r\n"); tok; tok = strtok(NULL, ",\r\n"), col++) {
        if (!strcmp(tok, "ax_g")) cax = col;
        else if (!strcmp(tok, "ay_g")) cay = col;
        else if (!strcmp(tok, "az_g")) caz = col;
    }
    if (cax < 0 || cay < 0 || caz < 0) { fprintf(stderr, "CSV needs ax_g, ay_g, az_g columns\n"); fclose(f); return -1; }
    long cap = 1 << 16, n = 0;
    g_ax = malloc(cap * sizeof(float)); g_ay = malloc(cap * sizeof(float)); g_az = malloc(cap * sizeof(float));
    while (fgets(line, sizeof line, f)) {
        if (n == cap) {
            cap *= 2;
            g_ax = realloc(g_ax, cap * sizeof(float)); g_ay = realloc(g_ay, cap * sizeof(float)); g_az = realloc(g_az, cap * sizeof(float));
        }
        double v[16] = {0}; int c = 0; char *p = line;
        while (c < 16) {
            v[c++] = strtod(p, NULL);
            char *comma = strchr(p, ',');
            if (!comma) break;
            p = comma + 1;
        }
        if (c <= caz) continue;
        g_ax[n] = (float)v[cax]; g_ay[n] = (float)v[cay]; g_az[n] = (float)v[caz];
        n++;
    }
    fclose(f);
    g_nrows = n;
    return n > 0 ? 0 : -1;
}

/* ---------------- FFT (radix-2, in place) ---------------- */
static void fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / len;
        float wr = (float)cos(ang), wi = (float)sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = a + len / 2;
                float xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr; im[a] += xi;
                float ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}


/* ---------------- simulated SCARA joint (plant) ----------------
 * A jerk-limited point-to-point motion profile drives a lightly damped flexible mode
 * (base excitation):  x'' + 2*z*w*x' + w^2*x = -a_cmd.
 * The IMU measures commanded acceleration + mode acceleration + sensor noise. Because the
 * controller knows a_cmd, it analyses the residual (measured - commanded): the vibration.
 * New jerk limits apply at the start of the next move, like a real trajectory planner.
 * Scenario (repeats every 60 s): 0-5 s idle, 5-35 s drawing, 35-45 s idle, 45-60 s drawing.
 */
#define SIM_F0   12.0     /* Hz, flexible mode */
#define SIM_Z    0.03     /* damping ratio */
#define SIM_A    3.0      /* m/s^2 peak accel */
#define SIM_DV   1.2      /* m/s per accel phase */
#define SIM_TC   0.15     /* s cruise */
#define SIM_J0   150.0    /* m/s^3 nominal jerk (100 %) */
#define SIM_SUB  20
typedef struct { double dur, jerk; } seg_t;
static struct {
    double x, v, a;            /* mode state, commanded accel */
    seg_t seg[8]; int nseg, si; double seg_left;
    long moves_done; double move_time_sum, move_t0;
    float *nb[3]; long nn, ni; /* noise bank (g) */
    unsigned rng;
} P;
static int sim_drawing(double t) { double m = fmod(t, 60.0); return (m >= 5 && m < 35) || (m >= 45); }
static double gauss(void) { /* Box-Muller on a small LCG: deterministic runs */
    P.rng = P.rng * 1103515245u + 12345u; double u1 = ((P.rng >> 8) + 1.0) / 16777217.0;
    P.rng = P.rng * 1103515245u + 12345u; double u2 = ((P.rng >> 8) + 1.0) / 16777217.0;
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}
static void sim_plan_move(double J) {
    double Ap = SIM_A, tj = Ap / J, ta = SIM_DV / Ap - tj;
    if (ta < 0) { Ap = sqrt(SIM_DV * J); tj = Ap / J; ta = 0; }
    seg_t s[8] = { {tj, +J}, {ta, 0}, {tj, -J}, {SIM_TC, 0}, {tj, -J}, {ta, 0}, {tj, +J}, {0.10, 0} };
    memcpy(P.seg, s, sizeof s); P.nseg = 8; P.si = 0; P.seg_left = P.seg[0].dur;
}
static void sim_init(void) {
    memset(&P, 0, sizeof P); P.rng = 12345u; P.nseg = 0;
    if (C.noise_csv) {   /* reuse the CSV loader, then keep deviations from each axis mean */
        float *ax = g_ax, *ay = g_ay, *az = g_az; long n = g_nrows;
        if (load_csv(C.noise_csv) == 0) {
            long a = g_nrows > 8000 ? 3000 : 0, b = g_nrows > 8000 ? 8000 : g_nrows; /* quiet stretch */
            P.nn = b - a;
            for (int k = 0; k < 3; k++) P.nb[k] = malloc(P.nn * sizeof(float));
            double m[3] = {0};
            for (long i = a; i < b; i++) { m[0] += g_ax[i]; m[1] += g_ay[i]; m[2] += g_az[i]; }
            for (int k = 0; k < 3; k++) m[k] /= P.nn;
            for (long i = a; i < b; i++) {
                P.nb[0][i - a] = g_ax[i] - m[0]; P.nb[1][i - a] = g_ay[i] - m[1]; P.nb[2][i - a] = g_az[i] - m[2];
            }
            free(g_ax); free(g_ay); free(g_az);
        }
        g_ax = ax; g_ay = ay; g_az = az; g_nrows = n;
    }
}
/* advance one sample period; returns residual accel vector (g) the IMU would report minus a_cmd */
static void sim_step(double t, float jerk_pct, double dt, float out[3]) {
    double h = dt / SIM_SUB, w = 2 * M_PI * SIM_F0, xdd = 0;
    int drawing = sim_drawing(t);
    for (int k = 0; k < SIM_SUB; k++) {
        if (P.si >= P.nseg) {              /* between moves: start a new one if drawing */
            if (drawing) { sim_plan_move(SIM_J0 * jerk_pct / 100.0); P.move_t0 = t + k * h; }
            else { P.nseg = 0; P.si = 0; P.a = 0; }
        }
        double jerk = 0;
        if (P.si < P.nseg) {
            jerk = P.seg[P.si].jerk; P.seg_left -= h;
            if (P.seg_left <= 0) {
                P.si++;
                if (P.si < P.nseg) P.seg_left += P.seg[P.si].dur;
                else { P.a = 0; P.moves_done++; P.move_time_sum += t + (k + 1) * h - P.move_t0; }
            }
        }
        P.a += jerk * h;
        xdd = -2 * SIM_Z * w * P.v - w * w * P.x - P.a;
        P.v += xdd * h; P.x += P.v * h;
    }
    float n0, n1, n2;
    if (P.nn) { n0 = P.nb[0][P.ni]; n1 = P.nb[1][P.ni]; n2 = P.nb[2][P.ni]; P.ni = (P.ni + 1) % P.nn; }
    else { n0 = 0.0007f * (float)gauss(); n1 = 0.0007f * (float)gauss(); n2 = 0.0007f * (float)gauss(); }
    out[0] = (float)(xdd / 9.81) + n0;   /* mode acts along the arm's tangential axis */
    out[1] = n1;
    out[2] = 1.0f + n2;                  /* gravity */
}

/* ---------------- threads ---------------- */
static int set_prio(int prio) {
    if (!C.rt) return 0;                 /* -R: stay at the default policy/priority */
    struct sched_param sp = { .sched_priority = prio };
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

static void *acq_thread(void *arg) {
    (void)arg;
    if (set_prio(C.prio_acq) && !C.quiet) fprintf(stderr, "note: acq SCHED_FIFO %d not set\n", C.prio_acq);
    { int pol; struct sched_param sp; pthread_getschedparam(pthread_self(), &pol, &sp); g_eff_prio[0] = sp.sched_priority; g_eff_pol[0] = pol; }
    long total = C.sim ? C.max_samples : (C.max_samples > 0 && C.max_samples < g_nrows ? C.max_samples : g_nrows);
    long period_ns = (long)(1e9 / (C.rate_hz * C.speedup));
#ifdef __QNX__
    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent ev;
    /* With RT priorities the timer pulse carries priority 50. With -R the pulse inherits the
       receiver's own priority, so the thread really runs at the default priority. */
    SIGEV_PULSE_INIT(&ev, coid, C.rt ? C.prio_acq : SIGEV_PULSE_PRIO_INHERIT, _PULSE_CODE_MINAVAIL + 1, 0);
    timer_t tid;
    timer_create(CLOCK_MONOTONIC, &ev, &tid);
    struct itimerspec its = {0};
    its.it_value.tv_sec = its.it_interval.tv_sec = period_ns / 1000000000L;
    its.it_value.tv_nsec = its.it_interval.tv_nsec = period_ns % 1000000000L;
    timer_settime(tid, 0, &its, NULL);
    struct _pulse pulse;
#else
    struct timespec next; clock_gettime(CLOCK_MONOTONIC, &next);
#endif
    for (long i = 0; i < total && !g_done; i++) {
#ifdef __QNX__
        while (MsgReceivePulse(chid, &pulse, sizeof pulse, NULL) == -1 && errno == EINTR) {}
#else
        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) {}
#endif
        uint64_t t = now_ns();
        acq_t[acq_n++] = t;
        float ax, ay, az;
        if (C.sim) { float o[3]; sim_step(i / C.rate_hz, C.closed_loop ? g_jerk_pct : 100.0f, 1.0 / C.rate_hz, o); ax = o[0]; ay = o[1]; az = o[2]; }
        else { ax = g_ax[i]; ay = g_ay[i]; az = g_az[i]; }
        sample_t s = { { ax, ay, az }, t, i };
        pthread_mutex_lock(&ring_mx);
        ring[ring_count & (RING - 1)] = s;
        ring_count++;
        long rc = ring_count;
        pthread_mutex_unlock(&ring_mx);
        if (rc >= C.win && rc % C.hop == 0) sem_post(&sem_ana);
    }
#ifdef __QNX__
    timer_delete(tid);
#endif
    g_done = 1;
    sem_post(&sem_ana);
    return NULL;
}

static void *ana_thread(void *arg) {
    (void)arg;
    if (set_prio(C.prio_ana) && !C.quiet) fprintf(stderr, "note: ana SCHED_FIFO %d not set\n", C.prio_ana);
    { int pol; struct sched_param sp; pthread_getschedparam(pthread_self(), &pol, &sp); g_eff_prio[1] = sp.sched_priority; g_eff_pol[1] = pol; }
    int W = C.win;
    float *re = malloc(W * sizeof(float)), *im = malloc(W * sizeof(float)), *hann = malloc(W * sizeof(float));
    sample_t *win = malloc(W * sizeof(sample_t));
    for (int i = 0; i < W; i++) hann[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (W - 1));
    double fs = C.rate_hz;   /* analysis is in "sample time": the recording's 200 Hz */
    for (;;) {
        sem_wait(&sem_ana);
        if (g_done) {
            /* drain: one final pass not needed */
            break;
        }
        uint64_t ts = now_ns();
        pthread_mutex_lock(&ring_mx);
        long rc = ring_count;
        for (int i = 0; i < W; i++) win[i] = ring[(rc - W + i) & (RING - 1)];
        pthread_mutex_unlock(&ring_mx);

        /* 3-axis vibration RMS: sqrt(var_x + var_y + var_z) of the window (gravity and offsets removed) */
        double mean[3] = {0}, var[3] = {0};
        for (int i = 0; i < W; i++) for (int k = 0; k < 3; k++) mean[k] += win[i].a[k];
        for (int k = 0; k < 3; k++) mean[k] /= W;
        for (int i = 0; i < W; i++) for (int k = 0; k < 3; k++) { double d = win[i].a[k] - mean[k]; var[k] += d * d; }
        int kx = 0; for (int k = 1; k < 3; k++) if (var[k] > var[kx]) kx = k;   /* dominant axis */
        double rms_mg = sqrt((var[0] + var[1] + var[2]) / W) * 1000.0;
        for (int i = 0; i < W; i++) { re[i] = (float)(win[i].a[kx] - mean[kx]) * hann[i]; im[i] = 0; }
        fft(re, im, W);
        int kmax = 1; float pmax = -1;
        for (int k = 1; k < W / 2; k++) { float p = re[k] * re[k] + im[k] * im[k]; if (p > pmax) { pmax = p; kmax = k; } }
        double peak_hz = kmax * fs / W;
        uint64_t te = now_ns();

        pthread_mutex_lock(&ana_mx);
        if (ana_n < ana_cap) {
            ana_rec_t *r = &ana[ana_n++];
            r->t_s = (ts - t0_ns) / 1e9; r->last_idx = win[W - 1].idx; r->t_last_ns = win[W - 1].t_ns;
            r->t_done_ns = te; r->rms_mg = (float)rms_mg; r->peak_hz = (float)peak_hz; r->ana_us = (te - ts) / 1e3f;
        }
        pthread_mutex_unlock(&ana_mx);
        g_rms = (float)rms_mg; g_peak = (float)peak_hz;
        sem_post(&sem_ctl);
    }
    g_done = 2;
    sem_post(&sem_ctl);
    free(re); free(im); free(hann); free(win);
    return NULL;
}

static void *ctl_thread(void *arg) {
    (void)arg;
    if (set_prio(C.prio_ctl) && !C.quiet) fprintf(stderr, "note: ctl SCHED_FIFO %d not set\n", C.prio_ctl);
    { int pol; struct sched_param sp; pthread_getschedparam(pthread_self(), &pol, &sp); g_eff_prio[2] = sp.sched_priority; g_eff_pol[2] = pol; }
    double last_change_s = -1e9, below_since_s = -1;
    float jerk = 100.0f;
    for (;;) {
        sem_wait(&sem_ctl);
        pthread_mutex_lock(&ana_mx);
        int have = ana_consumed < ana_n;
        ana_rec_t r; if (have) r = ana[ana_consumed++];
        pthread_mutex_unlock(&ana_mx);
        if (!have) { if (g_done == 2) break; continue; }
        if (!C.closed_loop) continue;   /* baseline: guard observes only, never commands */
        /* control decisions use "sample time" so behaviour does not depend on speedup */
        double ts = r.last_idx / C.rate_hz;
        int kind = 0; float from = jerk;
        if (r.rms_mg > C.hi_mg) {
            below_since_s = -1;
            if (g_state == 0) { jerk = fmaxf(jerk * 0.5f, (float)C.jerk_min_pct); kind = 1; g_state = 1; }
            else if (ts - last_change_s >= C.step_s && jerk > C.jerk_min_pct) { jerk = fmaxf(jerk * 0.5f, (float)C.jerk_min_pct); kind = 2; g_state = 1; }
        } else if (r.rms_mg < C.lo_mg && jerk < 100.0f) {
            if (below_since_s < 0) below_since_s = ts;
            if (ts - below_since_s >= C.hold_s && ts - last_change_s >= C.step_s) {
                jerk = fminf(jerk * 1.5f, 100.0f); kind = 3; g_state = jerk >= 100.0f ? 0 : 2;
            }
        } else if (r.rms_mg >= C.lo_mg) {
            below_since_s = -1;
        }
        if (kind) {
            uint64_t tc = now_ns();   /* "issue" the simulated jerk-limit command */
            g_jerk_pct = jerk;
            last_change_s = ts;
            if (nevt < (long)(sizeof evts / sizeof evts[0])) {
                evt_t *e = &evts[nevt++];
                e->t_s = (tc - t0_ns) / 1e9; e->sample_idx = r.last_idx; e->rms_mg = r.rms_mg;
                e->jerk_from = from; e->jerk_to = jerk; e->kind = kind;
                e->latency_us = (tc - r.t_last_ns) / 1e3;
            }
        }
    }
    return NULL;
}


/* noisy neighbour: spins on a CPU for a fixed wall-clock time, independent of vibguard */
static volatile double g_hog_secs;
static volatile unsigned long g_hog_spins;
static volatile int g_hog_rc[64];
static void *hog_thread(void *arg) {
    int id = (int)(intptr_t)arg;
    if (C.hog_prio > 0) {
        struct sched_param sp = { .sched_priority = C.hog_prio };
        g_hog_rc[id] = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }
    uint64_t end = now_ns() + (uint64_t)(g_hog_secs * 1e9);
    unsigned long n = 0;
    while (now_ns() < end) { for (volatile int i = 0; i < 1000; i++) {} n++; }
    __sync_fetch_and_add(&g_hog_spins, n);
    return NULL;
}

/* ---------------- stats helpers ---------------- */
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static void stats(const char *name, double *v, long n, FILE *out) {
    if (n <= 0) { fprintf(out, "%-28s n=0\n", name); return; }
    double s = 0, mx = v[0], mn = v[0];
    for (long i = 0; i < n; i++) { s += v[i]; if (v[i] > mx) mx = v[i]; if (v[i] < mn) mn = v[i]; }
    qsort(v, n, sizeof(double), cmpd);
    fprintf(out, "%-28s n=%-6ld mean=%9.1f  min=%9.1f  p50=%9.1f  p99=%9.1f  max=%9.1f\n",
            name, n, s / n, mn, v[n / 2], v[(long)ceil(0.99 * (n - 1))], mx);
}

static void usage(void) {
    fprintf(stderr,
        "usage: vibguard -f data.csv [-o outdir] [-r 200] [-x speedup] [-w 256] [-k hop]\n"
        "                [-H hi_mg] [-L lo_mg] [-n max_samples] [-q]\n"
        "       vibguard -s [-c 1|0 closed loop] [-N noise.csv] [-n samples] ...   (simulated SCARA joint)\n"
        "       load test: [-l hog_threads] [-g hog_fifo_prio] [-R vibguard at default priority]\n");
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "f:o:r:x:w:k:H:L:n:qhsc:N:l:g:R")) != -1) {
        switch (opt) {
        case 'f': C.csv = optarg; break;
        case 'o': C.outdir = optarg; break;
        case 'r': C.rate_hz = atof(optarg); break;
        case 'x': C.speedup = atof(optarg); break;
        case 'w': C.win = atoi(optarg); break;
        case 'k': C.hop = atoi(optarg); break;
        case 'H': C.hi_mg = atof(optarg); break;
        case 'L': C.lo_mg = atof(optarg); break;
        case 'n': C.max_samples = atol(optarg); break;
        case 'q': C.quiet = 1; break;
        case 's': C.sim = 1; break;
        case 'c': C.closed_loop = atoi(optarg); break;
        case 'N': C.noise_csv = optarg; break;
        case 'l': C.hogs = atoi(optarg); break;
        case 'g': C.hog_prio = atoi(optarg); break;
        case 'R': C.rt = 0; break;
        default: usage(); return 1;
        }
    }
    if ((!C.csv && !C.sim) || (C.win & (C.win - 1)) || C.win > RING / 2 || C.hop < 1) { usage(); return 1; }
    if (C.sim) { if (C.max_samples <= 0) C.max_samples = 12000; C.csv = "simulated SCARA joint"; g_nrows = C.max_samples; sim_init(); }
    else if (load_csv(C.csv)) return 1;
    long total = C.max_samples > 0 && C.max_samples < g_nrows ? C.max_samples : g_nrows;
    acq_t = calloc(total + 1, sizeof(uint64_t));
    ana_cap = total / C.hop + 16; ana = calloc(ana_cap, sizeof(ana_rec_t));

    printf("vibguard: %s, %ld samples (%.1f s at %.0f Hz), speedup x%.1f\n", C.csv, total, total / C.rate_hz, C.rate_hz, C.speedup);
    if (C.sim) printf("mode: SIMULATED SCARA joint (%.0f Hz mode, zeta %.2f), %s loop, noise: %s\n",
                      SIM_F0, SIM_Z, C.closed_loop ? "CLOSED" : "OPEN (baseline)", C.noise_csv ? C.noise_csv : "gaussian 0.7 mg");
    printf("window %d samples (%.2f s), analysis every %d samples, thresholds hi %.1f mg / lo %.1f mg\n",
           C.win, C.win / C.rate_hz, C.hop, C.hi_mg, C.lo_mg);

    printf("priorities: %s; noisy neighbours: %d CPU hog thread(s) at %s %d\n",
           C.rt ? "vibguard SCHED_FIFO acq 50 / ctl 45 / ana 40" : "vibguard at DEFAULT priority (-R)",
           C.hogs, C.hog_prio > 0 ? "SCHED_FIFO" : "default prio", C.hog_prio);
    sem_init(&sem_ana, 0, 0); sem_init(&sem_ctl, 0, 0);
    t0_ns = now_ns();
    pthread_t th[64]; int nh = C.hogs > 64 ? 64 : C.hogs;
    g_hog_secs = total / (C.rate_hz * C.speedup);
    for (int i = 0; i < nh; i++) pthread_create(&th[i], NULL, hog_thread, (void *)(intptr_t)i);
    pthread_t ta, tn, tc;
    pthread_create(&tc, NULL, ctl_thread, NULL);
    pthread_create(&tn, NULL, ana_thread, NULL);
    pthread_create(&ta, NULL, acq_thread, NULL);

    /* dashboard (non real-time) */
    const char *stname[] = { "NORMAL", "ALERT", "RECOVER" };
    while (g_done == 0) {
        usleep(500000);
        if (!C.quiet)
            printf("t=%7.2fs  samples=%7ld  rms=%7.1f mg  peak=%5.1f Hz  jerk_limit=%5.1f%%  state=%s\n",
                   (now_ns() - t0_ns) / 1e9, ring_count, g_rms, g_peak, g_jerk_pct, stname[g_state]);
        fflush(stdout);
    }
    pthread_join(ta, NULL); pthread_join(tn, NULL); pthread_join(tc, NULL);
    for (int i = 0; i < nh; i++) pthread_join(th[i], NULL);

    /* ---- write logs ---- */
    char path[1024];
    snprintf(path, sizeof path, "%s/vibguard_analysis.csv", C.outdir);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "t_s,sample_idx,sample_t_s,rms_mg,peak_hz,analysis_us\n");
        for (long i = 0; i < ana_n; i++)
            fprintf(f, "%.4f,%ld,%.4f,%.3f,%.2f,%.2f\n", ana[i].t_s, ana[i].last_idx, ana[i].last_idx / C.rate_hz,
                    ana[i].rms_mg, ana[i].peak_hz, ana[i].ana_us);
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/vibguard_events.csv", C.outdir);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "t_s,sample_idx,sample_t_s,kind,rms_mg,jerk_from_pct,jerk_to_pct,latency_us\n");
        const char *kn[] = { "", "alert_lower", "lower_more", "restore_step" };
        for (long i = 0; i < nevt; i++)
            fprintf(f, "%.4f,%ld,%.4f,%s,%.2f,%.1f,%.1f,%.1f\n", evts[i].t_s, evts[i].sample_idx, evts[i].sample_idx / C.rate_hz,
                    kn[evts[i].kind], evts[i].rms_mg, evts[i].jerk_from, evts[i].jerk_to, evts[i].latency_us);
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/vibguard_ticks.csv", C.outdir);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "tick,t_ms,interval_us\n");
        for (long i = 0; i < acq_n; i++)
            fprintf(f, "%ld,%.4f,%.2f\n", i, (acq_t[i] - acq_t[0]) / 1e6, i ? (acq_t[i] - acq_t[i - 1]) / 1e3 : 0.0);
        fclose(f);
    }

    /* ---- timing summary ---- */
    snprintf(path, sizeof path, "%s/vibguard_summary.txt", C.outdir);
    FILE *s = fopen(path, "w");
    FILE *outs[2] = { stdout, s };
    double period_us = 1e6 / (C.rate_hz * C.speedup);
    double *v = malloc(sizeof(double) * (acq_n + ana_n + nevt + 8));
    long nl = 0, na = 0, nr = 0;
    for (long i = 0; i < nevt; i++) { if (evts[i].kind == 1) na++; if (evts[i].kind == 2) nl++; if (evts[i].kind == 3) nr++; }
    for (int o = 0; o < 2; o++) {
        FILE *out = outs[o]; if (!out) continue;
        fprintf(out, "\n=== vibguard summary (measured on this run) ===\n");
        fprintf(out, "input %s, %ld samples replayed, speedup x%.1f\n", C.csv, acq_n, C.speedup);
        long n = 0;
        for (long i = 1; i < acq_n; i++) v[n++] = fabs((acq_t[i] - acq_t[i - 1]) / 1e3 - period_us);
        long late = 0; for (long i = 1; i < acq_n; i++) if ((acq_t[i] - acq_t[i - 1]) / 1e3 > 1.5 * period_us) late++;
        fprintf(out, "load: %d hog thread(s) at %s %d; vibguard %s\n", C.hogs, C.hog_prio > 0 ? "FIFO" : "default", C.hog_prio,
                C.rt ? "SCHED_FIFO 50/45/40" : "DEFAULT priority");
        {
            const char *pn[] = { "OTHER", "FIFO", "RR" };
            fprintf(out, "effective thread policy/prio: acq %s %d, ana %s %d, ctl %s %d\n",
                    pn[g_eff_pol[0] % 3], g_eff_prio[0], pn[g_eff_pol[1] % 3], g_eff_prio[1], pn[g_eff_pol[2] % 3], g_eff_prio[2]);
            if (C.hogs) {
                fprintf(out, "hogs: %lu spin blocks total; setschedparam rc:", g_hog_spins);
                for (int i = 0; i < C.hogs && i < 64; i++) fprintf(out, " %d", g_hog_rc[i]);
                fprintf(out, "\n");
            }
            /* how far analysis fell behind acquisition: newest sample age when analysis ran */
            double lagmax = 0; for (long i = 0; i < ana_n; i++) { double lag = (ana[i].t_done_ns - ana[i].t_last_ns) / 1e3; if (lag > lagmax) lagmax = lag; }
            fprintf(out, "max analysis lag (newest sample -> analysis done): %.1f us\n", lagmax);
        }
        stats("acq |jitter| us", v, n, out);
        fprintf(out, "ticks later than 1.5x period: %ld of %ld (%.2f%%)\n", late, acq_n - 1, acq_n > 1 ? 100.0 * late / (acq_n - 1) : 0.0);
        n = 0; for (long i = 0; i < ana_n; i++) v[n++] = ana[i].ana_us;
        stats("analysis time us", v, n, out);
        n = 0; for (long i = 0; i < nevt; i++) if (evts[i].kind != 3) v[n++] = evts[i].latency_us;
        stats("alert latency us", v, n, out);
        fprintf(out, "events: %ld alerts (jerk lowered), %ld further reductions, %ld restore steps; final jerk %.1f%%\n",
                na, nl, nr, g_jerk_pct);
        double mx = 0; for (long i = 0; i < ana_n; i++) if (ana[i].rms_mg > mx) mx = ana[i].rms_mg;
        fprintf(out, "max window RMS %.1f mg\n", mx);
        if (C.sim) {
            double sum = 0; long cnt = 0;
            for (long i = 0; i < ana_n; i++) {           /* windows fully inside drawing phases */
                double te = ana[i].last_idx / C.rate_hz, tb = te - C.win / C.rate_hz;
                if (sim_drawing(tb) && sim_drawing(te)) { sum += ana[i].rms_mg; cnt++; }
            }
            fprintf(out, "sim: mean RMS while drawing %.1f mg (%ld windows); moves completed %ld, mean move time %.3f s\n",
                    cnt ? sum / cnt : 0.0, cnt, P.moves_done, P.moves_done ? P.move_time_sum / P.moves_done : 0.0);
        }
    }
    if (s) fclose(s);
    free(v);
    return 0;
}
