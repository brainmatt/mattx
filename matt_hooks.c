#include "mattx.h"

// We use a Kretprobe (Return Probe) so we can eventually hijack the return value (the FD)
static struct kretprobe openat_kprobe;

// This function runs right BEFORE do_sys_openat2 executes
static int entry_handler_openat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;

    // 1. FAST PATH: If this is a normal process, do absolutely nothing!
    // This guarantees zero performance impact for native applications.
    if (!is_guest_process(my_pid)) {
        return 0; 
    }

    // 2. We have a Guest! 
    // In x86_64, the 2nd argument to do_sys_openat2 is the filename pointer (stored in the RSI register)
    // We save this pointer inside the Kretprobe instance so the return handler can read it.
    *((const char __user **)ri->data) = (const char __user *)regs->si;

    return 0;
}

// This function runs right AFTER do_sys_openat2 finishes, but BEFORE it returns to user-space
static int ret_handler_openat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    const char __user *filename_ptr;
    char filename[256] = {0};
    long ret_val;

    // Fast path check again (just to be safe)
    if (!is_guest_process(my_pid)) {
        return 0;
    }

    // Retrieve the filename pointer we saved in the entry handler
    filename_ptr = *((const char __user **)ri->data);

    // Safely copy the filename string from user-space memory into our kernel buffer
    if (strncpy_from_user(filename, filename_ptr, sizeof(filename) - 1) > 0) {
        
        // The return value of the syscall (the File Descriptor) is stored in the RAX register
        ret_val = regs_return_value(regs);

        // --- EXTREME DEBUGGING ---
        printk(KERN_INFO "MattX:[HOOK] Intercepted open() by Guest PID %d!\n", my_pid);
        printk(KERN_INFO "MattX:[HOOK] Filename: '%s'\n", filename);
        printk(KERN_INFO "MattX:[HOOK] Kernel assigned Local FD: %ld\n", ret_val);
        
        // TODO Phase 12.2: Pause the process here and ask Node 1 to open the file instead!
    }

    return 0;
}

int mattx_hooks_init(void) {
    int ret;

    // Configure the Kretprobe for do_sys_openat2
    memset(&openat_kprobe, 0, sizeof(openat_kprobe));
    openat_kprobe.kp.symbol_name = "do_sys_openat2";
    openat_kprobe.entry_handler = entry_handler_openat;
    openat_kprobe.handler = ret_handler_openat;
    openat_kprobe.data_size = sizeof(const char __user *); // Space to store the filename pointer
    openat_kprobe.maxactive = 64; // Max concurrent open() calls we can track

    ret = register_kretprobe(&openat_kprobe);
    if (ret < 0) {
        printk(KERN_ERR "MattX: register_kretprobe failed, returned %d\n", ret);
        return ret;
    }

    printk(KERN_INFO "MattX: Syscall Hooks (Kprobes) registered successfully.\n");
    return 0;
}

void mattx_hooks_exit(void) {
    unregister_kretprobe(&openat_kprobe);
    printk(KERN_INFO "MattX: Syscall Hooks (Kprobes) unregistered.\n");
}

