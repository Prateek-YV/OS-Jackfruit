# Makefile for Multi-Container Runtime
# Builds: engine (user-space), monitor.ko (kernel module), workloads

KDIR     := /lib/modules/$(shell uname -r)/build
CC       := gcc
CFLAGS   := -Wall -Wextra -O2 -pthread

# User-space targets
USERSPACE_BINS := engine cpu_hog memory_hog io_pulse

.PHONY: all clean ci module userspace

all: userspace module

# ---- User-space ----
userspace: $(USERSPACE_BINS)

engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o $@ $<

cpu_hog: cpu_hog.c
	$(CC) $(CFLAGS) -o $@ $<

memory_hog: memory_hog.c
	$(CC) $(CFLAGS) -o $@ $<

io_pulse: io_pulse.c
	$(CC) $(CFLAGS) -o $@ $<

# ---- Kernel module ----
module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Kbuild picks this up
obj-m += monitor.o

# ---- CI target (no kernel headers, no sudo required) ----
ci: engine cpu_hog memory_hog io_pulse
	@echo "CI build OK"
	@./engine 2>&1 | grep -q "Usage:" && echo "engine usage check OK" || (echo "engine usage check FAILED" && exit 1)

# ---- Clean ----
clean:
	rm -f $(USERSPACE_BINS)
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
	rm -f *.o *.ko *.mod *.mod.c *.symvers *.order
