# MattX - The Modern Single System Image (SSI) Cluster

<div align="center">
  <img src="https://raw.githubusercontent.com/brainmatt/mattx/refs/heads/main/tools/3dmattx/3DMattX.gif" width="800" alt="3DMattX Cluster Visualization">
  <p><em>3DMattX: Gamified, real-time 3D cluster visualization and load monitoring.</em></p>
</div>

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![Kernel: 7.x](https://img.shields.io/badge/Linux_Kernel-7.0+-green.svg)]()

**MattX** is a modern, out-of-tree Linux kernel module that brings the magic of **Single System Image (SSI)** clustering to the Linux 7.x era. 

Inspired by the legendary openMosix project, MattX allows multiple physical or virtual machines to act as one massive, unified computer. It autonomously load-balances CPU-bound tasks by seamlessly teleporting running processes across the network—preserving their memory, CPU registers, file descriptors, and network sockets—without requiring any modifications to the Linux kernel source code.

---

## 📰 Latest News

### 🚀 MattX v1.7 Released: The "Native Injector" & Alma Linux Support!

<div style="display: inline-flex; align-items: center;">
  <!-- Video Thumbnail -->
  <a href="https://www.youtube.com/watch?v=am8meoc2H1s" target="_blank" style="display: inline-block;">
    <img src="./web/mattx-1.7-youtube.png" style="width: 100%; display: block;">
  </a>

</div>


---

*June 2026*
MattX v1.7 is our most stable and architecturally advanced release yet! We have completely re-architected the system to cleanly separate the **Control Plane** (process migration, lifecycle) from the **Data Plane** (File and Network I/O). 
* **Kworker + Native Injector:** We have moved away from the legacy "Deputy Hijack" (waking the sleeping home process via signals) for most syscalls. Instead, MattX now spawns background kernel threads (`kworkers`) that execute syscalls natively and forcefully inject the resulting `struct file *` directly into the sleeping Deputy's FD table. All core File and Network I/O syscalls have been refactored to this blazing-fast architecture!
* **Alma Linux & SELinux Victory:** MattX is now fully supported on RPM/RHEL-based distributions like Alma Linux. Even better? Because our Native Injector perfectly mimics standard local process behavior, **MattX runs flawlessly with SELinux set to ENFORCING!**
* **Version Interface:** You can now check your running cluster version via the new read-only `/proc/mattx/version` interface.

### 🛠️ The New MattX-TestSuite by Kris Buytaert
*June 2026*
A massive shoutout to openMosix legend Kris Buytaert for contributing the official **MattX-TestSuite**! This powerful Makefile/Bash toolkit uses `libvirt` and official cloud images to fully automate the deployment of a MattX cluster. With a single `make debcluster` command, you can spin up a fully configured, multi-node MattX development environment in under 5 minutes. It currently supports Debian, Ubuntu, and Alma Linux!
Please find it at: 
https://github.com/KrisBuytaert/mattx-testsuite

👉 **[Read older updates in our NEWS archive](./NEWS.md)**

---

## ✨ Key Features (MattX v1.7)

### 🧠 Live Process Teleportation & The Syscall Drainer
MattX migrates running processes safely using a custom **Syscall Drainer**. Instead of violently freezing processes with `SIGSTOP` while they hold kernel locks, MattX uses modern `task_work_add(TWA_SIGNAL)` to gently guide the process to the user-space boundary. Once in a 100% stable state, its memory map (VMAs), CPU registers (`pt_regs`), and credentials (`struct cred`) are extracted and streamed over a custom TCP data pump to the target node.

### 🗂️ MattXFS & The Namespace Illusion
To ensure migrated processes can still access their original files, MattX introduces **MattXFS**, a custom Virtual File System (VFS). 
When a process arrives on a remote node, MattX spawns a surrogate (`mattx-stub`) inside a private Linux Mount Namespace (`unshare(CLONE_NEWNS)`). The stub bind-mounts the remote MattXFS cluster tree (e.g., `/mattxfs/709/`) directly over its own root directory (`/`). 
* **The Result:** The Linux VFS natively routes all file operations (`open`, `read`, `write`, `seek`, `fsync`) across the network back to the Home Node. This completely eliminates the need for hacky Kprobes for file I/O!

### ⚡ Direct File System Access (DFSA)
For true High-Performance Computing (HPC), sending file I/O over a TCP wormhole is a bottleneck. MattX supports **DFSA**. If your cluster shares a SAN or NFS drive (e.g., `/mnt/shared`), you can configure MattX to bypass the network. The Namespace Illusion will bind-mount the local SAN directly into the surrogate's glass box, allowing migrated processes to read and write shared data at bare-metal disk speeds.

### 🌐 The Network I/O Wormhole
Migrated processes don't lose their network connections! MattX uses Kretprobes to intercept network system calls (`socket`, `bind`, `listen`, `connect`, `sendto`, `recvfrom`). It tunnels these requests back to the Home Node, allowing a process running on Node B to maintain an active TCP connection established on Node A. It even features a **Distributed Wait Queue** to seamlessly route asynchronous `poll()` and `select()` calls across the cluster!

### 🔄 Perfect Lifecycle Symmetry (The Funeral Director)
MattX maintains strict tracking of exported "Deputies" and remote "Surrogates." If a migrated process finishes its work and exits naturally, the remote node sends a Death Certificate back home to cleanly terminate the Deputy. If a Sysadmin types `kill -9 <pid>` on the Home Node, an Assassination Order is instantly dispatched to terminate the remote Surrogate. No ghost processes, no memory leaks.

---

## ⚙️ Configuration (`/etc/mattx.conf`)

MattX is designed to be "Zero-Config" via UDP Multicast auto-discovery, but it offers powerful toggles for cluster administrators:

```ini
# Network interface and UDP Multicast group for Zero-Config Auto-Discovery
INTERFACE=eth0
MULTICAST_GROUP=239.0.0.1
PORT=7225

# Feature Toggles (A/B Testing & Fallbacks)
MIGRATE_NETWORK_IO=true     # Set to false to disable socket routing
MIGRATE_FILE_IO=true        # Set to false to fallback to Kprobe File I/O
MATTXFS_ENABLED=true        # Set to true to enable the MattXFS Namespace Illusion

# Direct File System Access (DFSA)
DFSA_DIR=/mnt/shared        # Directory to bypass network routing (e.g., shared NFS/SAN)
```

---

## 📊 Administration & Monitoring (`/proc/mattx/`)

MattX embraces the UNIX philosophy. The entire cluster can be monitored and controlled using simple text files in the `/proc/mattx/` virtual directory.

### Read-Only Monitoring
* `cat /proc/mattx/nodes` - Displays a live table of all connected nodes, their IP addresses, CPU load, and Free Memory.
* `cat /proc/mattx/remote` - Lists all processes that have been migrated *away* from this node (`<orig_pid>:<target_node>`).
* `cat /proc/mattx/guests` - Lists all foreign processes currently running *on* this node (`<local_surrogate_pid>:<home_node>`).
* `cat /proc/mattx/version` - Displays the current MattX version and license information.

### Real-Time Cluster Control
You can control the cluster dynamically by echoing commands into `/proc/mattx/admin`:

```bash
# Disable the automatic load balancer (useful for manual testing)
echo "balancer 0" > /proc/mattx/admin

# Manually migrate a specific PID to a specific Node ID
echo "migrate 1234 814" > /proc/mattx/admin

# Recall a migrated process back to its Home Node!
echo "migrate 1234 home" > /proc/mattx/admin
```

---

## 🛠️ The Ecosystem Tools

* **`mattx.ko` & `mattxfs.ko`**: The core kernel modules.
* **`mattx-discd`**: The user-space UDP discovery daemon. It reads `/etc/mattx.conf` and configures the kernel via Generic Netlink.
* **`mattx-stub`**: The user-space surrogate binary. It carves memory, builds the Namespace Illusion, and sacrifices itself to receive the brain transplant of a migrated process.
* **`3DMattX`**: A gamified, WebGL-based 3D cluster visualization tool. Fly through your cluster in real-time and watch the CPU load bars rise and fall as processes migrate!

---

## Requirements and Kernel Support 

**MattX** is developed and tested on Debian Trixie for Kernel 6.x and Ubuntu 26.04 for Kernel 7.x. It is also fully supported on Alma Linux (Kernel 6.x) with SELinux enforcing!

---

## 📜 License

MattX is proudly open-source and released under the **GNU General Public License v2.0 (GPLv2)** to ensure maximum compatibility with the Linux kernel ecosystem. 

**Copyright (c) 2026 by Matthias Rechenburg.**
