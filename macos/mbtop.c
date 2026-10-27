// mbtop: nvitop-style live memory bandwidth monitor for Apple Silicon (no sudo).
// Build: clang -O2 -o mbtop mbtop.c -framework CoreFoundation -framework IOKit -lIOReport
//
// Bandwidth comes from the IOReport "PMP" histograms (time spent in GB/s bins);
// on M1 Pro, AFC tracked CPU streaming reads and AFR tracked Metal blits 1:1.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

typedef struct IOReportSubscription *IOReportSubscriptionRef;
extern CFMutableDictionaryRef IOReportCopyChannelsInGroup(CFStringRef, CFStringRef, uint64_t, uint64_t, uint64_t);
extern void IOReportMergeChannels(CFMutableDictionaryRef, CFMutableDictionaryRef, CFTypeRef);
extern IOReportSubscriptionRef IOReportCreateSubscription(void *, CFMutableDictionaryRef, CFMutableDictionaryRef *, uint64_t, CFTypeRef);
extern CFDictionaryRef IOReportCreateSamples(IOReportSubscriptionRef, CFMutableDictionaryRef, CFTypeRef);
extern CFDictionaryRef IOReportCreateSamplesDelta(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
extern void IOReportIterate(CFDictionaryRef, int (^)(CFDictionaryRef));
extern CFStringRef IOReportChannelGetGroup(CFDictionaryRef);
extern CFStringRef IOReportChannelGetSubGroup(CFDictionaryRef);
extern CFStringRef IOReportChannelGetChannelName(CFDictionaryRef);
extern CFStringRef IOReportChannelGetUnitLabel(CFDictionaryRef);
extern int64_t IOReportSimpleGetIntegerValue(CFDictionaryRef, int);
extern int IOReportStateGetCount(CFDictionaryRef);
extern CFStringRef IOReportStateGetNameForIndex(CFDictionaryRef, int);
extern int64_t IOReportStateGetResidency(CFDictionaryRef, int);

#define HISTN 2048
#define MAXPID 100000
#define MAXCORES 64
#define MAXPROCS 4096

#define C_CPU "\033[38;5;81m"
#define C_GPU "\033[38;5;114m"
#define C_OTH "\033[38;5;244m"
#define C_DIM "\033[38;5;239m"
#define C_BOX "\033[38;5;245m"
#define C_RST "\033[0m"
#define C_BLD "\033[1m"

typedef struct { double dram, cpu, gpu; } bw_t;
typedef struct { int pid; double cpu, gpu; uint64_t mem; char name[40]; } proc_t;
typedef struct { double sum, max; long n; } stat_t;

// ---- state ----
static char chip[64];
static double peak, mem_total;
static int ncores, ne;
static bw_t cur, hist[HISTN];
static long nhist;
static stat_t st[3];
static int has_pw[3];  // CPU, GPU, DRAM energy counters seen reporting
static double pw_cpu, pw_gpu, pw_dram, gpu_util, mem_used, swap_used;
static double core_util[MAXCORES];
static proc_t procs[MAXPROCS];
static int nprocs;
static int interval_ms = 1000, have_sample;
static int W = 100, H = 40;
static volatile sig_atomic_t quit, resized;
static struct termios orig_tio;
static mach_timebase_info_data_t tb;

// ---- output buffer ----
static char out[1 << 18];
static size_t ol;
static void p(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + ol, sizeof out - ol, fmt, ap);
    va_end(ap);
    if (n > 0) ol += (size_t)n < sizeof out - ol ? (size_t)n : sizeof out - ol - 1;
}
static void at(int y, int x) { p("\033[%d;%dH", y, x); }
static void rep(const char *s, int n) { for (int i = 0; i < n; i++) p("%s", s); }

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void cfstr(CFStringRef s, char *buf, size_t n) {
    buf[0] = 0;
    if (s) CFStringGetCString(s, buf, n, kCFStringEncodingUTF8);
}

