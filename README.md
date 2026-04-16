# 🚀 MattX: The Modern Linux SSI Cluster

**MattX** is an experimental, out-of-tree Single System Image (SSI) clustering framework for modern Linux 6.x kernels. 

Born from the spiritual heritage of the legendary **openMosix** project, MattX brings transparent, live process migration into the modern era. It allows a cluster of standard Linux machines to act as a single, massive symmetric multiprocessor (SMP) system. Processes can be teleported across the network—complete with their memory, CPU registers, credentials, and open file descriptors—without the application ever knowing it moved.

## ✨ Key Features

*   **Transparent Live Migration:** Teleport running, CPU-bound user-space applications across physical nodes with zero modifications to the application code.
*   **Zero-Config Auto-Discovery:** Nodes dynamically find each other using UDP Multicast beacons. No static IP maps or complex orchestration required.
*   **Autonomous Load Balancing:** The MattX kernel module continuously exchanges fixed-point CPU load metrics. When a node becomes overloaded, it automatically hunts for the heaviest CPU-bound task and migrates it to the least-loaded peer.
*   **The VFS Wormhole (Syscall Routing):** Migrated processes maintain their connection to the Home Node. Standard I/O (like `stdout` and `stderr`) is intercepted via dynamically injected "Fake FDs" and tunneled back to the user's original terminal.
*   **Modern Kernel Native:** Built for Linux 6.x. It safely navigates modern kernel protections, utilizing the Maple Tree (`VMA_ITERATOR`) for memory mapping, `access_process_vm` for safe memory injection, and RCU locks for process hunting.
*   **Distributed Lifecycle Management:** Perfect symmetry. If a migrated process exits on a remote node, the home node cleans up. If a user `kill`s the origin process on the home node, an "Assassination Order" is sent to instantly terminate the remote surrogate.

## 🏗️ Architecture

MattX operates on a **Master/Deputy & Surrogate** model, split across three main components:

1.  **`mattx.ko` (The Kernel Engine):** The core LKM. It handles high-speed TCP kernel-to-kernel communication, state extraction, memory injection, and VFS proxying.
2.  **`mattx-discd` (The Matchmaker):** A lightweight user-space daemon that broadcasts UDP beacons and bridges discovery data to the kernel via Generic Netlink.
3.  **`mattx-stub` (The Vessel):** A tiny user-space binary spawned on the target node during migration. It requests the incoming memory blueprint, uses `mmap(MAP_FIXED)` to carve out the exact memory layout required, and then sacrifices itself to be overwritten by the incoming process.

### The Migration Lifecycle (The Golden Path)
When a process is migrated from Node A (Home) to Node B (Remote):
1.  **Freeze & Extract:** Node A sends `SIGSTOP` to the target process. It extracts the CPU registers (`pt_regs`), Thread Local Storage (`FS_BASE`), Credentials (UID/GID), and the Memory Blueprint (VMAs).
2.  **Spawn & Carve:** Node B receives the blueprint and spawns `mattx-stub`. The stub carves the exact memory holes needed and expands its File Descriptor table.
3.  **The Data Pump:** Node A safely streams the physical memory pages over TCP. Node B injects them directly into the stub's carved memory.
4.  **The Awakening:** Node B overwrites the stub's CPU registers, applies the original user credentials, renames the process, injects the VFS Proxy FDs, and sends `SIGCONT`. The process resumes execution exactly where it left off.

## 🛠️ Getting Started

### Prerequisites
*   Linux Kernel 6.x (Tested on 6.12+)
*   `build-essential`, `linux-headers`, `libnl-3-dev`, `libnl-genl-3-dev`

### Installation
code
Bash
# Compile the kernel module and user-space tools
make

# Install the surrogate stub (Crucial: The kernel looks for it here!)
code
Bash
sudo cp mattx-stub /usr/local/bin/

# Load the kernel module
code
Bash
sudo insmod mattx.ko

# Start the discovery daemon
code
Bash
sudo ./mattx-discd &

### 🎛️ Administration (/proc/mattx)
MattX embraces the UNIX philosophy. Cluster management is handled entirely through a simple, scriptable /proc virtual filesystem.

View Cluster State:
code
Bash
cat /proc/mattx/nodes

Displays a live table of connected Node IDs, IP Addresses, CPU Load, and Free Memory.
Disable Automatic Load Balancing:
code
Bash
echo "balancer 0" > /proc/mattx/admin

Manual Forward Migration:
code
Bash
# Teleport PID 1234 to Node 814
echo "migrate 1234 814" > /proc/mattx/admin

Manual Return Migration (Recall):
code
Bash
# Pull a previously migrated process back to its Home Node
echo "migrate 1234 home" > /proc/mattx/admin

### ⚠️ Disclaimer
MattX is an experimental prototype diving into the deep "dark arts" of the Linux kernel (manipulating pt_regs, memory maps, and VFS structures on the fly). While it has been engineered with extreme care to avoid kernel panics, it should not be run on production systems containing critical data.
Built with 🍚, 🥚, and a lot of ☕.

