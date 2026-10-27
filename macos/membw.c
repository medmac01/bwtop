// membw: live Apple Silicon memory bandwidth from IOReport PMP histograms.
// Build: clang -O2 -o membw membw.c -framework CoreFoundation -lIOReport
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct IOReportSubscription *IOReportSubscriptionRef;
extern CFMutableDictionaryRef IOReportCopyChannelsInGroup(CFStringRef, CFStringRef, uint64_t, uint64_t, uint64_t);
extern IOReportSubscriptionRef IOReportCreateSubscription(void *, CFMutableDictionaryRef, CFMutableDictionaryRef *, uint64_t, CFTypeRef);
extern CFDictionaryRef IOReportCreateSamples(IOReportSubscriptionRef, CFMutableDictionaryRef, CFTypeRef);
extern CFDictionaryRef IOReportCreateSamplesDelta(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
extern void IOReportIterate(CFDictionaryRef, int (^)(CFDictionaryRef));
extern CFStringRef IOReportChannelGetSubGroup(CFDictionaryRef);
extern CFStringRef IOReportChannelGetChannelName(CFDictionaryRef);
extern int IOReportStateGetCount(CFDictionaryRef);
extern CFStringRef IOReportStateGetNameForIndex(CFDictionaryRef, int);
extern int64_t IOReportStateGetResidency(CFDictionaryRef, int);

// Estimate scale: 0 = bin lower edge, 0.5 = midpoint, 1 = upper edge.
static double edge = 0.5;

// Average GB/s from a histogram whose bins are labelled with their upper edge.
static double hist_avg(CFDictionaryRef c) {
    int n = IOReportStateGetCount(c);
    double sum = 0, w = 0, prev = 0;
    for (int i = 0; i < n; i++) {
        char s[64] = "";
        CFStringRef q = IOReportStateGetNameForIndex(c, i);
        if (q) CFStringGetCString(q, s, sizeof s, kCFStringEncodingUTF8);
        double hi = atof(s);
        double r = (double)IOReportStateGetResidency(c, i);
        sum += r * (prev + (hi - prev) * edge);
        w += r;
        prev = hi;
    }
    return w > 0 ? sum / w : 0;
}

// Calibrated on M1 Pro: AFC tracks CPU streaming reads, AFR tracks Metal blits 1:1.
static const char *rows[][3] = {
    {"DCS BW", "RD+WR", "DRAM"},
    {"AFC BW", "AMCC RD+WR", "CPU"},
    {"AFR BW", "AMCC RD+WR", "GPU"},
};
#define NROWS (sizeof rows / sizeof rows[0])

int main(int argc, char **argv) {
    int ms = 1000, count = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) edge = atof(argv[++i]);
        else { fprintf(stderr, "usage: membw [-i ms] [-n samples] [-e edge 0..1]\n"); return 1; }
    }
    CFMutableDictionaryRef ch = IOReportCopyChannelsInGroup(CFSTR("PMP"), NULL, 0, 0, 0);
    CFMutableDictionaryRef sub = NULL;
    IOReportSubscriptionRef s = IOReportCreateSubscription(NULL, ch, &sub, 0, NULL);
    if (!s) { fprintf(stderr, "IOReport subscription failed\n"); return 1; }

    printf("%8s", "time");
    for (size_t r = 0; r < NROWS; r++) printf("%9s", rows[r][2]);
    printf("   GB/s, rd+wr, +/- half a histogram bin\n");
    fflush(stdout);

    CFDictionaryRef prev = IOReportCreateSamples(s, sub, NULL);
    for (int k = 0; count == 0 || k < count; k++) {
        usleep(ms * 1000);
        CFDictionaryRef cur = IOReportCreateSamples(s, sub, NULL);
        CFDictionaryRef d = IOReportCreateSamplesDelta(prev, cur, NULL);
        double vbuf[NROWS] = {0}, *v = vbuf;
        IOReportIterate(d, ^int(CFDictionaryRef c) {
            char sg[64] = "", nm[64] = "";
            CFStringRef x = IOReportChannelGetSubGroup(c);
            if (x) CFStringGetCString(x, sg, sizeof sg, kCFStringEncodingUTF8);
            x = IOReportChannelGetChannelName(c);
            if (x) CFStringGetCString(x, nm, sizeof nm, kCFStringEncodingUTF8);
            for (size_t r = 0; r < NROWS; r++)
                if (!strcmp(sg, rows[r][0]) && !strcmp(nm, rows[r][1])) v[r] = hist_avg(c);
            return 0;
        });
        time_t now = time(NULL);
        char ts[16];
        strftime(ts, sizeof ts, "%H:%M:%S", localtime(&now));
        printf("%8s", ts);
        for (size_t r = 0; r < NROWS; r++) printf("%9.1f", v[r]);
        printf("   ");
        for (int b = 0; b < (int)(v[0] / 5); b++) printf("%s", b < (int)(v[1] / 5) ? "c" : b < (int)((v[1] + v[2]) / 5) ? "g" : "#");
        printf("\n");
        fflush(stdout);
        CFRelease(d);
        CFRelease(prev);
        prev = cur;
    }
}