// ---- chip info ----
// Theoretical peak DRAM bandwidth (GB/s) by generation and variant; 0 = unknown.
static double chip_peak(const char *name) {
    static const double t[][4] = {
        {68.25, 200, 400, 800},  // M1
        {100, 200, 400, 800},    // M2
        {100, 150, 400, 800},    // M3
        {120, 273, 546, 0},      // M4
        {153, 0, 0, 0},          // M5
    };
    const char *m = strstr(name, " M");
    if (!m) return 0;
    int gen = atoi(m + 2);
    if (gen < 1 || gen > 5) return 0;
    int v = strstr(name, "Ultra") ? 3 : strstr(name, "Max") ? 2 : strstr(name, "Pro") ? 1 : 0;
    return t[gen - 1][v];
}

static void init_info(void) {
    size_t n = sizeof chip;
    if (sysctlbyname("machdep.cpu.brand_string", chip, &n, NULL, 0)) strcpy(chip, "Apple Silicon");
    uint64_t mem = 0;
    n = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &n, NULL, 0);
    mem_total = mem / 1073741824.0;
    n = sizeof ncores;
    sysctlbyname("hw.ncpu", &ncores, &n, NULL, 0);
    n = sizeof ne;
    if (sysctlbyname("hw.perflevel1.logicalcpu", &ne, &n, NULL, 0)) ne = 0;
    if (ncores > MAXCORES) ncores = MAXCORES;
    if (!peak) peak = chip_peak(chip);
    mach_timebase_info(&tb);
}

// ---- IOReport: bandwidth + energy ----
static IOReportSubscriptionRef sub;
static CFMutableDictionaryRef subch;
static CFDictionaryRef ior_prev;

// Average GB/s from a histogram whose bins are labelled with their upper edge.
static double hist_avg(CFDictionaryRef c) {
    int n = IOReportStateGetCount(c);
    double sum = 0, w = 0, prev = 0;
    for (int i = 0; i < n; i++) {
        char s[64];
        cfstr(IOReportStateGetNameForIndex(c, i), s, sizeof s);
        double hi = atof(s), r = (double)IOReportStateGetResidency(c, i);
        sum += r * (prev + hi) / 2;
        w += r;
        prev = hi;
    }
    return w > 0 ? sum / w : 0;
}

static double energy_j(CFDictionaryRef c) {
    char u[16];
    cfstr(IOReportChannelGetUnitLabel(c), u, sizeof u);
    double v = (double)IOReportSimpleGetIntegerValue(c, 0);
    return u[0] == 'm' ? v * 1e-3 : u[0] == 'u' ? v * 1e-6 : u[0] == 'n' ? v * 1e-9 : v;
}

static int ior_init(void) {
    CFMutableDictionaryRef ch = IOReportCopyChannelsInGroup(CFSTR("PMP"), NULL, 0, 0, 0);
    CFMutableDictionaryRef en = IOReportCopyChannelsInGroup(CFSTR("Energy Model"), NULL, 0, 0, 0);
    if (!ch) return -1;
    if (en) IOReportMergeChannels(ch, en, NULL);
    sub = IOReportCreateSubscription(NULL, ch, &subch, 0, NULL);
    if (!sub) return -1;
    ior_prev = IOReportCreateSamples(sub, subch, NULL);
    return 0;
}

static void ior_sample(double dt) {
    CFDictionaryRef s = IOReportCreateSamples(sub, subch, NULL);
    CFDictionaryRef d = IOReportCreateSamplesDelta(ior_prev, s, NULL);
    CFRelease(ior_prev);
    ior_prev = s;
    __block bw_t b = {0};
    __block double ec = 0, eg = 0, ed = 0;
    IOReportIterate(d, ^int(CFDictionaryRef c) {
        char g[32], sg[32], nm[48];
        cfstr(IOReportChannelGetGroup(c), g, sizeof g);
        cfstr(IOReportChannelGetSubGroup(c), sg, sizeof sg);
        cfstr(IOReportChannelGetChannelName(c), nm, sizeof nm);
        if (!strcmp(g, "PMP")) {
            if (!strcmp(sg, "DCS BW") && !strcmp(nm, "RD+WR")) b.dram = hist_avg(c);
            else if (!strcmp(sg, "AFC BW") && !strcmp(nm, "AMCC RD+WR")) b.cpu = hist_avg(c);
            else if (!strcmp(sg, "AFR BW") && !strcmp(nm, "AMCC RD+WR")) b.gpu = hist_avg(c);
        } else if (!strcmp(g, "Energy Model")) {
            if (!strcmp(nm, "CPU Energy")) ec = energy_j(c);
            else if (!strcmp(nm, "GPU Energy")) eg = energy_j(c);
            else if (!strncmp(nm, "DRAM", 4)) ed += energy_j(c);
        }
        return 0;
    });
    CFRelease(d);
    cur = b;
    pw_cpu = ec / dt;
    pw_gpu = eg / dt;
    pw_dram = ed / dt;
    has_pw[0] |= ec > 0;
    has_pw[1] |= eg > 0;
    has_pw[2] |= ed > 0;
    hist[nhist++ % HISTN] = b;
    double v[3] = {b.dram, b.cpu, b.gpu};
    for (int i = 0; i < 3; i++) {
        st[i].sum += v[i];
        st[i].n++;
        if (v[i] > st[i].max) st[i].max = v[i];
    }
}

