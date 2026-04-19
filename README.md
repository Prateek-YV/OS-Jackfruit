# Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running supervisor and a kernel-space memory monitor.

---

## 1. Team Information

| Name | SRN |
|------|-----|
| \<YELLAPANTULA VENKATA PRATEEK\> | \<PES2UG24CS617\> |
| \<YELLAPANTULA VENKATA PRANAV\> | \<PES2UG24CS616\> |

---

## 2. Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 VM with Secure Boot OFF. No WSL.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) git
```

### Clone the Repository

```bash
git clone https://github.com/Prateek-YV/OS-Jackfruit.git
cd OS-Jackfruit/boilerplate
```

### Prepare the Root Filesystem

```bash
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Copy workload binaries into rootfs
cp cpu_hog memory_hog io_pulse rootfs-base/
```

### Build Everything

```bash
make
```

This builds:
- `engine` — user-space supervisor and CLI
- `monitor.ko` — kernel module
- `cpu_hog`, `io_pulse`, `memory_hog` — test workloads

### Load the Kernel Module

```bash
sudo insmod monitor.ko
```

### Verify the Control Device

```bash
ls -l /dev/container_monitor
```

### Check Kernel Logs

```bash
dmesg | tail -5
```

---

## 3. Running the Project

### Start the Supervisor (Terminal 1)

```bash
sudo ./engine supervisor ./rootfs-base
```

### Create Per-Container Rootfs Copies (Terminal 2)

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### Start Two Containers

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96
```

### List Tracked Containers

```bash
sudo ./engine ps
```

### Inspect Container Logs

```bash
sudo ./engine logs alpha
sudo ./engine logs beta
```

### Run a Container in Foreground

```bash
sudo ./engine run gamma ./rootfs-alpha /bin/sh
```

### Stop a Container

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
```

### Test Memory Limits

```bash
# Run memory_hog inside a container with low limits
sudo ./engine start memtest ./rootfs-alpha /memory_hog --soft-mib 20 --hard-mib 40

# Watch kernel logs for soft/hard limit events
dmesg | tail -10
```

### Run Scheduling Experiments

```bash
# CPU-bound at default priority
sudo ./engine start cpu1 ./rootfs-alpha /cpu_hog

# CPU-bound at lower priority (nice=10)
sudo ./engine start cpu2 ./rootfs-beta /cpu_hog --nice 10

# Compare completion times in logs
sudo ./engine logs cpu1
sudo ./engine logs cpu2
```

### Unload the Module and Clean Up

```bash
sudo ./engine stop alpha
sudo ./engine stop beta

sudo rmmod monitor

dmesg | tail -5

