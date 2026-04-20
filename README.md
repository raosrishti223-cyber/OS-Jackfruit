# Multi-Container Runtime (Engine + Monitor)

## 1. Project Overview

This project implements a lightweight container runtime in C with:

* A **user-space supervisor (engine)** to manage containers
* A **kernel module (monitor)** to enforce memory limits

The system supports multiple containers running concurrently, logging, IPC-based control, and scheduling experiments.

---

## 2. Components

### 2.1 engine.c (User-Space Runtime)

Responsibilities:

* Start and manage multiple containers
* Maintain metadata (ID, PID, state, logs)
* Provide CLI commands
* Communicate with supervisor using IPC
* Capture logs via pipes

### 2.2 monitor.c (Kernel Module)

Responsibilities:

* Register container PIDs
* Monitor memory usage (RSS)
* Enforce:

  * Soft limit → warning
  * Hard limit → kill process
* Expose device `/dev/container_monitor`

---

## 3. Setup Instructions

### Install dependencies

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build project

```bash
make clean
make
```

---

## 4. Root Filesystem Setup

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz

tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

Create container copies:

```bash
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
cp -a rootfs-base rootfs-gamma
```

---

## 5. Running the System

### Load kernel module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### Start supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

---

## 6. CLI Commands

### Start container (background)

```bash
sudo ./engine start alpha ./rootfs-alpha "/bin/sh"
```

### Run container (foreground)

```bash
sudo ./engine run gamma ./rootfs-gamma "echo hello_from_container"
```

### List containers

```bash
sudo ./engine ps
```

### View logs

```bash
sudo ./engine logs alpha
```

### Stop container

```bash
sudo ./engine stop alpha
```

---

## 7. Tasks and Demonstrations

### Task 1: Multi-Container Runtime

Commands:

```bash
sudo ./engine start c1 ./rootfs-alpha ./cpu_hog --nice 0
sudo ./engine start c2 ./rootfs-alpha ./cpu_hog --nice 15
sudo ./engine start c3 ./rootfs-alpha ./cpu_hog
sudo ./engine start c4 ./rootfs-alpha ./io_pulse
```

Description:

* Multiple containers run concurrently
* Each container has isolated namespaces

---

### Task 2: CLI and IPC

Commands:

```bash
sudo ./engine start alpha ./rootfs-alpha "echo hello_from_container"
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
```

Description:

* CLI communicates with supervisor using IPC
* Supervisor executes commands and returns output

---

### Task 3: Logging System

Commands:

```bash
sudo ./engine logs alpha
```

Description:

* Container stdout/stderr captured via pipes
* Stored in log files using bounded buffer

---

### Task 4: Memory Monitoring

Commands:

```bash
chmod +x ./rootfs-alpha/memory_hog
sudo ./engine start mem1 ./rootfs-alpha ./memory_hog --soft-mib 40 --hard-mib 60
sudo dmesg | tail
```

Description:

* Kernel module tracks memory usage
* Soft limit generates warning
* Hard limit kills container

---

### Task 5: Scheduling Experiments

Commands:

```bash
top
```

Description:

* CPU-bound vs I/O-bound workloads
* Effect of different nice values observed

---

### Task 6: Cleanup

Commands:

```bash
sudo ./engine stop alpha
ps aux | grep defunct
sudo rmmod monitor
```

Description:

* Containers stopped cleanly
* No zombie processes
* Kernel module unloaded

---

## 8. Common Issues

### Container exits immediately

* Cause: Command finished (e.g., echo)
* Fix:

```bash
"echo hello && sleep 100"
```

### chroot failed

* Rootfs missing or incorrect path
* Ensure rootfs exists and is valid

---

## 9. Key Concepts

* **IPC**: CLI ↔ Supervisor communication
* **Namespaces**: Isolation of containers
* **chroot**: Filesystem isolation
* **Pipes**: Logging mechanism
* **Kernel module**: Memory enforcement

---

## 10. Conclusion

This project demonstrates how container runtimes work internally by combining process isolation, IPC, logging, kernel interaction, and scheduling behavior into a single system.
