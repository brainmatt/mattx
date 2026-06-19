# MattX - News

---

## 📰 Latest News

### 🚀 MattX v1.7 Released: The "Native Injector" & Alma Linux Support!
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


---