// ---- GPU utilization and per-process GPU time (IORegistry) ----
static uint64_t gbuf[2][MAXPID], cbuf[2][MAXPID];
static uint64_t *gpu_now = gbuf[0], *gpu_prev = gbuf[1], *cpu_now = cbuf[0], *cpu_prev = cbuf[1];

static int64_t cfnum(CFTypeRef n) {
    int64_t v = 0;
    if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue(n, kCFNumberSInt64Type, &v);
    return v;
}

static void gpu_sample(void) {
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it)) return;
    io_object_t acc;
    while ((acc = IOIteratorNext(it))) {
        CFTypeRef ps = IORegistryEntryCreateCFProperty(acc, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
        if (ps && CFGetTypeID(ps) == CFDictionaryGetTypeID())
            gpu_util = (double)cfnum(CFDictionaryGetValue(ps, CFSTR("Device Utilization %")));
        if (ps) CFRelease(ps);
        io_iterator_t ci;
        if (!IORegistryEntryGetChildIterator(acc, kIOServicePlane, &ci)) {
            io_object_t c;
            while ((c = IOIteratorNext(ci))) {
                CFTypeRef cr = IORegistryEntryCreateCFProperty(c, CFSTR("IOUserClientCreator"), kCFAllocatorDefault, 0);
                CFTypeRef au = IORegistryEntryCreateCFProperty(c, CFSTR("AppUsage"), kCFAllocatorDefault, 0);
                int pid = 0;
                char s[128];
                if (cr && CFGetTypeID(cr) == CFStringGetTypeID()) {
                    cfstr(cr, s, sizeof s);
                    sscanf(s, "pid %d", &pid);
                }
                if (pid > 0 && pid < MAXPID && au && CFGetTypeID(au) == CFArrayGetTypeID()) {
                    for (CFIndex i = 0; i < CFArrayGetCount(au); i++) {
                        CFTypeRef e = CFArrayGetValueAtIndex(au, i);
                        if (CFGetTypeID(e) == CFDictionaryGetTypeID())
                            gpu_now[pid] += cfnum(CFDictionaryGetValue(e, CFSTR("accumulatedGPUTime")));
                    }
                }
                if (cr) CFRelease(cr);
                if (au) CFRelease(au);
                IOObjectRelease(c);
            }
            IOObjectRelease(ci);
        }
        IOObjectRelease(acc);
    }
    IOObjectRelease(it);
}

// ---- CPU cores ----
static uint64_t core_prev[MAXCORES][2];

static void cores_sample(void) {
    natural_t n;
    processor_info_array_t info;
    mach_msg_type_number_t cnt;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &n, &info, &cnt)) return;
    processor_cpu_load_info_t l = (processor_cpu_load_info_t)info;
    for (int i = 0; i < (int)n && i < MAXCORES; i++) {
        uint64_t busy = l[i].cpu_ticks[CPU_STATE_USER] + l[i].cpu_ticks[CPU_STATE_SYSTEM] + l[i].cpu_ticks[CPU_STATE_NICE];
        uint64_t tot = busy + l[i].cpu_ticks[CPU_STATE_IDLE];
        uint64_t db = busy - core_prev[i][0], dt = tot - core_prev[i][1];
        core_util[i] = dt ? 100.0 * db / dt : 0;
        core_prev[i][0] = busy;
        core_prev[i][1] = tot;
    }
    vm_deallocate(mach_task_self(), (vm_address_t)info, cnt * sizeof(integer_t));
}

