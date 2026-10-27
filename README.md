# bwtop

Live memory-bandwidth monitors in the style of `nvitop`. They show how much DRAM bandwidth your machine is using right now and which part of it is using it.

| Tool | Platform | Measures | Root |
|---|---|---|---|
| [`mbtop`](#mbtop-apple-silicon) | macOS, Apple Silicon | DRAM total, split into **CPU** and **GPU** | no |
| [`nvbwtop`](#nvbwtop-nvidia--linux) | Linux, NVIDIA GPUs | VRAM bandwidth per GPU, plus host DRAM per socket (Intel) | only for host DRAM |

---

## mbtop (Apple Silicon)

```
╭─ mbtop · Apple M1 Pro · 16 GB · 200 GB/s peak ──────────────────────── 15:18:13 ─╮
│ DRAM ███████████████████████████████████▋─────  173.6 GB/s   87%   avg 173.2  max 173.6 │
│ CPU  ██▎──────────────────────────────────────   10.7 GB/s    5%   avg  11.6  max  12.4 │
│ GPU  █████████████████████████████████▊───────  164.7 GB/s   82%   avg 163.7  max 164.7 │
├──────────────────────────────────────────────────────────────────────────────────┤
│ CPU  41%  ██ ▃▂▂▂▁▁  E  95%  P  23%    GPU  99%    MEM 12.9/16 GB  swap 6.6 GB    │
│ PWR  GPU  5.51 W                                                                  │
╰──────────────────────────────────────────────────────────────────────────────────╯
╭─ bandwidth history ■ CPU ■ GPU ■ other ──────────────────────────────────────────╮
│  200 ┤                                                      ▄▄▄▄▄▄▄▄██████████   │
│  100 ┤                                    ▂▂▂▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃██████████████████   │
│    0 ┤▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁████████████████████████████████████   │
╰───────────────────────────────────────────────────────────────────────── 136s ─╯
    PID    GPU%    CPU%      MEM  COMMAND
  87954    91.6     0.3     1.0G  metalbench
    625     3.3       -        -  WindowServer
```

### Build and run

```sh
cd macos
make            # builds mbtop and membw, no dependencies beyond Xcode CLT
./mbtop         # q quit · +/- interval · r reset avg/max
./mbtop -1      # print one frame and exit
./membw         # plain-text logger: time, DRAM, CPU, GPU (GB/s) per line
sudo make install   # optional, copies both to /usr/local/bin
```

| Flag | Meaning |
|---|---|
| `-i ms` | update interval (default 1000) |
| `--peak GB/s` | override the chip's theoretical peak, used to scale the bars |
| `-1` | print a single frame and exit |

### How it works

Apple's private `IOReport` framework exposes a `PMP` group of bandwidth histograms. Each histogram records how long the memory fabric spent in each bandwidth bin. `mbtop` subscribes to them and turns each interval's histogram into an average GB/s:

| Channel | Shown as | Verified with |
|---|---|---|
| `DCS BW / RD+WR` | DRAM total | both benchmarks below |
| `AFC BW / AMCC RD+WR` | CPU | multithreaded streaming read: 55 GB/s bench → 55 GB/s reported |
| `AFR BW / AMCC RD+WR` | GPU | Metal blit copy: 174.3 GB/s bench → 175.3 GB/s reported |

Per-process bandwidth is not exposed by the hardware, so the process table shows GPU time (from the IORegistry) and CPU usage instead.

### Caveats

- Values are binned: about **±4 GB/s for DRAM** and **±8 GB/s for CPU/GPU**. At idle you'll see a few GB/s even when the true value is lower.
- `IOReport` is undocumented and can change with any macOS release.
- The CPU/GPU channel mapping was validated on an **M1 Pro**. Other chips likely behave the same, but they haven't been tested. The built-in peak table covers M1 through M4, plus base M5; use `--peak` for anything else.
- Recent macOS versions report zero for the CPU and DRAM energy counters, so `mbtop` shows only the power figures that actually report data.

---

## nvbwtop (NVIDIA + Linux)

```
╭─ nvbwtop · gpu-box · driver 550.54.15 · CUDA 12.4 ─────────────────────── 15:31:02 ─╮
│ GPU0  VRAM ███████████████████████▌────────  48% busy  ≈ 326 GB/s of 768   max 373 │
│       SM   ███████████████████████████████▏  88%        8.9/48 GB VRAM used        │
│       RTX A6000 · PCIe ↓0.00 ↑0.00 GB/s · 262/300 W · 87°C · mem 7600 MHz          │
├────────────────────────────────────────────────────────────────────────────────────┤
│ GPU1  VRAM ────────────────────────────────   0% busy  ≈   0 GB/s of 768   max 0   │
│       SM   ────────────────────────────────   0%        0.7/48 GB VRAM used        │
│       RTX A6000 · PCIe ↓0.00 ↑0.00 GB/s · 22/300 W · 51°C · mem 405 MHz            │
├────────────────────────────────────────────────────────────────────────────────────┤
│ HOST  DRAM ███████─────────────────────────   23.3 GB/s  14% of 164   max 38       │
│       S0 rd 5.3 wr 3.7   S1 rd 8.7 wr 5.6                                          │
│       CPU   9% of 96 threads    RAM 55/220 GB                                      │
╰────────────────────────────────────────────────────────────────────────────────────╯
     ... bandwidth history strip per GPU and host ...
    PID USER       GPU   SM%  MEM%    VRAM   CPU%     RSS  COMMAND
 412093 alice        0    74    40    8.5G  123.0   13.7G  python train.py
```

### Run

It's a single Python 3.8+ file that uses only the standard library and needs no `pip install`. It talks to `libnvidia-ml.so` directly through `ctypes`.

```sh
cd nvidia
./nvbwtop          # GPU rows only
sudo ./nvbwtop     # adds host DRAM rows (memory-controller counters need root)
./nvbwtop -1       # one frame and exit
```

| Flag | Meaning |
|---|---|
| `-i s` | update interval in seconds (default 1) |
| `--host-peak GB/s` | override host DRAM peak (otherwise computed from `dmidecode`) |
| `-1` | print a single frame and exit |

### How it works

- **GPU:** NVML gives the fraction of time device memory was busy (`utilization.memory`), plus SM %, VRAM, PCIe, power, clocks, and per-process SM/memory utilization. Theoretical peak = max memory clock × 2 × bus width.
- **Host:** Intel uncore IMC `cas_count_read` / `cas_count_write` counters, read with `perf_event_open` on one CPU per socket (64 bytes per CAS). `perf` doesn't need to be installed.

### Accuracy

| | Source | Accuracy |
|---|---|---|
| Host DRAM GB/s | IMC hardware counters | measured; within ~10% of a streaming-read benchmark |
| GPU VRAM busy % | NVML | as reported by the driver |
| GPU VRAM GB/s | **estimated** from busy % | calibrated on RTX A6000: 100% busy ≈ 678 GB/s (88% of peak). At partial load it reads ~15% high, and it can overstate bandwidth for compute-bound kernels that only touch memory sparsely. |

NVML on consumer and workstation GPUs doesn't expose real DRAM byte counters. For exact numbers, use DCGM's `DCGM_FI_PROF_DRAM_ACTIVE` profiling field (needs DCGM installed and admin profiling access).

The host section currently supports **Intel** server CPUs that have `uncore_imc` PMUs. On AMD and other CPUs, the HOST row reports that the counters aren't available.

---

## Calibrating on your own machine

`bench/` contains the load generators used to validate both tools:

```sh
# CPU streaming read (macOS or Linux)
cc -O3 -pthread -o rdbench bench/rdbench.c && ./rdbench 8

# Apple GPU (Metal blit copy)
swiftc -O -o metalbench bench/metalbench.swift && ./metalbench

# NVIDIA GPU: device-copy kernel; args: gpu seconds duty(0..1)
nvcc -O2 -o cudabench bench/cudabench.cu && ./cudabench 0 5 0.5
```

Run one while the monitor is open and compare the benchmark's reported GB/s with the monitor's reading.

## License

MIT
