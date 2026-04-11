---

# 🚀 MattX v0.1: The Modern SSI Foundation
**Project Goal:** To resurrect the magic of openMosix for the modern Linux 6.x era, creating a transparent, kernel-level distributed computing cluster.

## 🏗️ Core Architecture
The MattX system is divided into three distinct, highly synchronized components:

1.  **The Kernel Module (`mattx.ko`):** The core engine. It handles high-speed TCP kernel-to-kernel communication, cluster state management, load balancing heuristics, and the "dark arts" of process state extraction and memory injection.
2.  **The Discovery Daemon (`mattx-discd`):** The matchmaker. A lightweight user-space daemon that uses UDP Multicast to provide Zero-Config cluster discovery. It communicates with the kernel module via a high-speed Generic Netlink bridge.
3.  **The Surrogate (`mattx-stub`):** The empty vessel. A tiny user-space binary spawned on the target node. It requests the memory blueprint, uses `mmap(MAP_FIXED)` to carve out the exact memory holes required, and then sacrifices itself to be overwritten by the incoming process.

## ✨ Key Features of v0.1

*   **Zero-Config Auto-Discovery:** Nodes dynamically join and leave the cluster using UDP beacons. No static IP maps required.
*   **Autonomous Load Balancing:** Nodes exchange fixed-point CPU load metrics every 2 seconds. The Decision Engine automatically identifies when a node is overloaded and finds the least-loaded peer in the cluster.
*   **Smart Process Hunting:** The scheduler safely walks the RCU-protected process list, filtering out kernel threads, system daemons, and already-migrated guests, locking onto the heaviest user-space CPU hog.
*   **Live State Extraction:** The engine freezes a running process (`SIGSTOP`) and extracts its complete DNA:
    *   The Memory Blueprint (Virtual Memory Areas / VMAs).
    *   The Full Brain (All 21 general-purpose CPU registers, including `RIP` and `RSP`).
    *   The Soul (Thread Local Storage bases: `FS_BASE` and `GS_BASE`).
    *   The Identity (UID, GID, and the 16-byte Process Name).
*   **Safe Memory Teleportation:** Bypassing the kernel's strict `vm_insert_page` limitations, MattX uses a synchronized "Ready-ACK" data pump. It reads memory using `access_process_vm` and streams it in 4KB chunks over TCP.
*   **The Brain Transplant:** The target kernel safely injects the network data directly into the carved memory of the `mattx-stub`, overwrites its CPU registers, applies the original user credentials, renames the process, and sends `SIGCONT` to wake it up.
*   **Perfect Lifecycle Symmetry:** 
    *   *The Forward Funeral:* If the migrated process finishes its work and dies on the remote node, the Home Node is notified and cleans up the frozen Deputy.
    *   *The Backward Funeral (Assassination Order):* If the user kills the frozen Deputy on the Home Node, the Remote Node is notified and instantly executes the Surrogate.
    *   *Anti-Ping-Pong:* Migrated guests are registered and immunized against being load-balanced again, ensuring cluster stability.

## 🧬 The Migration Lifecycle (The Golden Path)
1.  **[DECISION]** Node 1 detects high load, selects `stress` (PID A), and targets Node 2.
2.  **[EXTRACT]** Node 1 freezes PID A, extracts the Blueprint, and sends it to Node 2.
3.  **[SPAWN]** Node 2 receives the Blueprint and spawns `mattx-stub` (PID B).
4.  **[CARVE]** PID B asks the kernel for the Blueprint, carves the memory using `mmap`, and sends `HIJACK_ME`.
5.  **[READY]** Node 2 tells Node 1: *"The vessel is prepared."*
6.  **[PUMP]** Node 1 streams the physical memory bytes to Node 2. Node 2 injects them into PID B.
7.  **[AWAKEN]** Node 2 overwrites PID B's registers, applies the UID/GID, renames it to `stress`, and wakes it up.
8.  **[FUNERAL]** When the user kills PID A on Node 1, Node 1 sends an Assassination Order, and Node 2 terminates PID B.

---

### 🔮 The Road Ahead: Phase 8 (The Illusion)
MattX v0.1 successfully teleports CPU-bound tasks (like math calculators). However, the process is currently a "nomad." If it tries to read a file or write to the screen, it will interact with Node 2's hardware, not Node 1's.

To achieve **MattX v0.2**, we must enter the realm of **Syscall Routing**. We will need to intercept system calls on the remote node, package them, and forward them back to the Deputy on the home node to execute on the Surrogate's behalf. 