// ---- memory ----
static void mem_sample(void) {
    vm_statistics64_data_t vs;
    mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;
    if (!host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &c))
        mem_used = (double)(vs.internal_page_count - vs.purgeable_count + vs.wire_count + vs.compressor_page_count) *
                   vm_page_size / 1073741824.0;
    struct xsw_usage sw;
    size_t n = sizeof sw;
    if (!sysctlbyname("vm.swapusage", &sw, &n, NULL, 0)) swap_used = sw.xsu_used / 1073741824.0;
}

// ---- processes ----
static int proc_cmp(const void *a, const void *b) {
    const proc_t *x = a, *y = b;
    double kx = x->gpu * 1000 + (x->cpu > 0 ? x->cpu : 0), ky = y->gpu * 1000 + (y->cpu > 0 ? y->cpu : 0);
    return kx < ky ? 1 : kx > ky ? -1 : 0;
}

static void procs_sample(double dt) {
    static int pids[MAXPROCS];
    int n = proc_listallpids(pids, sizeof pids);
    nprocs = 0;
    for (int i = 0; i < n; i++) {
        int pid = pids[i];
        if (pid <= 0 || pid >= MAXPID) continue;
        struct rusage_info_v2 ri;
        int ok = !proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t *)&ri);
        if (ok) cpu_now[pid] = (ri.ri_user_time + ri.ri_system_time) * tb.numer / tb.denom;
        double cpu = ok && cpu_prev[pid] && cpu_now[pid] >= cpu_prev[pid] ? (cpu_now[pid] - cpu_prev[pid]) / (dt * 1e7) : -1;
        double gpu = gpu_prev[pid] && gpu_now[pid] >= gpu_prev[pid] ? (gpu_now[pid] - gpu_prev[pid]) / (dt * 1e7) : 0;
        if (gpu < 0.05 && cpu < 0.05) continue;
        proc_t *pr = &procs[nprocs++];
        pr->pid = pid;
        pr->cpu = cpu;
        pr->gpu = gpu > 100 ? 100 : gpu;
        pr->mem = ok ? ri.ri_phys_footprint : 0;
        if (proc_name(pid, pr->name, sizeof pr->name) <= 0) {
            struct kinfo_proc kp;
            size_t kl = sizeof kp;
            int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
            if (!sysctl(mib, 4, &kp, &kl, NULL, 0) && kl) snprintf(pr->name, sizeof pr->name, "%s", kp.kp_proc.p_comm);
            else snprintf(pr->name, sizeof pr->name, "[%d]", pid);
        }
        if (nprocs == MAXPROCS) break;
    }
    qsort(procs, nprocs, sizeof *procs, proc_cmp);
}

static void sample(double dt) {
    uint64_t *t;
    t = gpu_prev; gpu_prev = gpu_now; gpu_now = t;
    t = cpu_prev; cpu_prev = cpu_now; cpu_now = t;
    memset(gpu_now, 0, sizeof gbuf[0]);
    memset(cpu_now, 0, sizeof cbuf[0]);
    ior_sample(dt);
    gpu_sample();
    cores_sample();
    mem_sample();
    procs_sample(dt);
    have_sample = 1;
}