make clean
```

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Each container is created using `clone()` with `CLONE_NEWPID`, `CLONE_NEWUTS`, and `CLONE_NEWNS` flags. This gives each container its own PID namespace (so it sees itself as PID 1), its own hostname, and its own mount namespace. After `clone()`, the child calls `chroot()` to change its root directory to the provided Alpine rootfs, then mounts `/proc` inside. The host kernel is still shared between all containers — they share the same kernel code, scheduler, network stack, and device drivers. Only the namespaced resources are isolated.

### 4.2 Supervisor and Process Lifecycle

A long-running supervisor is useful because it can track container state across their entire lifecycle, reap exited children via `SIGCHLD`, and maintain a consistent metadata store. Without a persistent supervisor, there would be no process to wait on children, leading to zombie processes. The supervisor uses `waitpid(-1, &status, WNOHANG)` inside the SIGCHLD handler to reap all exited children immediately. Container state transitions go from `starting` → `running` → `exited` / `stopped` / `killed` depending on how the process ends.

### 4.3 IPC, Threads, and Synchronization

The project uses two IPC mechanisms. Pipes carry container output from the container to the supervisor's log reader threads. A UNIX domain socket carries CLI commands from the client to the supervisor. The bounded buffer is protected by a `pthread_mutex_t` with two `pthread_cond_t` variables — one for "not full" (used by producers) and one for "not empty" (used by the consumer). Without the mutex, concurrent pushes and pops could corrupt the head/tail indices. Without condition variables, threads would busy-wait and waste CPU. The container metadata list is protected by a separate mutex so signal handlers and the main thread do not race on container state updates.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the amount of physical RAM currently held by a process. It does not measure swap usage, shared library pages, or memory-mapped files that have not been faulted in yet. Soft and hard limits serve different purposes — the soft limit triggers a warning so the operator is aware a container is using more memory than expected, while the hard limit enforces a strict ceiling by killing the process. Enforcement belongs in kernel space because the kernel has direct, lock-protected access to each process's `mm_struct` and can act on limit violations atomically with the scheduler, preventing a process from allocating more memory between a user-space check and the kill signal.

### 4.5 Scheduling Behavior

The Linux Completely Fair Scheduler (CFS) tracks each process's virtual runtime and always runs the process with the lowest virtual runtime next. Nice values adjust the weight assigned to a process — a process with nice=10 gets less CPU weight than one with nice=0, so it accumulates virtual runtime faster and is scheduled less frequently. In our experiments, a `cpu_hog` at nice=10 completed slower than one at nice=0 when both ran concurrently. An `io_pulse` process remained responsive even when running alongside `cpu_hog` because it spent most of its time sleeping, accumulating very low virtual runtime, and was immediately scheduled when it woke up.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** Used `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` with `chroot()`.
**Tradeoff:** Does not use `pivot_root` or network namespaces, so containers share the host network stack.
**Justification:** Sufficient for the project requirements and simpler to implement correctly.

### Supervisor Architecture
**Choice:** Single-threaded event loop with `select()` for the control socket.
**Tradeoff:** Handles only one CLI request at a time; concurrent CLI requests would queue.
**Justification:** Container management operations are infrequent, so a single-threaded loop is safe and avoids complexity.

### IPC and Logging
**Choice:** Pipes for log capture, UNIX domain socket for CLI control.
**Tradeoff:** Two separate IPC mechanisms add complexity compared to a single shared channel.
**Justification:** Separating concerns makes the code cleaner and matches the project specification requirement for a second IPC mechanism.

### Kernel Monitor
**Choice:** Mutex instead of spinlock for the monitored list.
**Tradeoff:** Mutex can sleep, which is not allowed in interrupt context, but is safe in timer callback context which runs in softirq context with `mutex_trylock`.
**Justification:** The list operations (kmalloc, kfree, iteration) can block, so a mutex is more appropriate than a spinlock.

### Scheduling Experiments
**Choice:** Used `nice()` syscall to adjust priorities rather than cgroups or CPU affinity.
**Tradeoff:** Nice values affect scheduling weight but do not hard-limit CPU usage.
**Justification:** Nice values are the simplest way to observe CFS behavior differences without requiring cgroup setup.

---

## 6. Scheduler Experiment Results

### Experiment 1: Two CPU-bound containers at different priorities

| Container | Nice Value | Duration | Observations |
|-----------|-----------|----------|--------------|
| cpu1 | 0 (default) | ~10s | Completed faster, received more CPU share |
| cpu2 | 10 (lower priority) | ~15s | Completed slower, preempted more often by cpu1 |

**Analysis:** CFS assigned higher weight to cpu1 (nice=0), so it accumulated virtual runtime more slowly and was scheduled more frequently. cpu2 at nice=10 received approximately 30% less CPU time, consistent with the CFS weight calculation.

### Experiment 2: CPU-bound vs I/O-bound container

| Container | Type | Nice Value | Observations |
|-----------|------|-----------|--------------|
| cpu1 | CPU-bound (cpu_hog) | 0 | Fully utilized its CPU share |
| io1 | I/O-bound (io_pulse) | 0 | Remained highly responsive, finished all iterations on time |

**Analysis:** The io_pulse process spent most of its time sleeping between write iterations. Each time it woke up, CFS immediately scheduled it because its virtual runtime was very low relative to cpu_hog. This demonstrates CFS's natural fairness — sleeping processes are not penalized and receive prompt scheduling when they wake.

---

## GitHub Link

https://github.com/Prateek-YV/OS-Jackfruit