// ---- drawing ----
static const char *hb[] = {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"};
static const char *vb[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

static const char *pct_col(double v) { return v < 50 ? "\033[32m" : v < 80 ? "\033[33m" : "\033[31m"; }

// Horizontal stacked bar of `w` cells; full width represents `scale`.
static void bar(int w, const double *seg, const char **col, int nseg, double scale) {
    double cum[4], tot = 0;
    for (int i = 0; i < nseg; i++) cum[i] = tot += seg[i];
    int t8 = scale > 0 ? (int)(tot / scale * w * 8 + 0.5) : 0;
    if (t8 > w * 8) t8 = w * 8;
    for (int i = 0; i < w; i++) {
        int f = t8 - 8 * i;
        if (f > 8) f = 8;
        if (f <= 0) { p(C_DIM "─"); continue; }
        double pos = (8 * i + f / 2.0) / (8.0 * w) * scale;
        int k = 0;
        while (k < nseg - 1 && pos > cum[k]) k++;
        p("%s%s", col[k], hb[f]);
    }
    p(C_RST);
}

// Display columns of a UTF-8 string, skipping ANSI escape sequences.
static int vlen(const char *s) {
    int n = 0;
    for (; *s; s++) {
        if (*s == '\033') { while (*s && !(*s >= '@' && *s <= '~' && *s != '[')) s++; if (!*s) break; continue; }
        n += (*s & 0xC0) != 0x80;
    }
    return n;
}

static void box_top(int y, const char *title, const char *right) {
    at(y, 1);
    p(C_BOX "╭─ " C_RST C_BLD "%s" C_RST C_BOX " ", title);
    int tl = vlen(title);
    int rl = right ? (int)strlen(right) + 2 : 0;
    rep("─", W - 6 - tl - rl);
    if (right) p(" %s ", right);
    p("─╮" C_RST);
}
static void box_mid(int y) { at(y, 1); p(C_BOX "├"); rep("─", W - 2); p("┤" C_RST); }
static void box_bot(int y, const char *right) {
    at(y, 1);
    p(C_BOX "╰");
    int rl = right ? (int)strlen(right) + 2 : 0;
    rep("─", W - 3 - rl);
    if (right) p(" %s ", right);
    p("─╯" C_RST);
}
static void box_row(int y) { at(y, 1); p("\033[2K" C_BOX "│" C_RST "\033[%dG" C_BOX "│" C_RST "\033[%dG", W, 3); }

static void fmt_mem(char *b, size_t n, uint64_t v) {
    if (!v) snprintf(b, n, "-");
    else if (v >= 1ull << 30) snprintf(b, n, "%.1fG", v / 1073741824.0);
    else snprintf(b, n, "%.0fM", v / 1048576.0);
}

static void bw_row(int y, const char *label, const char *lcol, const double *seg, const char **col, int nseg, int si) {
    double v = 0;
    for (int i = 0; i < nseg; i++) v += seg[i];
    double scale = peak > 0 ? peak : (st[0].max > 50 ? st[0].max : 50);
    int bw = W - 6 - 9 - 44;
    if (bw < 10) bw = 10;
    box_row(y);
    p("%s%-5s" C_RST, lcol, label);
    bar(bw, seg, col, nseg, scale);
    p(" " C_BLD "%6.1f" C_RST " GB/s", v);
    if (peak > 0) p(" %s%4.0f%%" C_RST, pct_col(100 * v / peak), 100 * v / peak);
    else p("      ");
    p(C_OTH "   avg" C_RST " %6.1f" C_OTH "   max" C_RST " %6.1f", st[si].n ? st[si].sum / st[si].n : 0, st[si].max);
}

static void render(void) {
    ol = 0;
    p("\033[H\033[2J");
    if (W < 70 || H < 24) {
        p("mbtop: terminal too small (%dx%d, need 70x24). q to quit.", W, H);
        write(1, out, ol);
        return;
    }
    int y = 1;
    char title[160], right[64];
    time_t tn = time(NULL);
    strftime(right, sizeof right, "%H:%M:%S", localtime(&tn));
    if (peak > 0) snprintf(title, sizeof title, "mbtop · %s · %.0f GB · %.0f GB/s peak", chip, mem_total, peak);
    else snprintf(title, sizeof title, "mbtop · %s · %.0f GB · peak unknown (--peak)", chip, mem_total);
    box_top(y++, title, right);

    // Bandwidth bars
    double other = cur.dram - cur.cpu - cur.gpu;
    if (other < 0) other = 0;
    double dseg[3] = {cur.cpu, cur.gpu, other};
    double dsum = cur.cpu + cur.gpu + other, dscale = dsum > 0 ? cur.dram / dsum : 0;
    for (int i = 0; i < 3; i++) dseg[i] *= dscale;  // stacked shares that add up to the DRAM total
    const char *dcol[3] = {C_CPU, C_GPU, C_OTH};
    bw_row(y++, "DRAM", C_BLD, dseg, dcol, 3, 0);
    const char *cc[1] = {C_CPU}, *gc[1] = {C_GPU};
    bw_row(y++, "CPU", C_CPU, &cur.cpu, cc, 1, 1);
    bw_row(y++, "GPU", C_GPU, &cur.gpu, gc, 1, 2);
    box_mid(y++);

    // Utilization line
    double eu = 0, pu = 0, all = 0;
    for (int i = 0; i < ncores; i++) {
        all += core_util[i];
        if (i < ne) eu += core_util[i];
        else pu += core_util[i];
    }
    box_row(y++);
    p(C_CPU "CPU" C_RST " %s%3.0f%%" C_RST "  ", pct_col(all / ncores), all / ncores);
    for (int i = 0; i < ncores; i++) {
        if (i == ne && ne) p(" ");
        int k = (int)(core_util[i] / 100 * 8 + 0.5);
        p("%s%s", pct_col(core_util[i]), k ? vb[k > 8 ? 8 : k] : C_DIM "▁");
    }
    p(C_RST);
    if (ne) p(C_OTH "  E" C_RST " %3.0f%%" C_OTH "  P" C_RST " %3.0f%%", eu / ne, pu / (ncores - ne));
    p("    " C_GPU "GPU" C_RST " %s%3.0f%%" C_RST, pct_col(gpu_util), gpu_util);
    p("    " C_BLD "MEM" C_RST " %.1f/%.0f GB" C_OTH "  swap" C_RST " %.1f GB", mem_used, mem_total, swap_used);

    // Power line
    box_row(y++);
    p(C_BLD "PWR" C_RST);
    if (has_pw[0]) p("  " C_CPU "CPU" C_RST " %5.2f W ", pw_cpu);
    if (has_pw[1]) p("  " C_GPU "GPU" C_RST " %5.2f W ", pw_gpu);
    if (has_pw[2]) p("  " C_BLD "DRAM" C_RST " %5.2f W", pw_dram);
    if (!has_pw[0] || !has_pw[2]) p(C_OTH "   (%s%s%snot reported by this macOS)" C_RST, has_pw[0] ? "" : "CPU ",
                                    has_pw[0] || has_pw[2] ? "" : "and ", has_pw[2] ? "" : "DRAM ");
    box_bot(y++, NULL);

    // History graph
    int rest = H - y - 2;
    int gh = rest / 2 - 2;
    if (gh > 14) gh = 14;
    if (gh < 4) gh = 4;
    int gw = W - 9;
    if (gw > HISTN) gw = HISTN;
    double ymax = 20;
    for (int x = 0; x < gw; x++) {
        long idx = nhist - gw + x;
        if (idx < 0 || idx < nhist - HISTN) continue;
        bw_t s = hist[idx % HISTN];
        double t = s.cpu + s.gpu > s.dram ? s.cpu + s.gpu : s.dram;
        if (t * 1.1 > ymax) ymax = t * 1.1;
    }
    ymax = ((int)(ymax / 10) + 1) * 10;
    snprintf(title, sizeof title, "bandwidth history " C_RST C_CPU "■ CPU " C_GPU "■ GPU " C_OTH "■ other");
    box_top(y++, title, NULL);
    for (int r = 0; r < gh; r++) {
        box_row(y);
        if (r == 0) p(C_OTH "%4.0f ┤" C_RST, ymax);
        else if (r == gh / 2) p(C_OTH "%4.0f ┤" C_RST, ymax / 2);
        else if (r == gh - 1) p(C_OTH "   0 ┤" C_RST);
        else p(C_DIM "     │" C_RST);
        for (int x = 0; x < gw; x++) {
            long idx = nhist - gw + x;
            if (idx < 0 || idx < nhist - HISTN) { p(" "); continue; }
            bw_t s = hist[idx % HISTN];
            double t = s.cpu + s.gpu > s.dram ? s.cpu + s.gpu : s.dram;
            double k = 8.0 * gh / ymax;
            int base = 8 * (gh - 1 - r), f = (int)(t * k + 0.5) - base;
            if (f <= 0) { p(r == gh - 1 ? C_DIM "▁" : " "); continue; }
            if (f > 8) f = 8;
            double pos = (base + f / 2.0) / k;
            p("%s%s", pos < s.cpu ? C_CPU : pos < s.cpu + s.gpu ? C_GPU : C_OTH, vb[f]);
        }
        p(C_RST);
        y++;
    }
    snprintf(right, sizeof right, "%.0fs", gw * interval_ms / 1000.0);
    box_bot(y++, right);

    // Process table
    at(y++, 1);
    p("\033[7m%7s  %6s  %6s  %7s  %-*s" C_RST, "PID", "GPU%", "CPU%", "MEM", W - 34, "COMMAND");
    for (int i = 0; i < nprocs && y < H; i++, y++) {
        proc_t *pr = &procs[i];
        char m[16], c[16];
        fmt_mem(m, sizeof m, pr->mem);
        if (pr->cpu < 0) snprintf(c, sizeof c, "-");
        else snprintf(c, sizeof c, "%.1f", pr->cpu);
        at(y, 1);
        p("%7d  %s%6.1f" C_RST "  %6s  %7s  %.*s", pr->pid, pr->gpu > 0.05 ? C_GPU : C_DIM, pr->gpu, c, m, W - 34, pr->name);
    }
    if (!have_sample) { at(y, 1); p(C_OTH "  sampling..." C_RST); }

    at(H, 1);
    p("\033[7m q \033[0m quit  \033[7m +/- \033[0m interval %.2gs  \033[7m r \033[0m reset avg/max", interval_ms / 1000.0);
    if (W >= 100) p(C_OTH "   bandwidth binned: ±4 GB/s DRAM, ±8 CPU/GPU" C_RST);
    write(1, out, ol);
}

// ---- terminal ----
static void term_size(void) {
    struct winsize ws;
    if (!ioctl(1, TIOCGWINSZ, &ws) && ws.ws_col) { W = ws.ws_col; H = ws.ws_row; }
}
static void term_restore(void) {
    tcsetattr(0, TCSANOW, &orig_tio);
    printf("\033[?25h\033[?1049l");
    fflush(stdout);
}
static void on_sig(int s) { if (s == SIGWINCH) resized = 1; else quit = 1; }

int main(int argc, char **argv) {
    int once = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) interval_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--peak") && i + 1 < argc) peak = atof(argv[++i]);
        else if (!strcmp(argv[i], "-1")) once = 1;
        else {
            fprintf(stderr, "usage: mbtop [-i ms] [--peak GB/s] [-1]\n"
                            "  -i ms       update interval (default 1000)\n"
                            "  --peak N    override the chip's peak DRAM bandwidth\n"
                            "  -1          print one frame and exit\n");
            return 1;
        }
    }
    if (interval_ms < 100) interval_ms = 100;
    init_info();
    if (ior_init()) { fprintf(stderr, "mbtop: IOReport PMP channels unavailable on this machine\n"); return 1; }

    double t_last = now_s();
    if (once) {
        term_size();
        for (int i = 0; i < 2; i++) {
            usleep(interval_ms * 1000);
            double t = now_s();
            sample(t - t_last);
            t_last = t;
        }
        render();
        p("\n");
        return 0;
    }
    if (!isatty(0) || !isatty(1)) { fprintf(stderr, "mbtop: needs a terminal (use -1 for a snapshot)\n"); return 1; }

    tcgetattr(0, &orig_tio);
    struct termios raw = orig_tio;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);
    atexit(term_restore);
    struct sigaction sa = {0};
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGWINCH, &sa, NULL);
    printf("\033[?1049h\033[?25l");
    fflush(stdout);

    term_size();
    render();
    while (!quit) {
        double remain = interval_ms / 1000.0 - (now_s() - t_last);
        if (remain <= 0) {
            double t = now_s();
            sample(t - t_last);
            t_last = t;
            render();
            continue;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(0, &fds);
        struct timeval tv = {(time_t)remain, (suseconds_t)((remain - (int)remain) * 1e6)};
        int r = select(1, &fds, NULL, NULL, &tv);
        if (resized) { resized = 0; term_size(); render(); }
        if (r <= 0) continue;
        char c;
        while (read(0, &c, 1) == 1) {
            if (c == 'q' || c == 'Q') quit = 1;
            else if (c == '+' || c == '=') interval_ms = interval_ms >= 5000 ? 5000 : interval_ms * 2;
            else if (c == '-' || c == '_') interval_ms = interval_ms <= 125 ? 125 : interval_ms / 2;
            else if (c == 'r' || c == 'R') memset(st, 0, sizeof st);
        }
        render();
    }
    return 0;
}
