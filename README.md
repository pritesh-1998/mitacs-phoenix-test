# 🔥 Phoenix — Linux Kernel Syscall Monitor

> **A beginner-friendly deep dive into every file, every concept, and every line of this project.**
> Written for someone who can code but has never touched the Linux kernel before.

---

## 📚 Table of Contents

1. [What is This Project?](#1-what-is-this-project)
2. [Background: How Linux Works (the 30-second version)](#2-background-how-linux-works)
3. [Project Architecture — The Big Picture](#3-project-architecture--the-big-picture)
4. [File-by-File Breakdown](#4-file-by-file-breakdown)
   - [phoenix.c — The Kernel Module](#41-phoenixc--the-kernel-module)
   - [phoenix_ctl.c — The Userspace Controller](#42-phoenix_ctlc--the-userspace-controller)
   - [fsm.json — The State Machine Config](#43-fsmjson--the-state-machine-config)
   - [test.c — The Target Test Program](#44-testc--the-target-test-program)
   - [Makefile — The Build System](#45-makefile--the-build-system)
   - [Generated Build Artefacts](#46-generated-build-artefacts)
5. [Key Concepts Explained Simply](#5-key-concepts-explained-simply)
   - [System Calls (syscalls)](#51-system-calls-syscalls)
   - [Kernel Modules (.ko files)](#52-kernel-modules-ko-files)
   - [kprobes — Hooking into the Kernel](#53-kprobes--hooking-into-the-kernel)
   - [IOCTL — Talking to the Kernel from Userspace](#54-ioctl--talking-to-the-kernel-from-userspace)
   - [Misc Devices (/dev/phoenix_ctl)](#55-misc-devices-devphoenix_ctl)
   - [Finite State Machine (FSM)](#56-finite-state-machine-fsm)
6. [Data Flow — What Happens Step by Step](#6-data-flow--what-happens-step-by-step)
7. [Operating Modes](#7-operating-modes)
8. [How to Build and Run](#8-how-to-build-and-run)
9. [Command-Line Reference](#9-command-line-reference)
10. [Common Kernel Development Gotchas](#10-common-kernel-development-gotchas)
11. [Glossary](#11-glossary)

---

## 1. What is This Project?

**Phoenix** is a Linux **kernel module** that acts like a surveillance camera for system calls.

Think of it this way:

> Every time any program on your computer reads or writes data, it has to ask the Linux kernel for permission through a **system call**. Phoenix secretly watches those requests, logs them, or pretends to block them — all without modifying the program being watched.

### What it can do:
| Feature | Description |
|---|---|
| 🔍 **Log mode** | Silently record every `read` or `write` system call that matches your filter |
| 🚫 **Block mode** | Detect and log matched syscalls as "BLOCKED" (soft block — logs it but doesn't actually stop it) |
| 🎯 **PID filter** | Only watch a specific process by its Process ID |
| 🔄 **FSM mode** | Follow a sequence of expected syscalls (e.g., always expect `write` then `read` then `write`...) |
| 📡 **Live event query** | Ask the kernel "what was the last syscall you saw?" from a normal program |

---

## 2. Background: How Linux Works

Before diving into code, here are the 4 concepts you absolutely need to understand:

### The Two Worlds: Kernel Space vs. User Space

```
┌───────────────────────────────────────────────────────────────┐
│                        USER SPACE                             │
│                                                               │
│   Your programs live here: firefox, bash, your test.c, etc.  │
│   They have LIMITED power. They can't touch hardware directly.│
│                                                               │
│   phoenix_ctl  (our controller)  ──────────────────────────► │
│                          asks kernel to do things via IOCTL   │
├───────────────────────────────────────────────────────────────┤
│                  SYSTEM CALL BOUNDARY                         │
│     (programs cross this line 1000s of times per second)      │
├───────────────────────────────────────────────────────────────┤
│                       KERNEL SPACE                            │
│                                                               │
│   The kernel lives here. It has FULL power over hardware.     │
│   phoenix.ko  (our module)  ◄── intercepts syscalls here      │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

- **User space** = where normal programs run. Limited power.
- **Kernel space** = where the OS runs. Unlimited power, but a mistake here **crashes the whole machine**.
- A **system call** is the bridge: a user program says "kernel, please write this data to disk" → kernel does it.

### Why is Kernel Development Hard?
- No safety net: a bug doesn't crash just your program — it can freeze the **entire machine**.
- No standard library: you can't use `printf()`, `malloc()`, or any C standard library you're used to.
- You use kernel-specific functions instead: `printk()` for logging, `kmalloc()` for memory.
- You have to be careful about things like locking, memory access, and timing.

---

## 3. Project Architecture — The Big Picture

```
┌─────────────────────────────────────────────────────────────┐
│                  YOUR LINUX SYSTEM                          │
│                                                             │
│  ┌──────────────┐    IOCTL calls    ┌──────────────────┐   │
│  │ phoenix_ctl  │ ◄──────────────► │  /dev/phoenix_ctl │   │
│  │ (userspace   │                   │  (device file)    │   │
│  │  controller) │                   └────────┬─────────┘   │
│  └──────┬───────┘                            │             │
│         │                                    ▼             │
│         │                        ┌───────────────────────┐ │
│  ┌──────▼───────┐                │   phoenix.ko          │ │
│  │  fsm.json    │                │   (kernel module)     │ │
│  │  (sequence   │                │                       │ │
│  │  of syscalls)│                │  • kprobe on ksys_read│ │
│  └──────────────┘                │  • kprobe on ksys_write│ │
│                                  │  • ioctl handler      │ │
│                                  └───────────┬───────────┘ │
│                                              │ intercepts   │
│  ┌───────────────────────────────────────────▼───────────┐ │
│  │              LINUX KERNEL (always running)            │ │
│  │                                                       │ │
│  │   ksys_write() ──► called when ANY program writes     │ │
│  │   ksys_read()  ──► called when ANY program reads      │ │
│  └───────────────────────────────────────────────────────┘ │
│                                                             │
│  ┌───────────────────┐                                      │
│  │  test (test.c)    │ ◄── the program we watch            │
│  │  prints "hello"   │     (calls write 10,000 times)      │
│  │  10,000 times     │                                      │
│  └───────────────────┘                                      │
└─────────────────────────────────────────────────────────────┘
```

In plain English:
1. `test` is a **dummy program** that does a lot of writes — it's the "victim" we observe.
2. `phoenix.ko` is loaded into the kernel. It secretly hooks into `ksys_write` and `ksys_read` using **kprobes**.
3. `phoenix_ctl` is our **remote control**. We run it from a terminal to tell the module what to watch, and what mode to be in.
4. `fsm.json` defines a **sequence** of syscalls the FSM controller watches for.

---

## 4. File-by-File Breakdown

### 4.1 `phoenix.c` — The Kernel Module

This is the **heart** of the project. It runs inside the Linux kernel.

**Full path:** `phoenix.c` → compiled into `phoenix.ko`

#### Includes — The Kernel's "Standard Library"

```c
#include <linux/module.h>    // Needed by ALL kernel modules
#include <linux/kernel.h>    // For printk() — kernel's version of printf
#include <linux/init.h>      // For __init and __exit macros
#include <linux/kprobes.h>   // For kprobes — the syscall interception mechanism
#include <linux/miscdevice.h>// For creating /dev/phoenix_ctl device file
#include <linux/fs.h>        // For file_operations struct
#include <linux/uaccess.h>   // For copy_from_user / copy_to_user (safe memory copy)
#include <linux/ioctl.h>     // For IOCTL command macros
#include <linux/sched.h>     // For 'current' pointer (the currently running process)
```

> **Why no `<stdio.h>`?** In kernel space, the C standard library does NOT exist. The kernel has its own equivalents. `printk()` replaces `printf()`, etc.

---

#### Module Metadata

```c
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pritesh Lathia");
MODULE_DESCRIPTION("Phoenix syscall monitor");
```

These are mandatory tags embedded in the `.ko` file. `GPL` is important — many kernel APIs are only available to GPL-licensed modules.

---

#### State Variables

```c
#define MODE_OFF   0
#define MODE_LOG   1
#define MODE_BLOCK 2

static int mode = MODE_OFF;              // Current operating mode
static int selected_syscall = SYSCALL_NONE; // Which syscall to watch
static int last_event = SYSCALL_NONE;    // Last syscall that matched
static int target_pid = -1;             // -1 means "watch all processes"
```

These are **global variables** inside the kernel. They persist as long as the module is loaded, and they're shared between all code paths. `static` means they're private to this file.

---

#### The IOCTL Commands

```c
#define PHX_MAGIC 'P'
#define IOCTL_SET_MODE    _IOW(PHX_MAGIC, 1, int)  // Write: set mode
#define IOCTL_SET_SYSCALL _IOW(PHX_MAGIC, 2, int)  // Write: set which syscall to watch
#define IOCTL_SET_PID     _IOW(PHX_MAGIC, 3, int)  // Write: set target PID
#define IOCTL_GET_EVENT   _IOR(PHX_MAGIC, 4, int)  // Read: get last event
```

Think of these as **channel numbers on a walkie-talkie**. Each number is a unique command that `phoenix_ctl` can send to the kernel module via the `/dev/phoenix_ctl` device.

- `_IOW` = "I'm writing data TO the kernel" (the userspace program sends data)
- `_IOR` = "I'm reading data FROM the kernel" (the kernel sends data back)
- `PHX_MAGIC 'P'` = a unique "namespace" so our commands don't conflict with other drivers

---

#### `syscall_matches_target()` — The Filter Function

```c
static int syscall_matches_target(int wanted_syscall)
{
    if (selected_syscall != wanted_syscall)
        return 0;                               // Wrong syscall type, ignore

    if (target_pid != -1 && current->pid != target_pid)
        return 0;                               // Wrong PID, ignore

    return 1;                                   // This one matches!
}
```

This function is called inside every syscall hook. `current` is a **kernel magic pointer** that always points to the currently executing process. `current->pid` gives you its process ID.

The function returns `1` (match) only if:
- The syscall type matches what we're watching (`write` or `read`)
- AND either we're watching all PIDs (`target_pid == -1`) OR the current PID matches

---

#### `write_handler()` and `read_handler()` — The Interception Functions

```c
static int write_handler(struct kprobe *p, struct pt_regs *regs)
{
    unsigned long fd;
    unsigned long count;
    last_event = SYSCALL_WRITE;       // Record that we saw a write (even in OFF mode)
    if (mode == MODE_OFF)
        return 0;                     // Do nothing in OFF mode

    if (!syscall_matches_target(SYSCALL_WRITE))
        return 0;                     // Doesn't match our filter

    fd    = regs->di;                 // First argument to write(): file descriptor
    count = regs->dx;                 // Third argument to write(): byte count

    if (mode == MODE_LOG) {
        printk(KERN_INFO "[phoenix] write syscall pid=%d comm=%s fd=%lu count=%lu\n",
            current->pid, current->comm, fd, count);
    } else if (mode == MODE_BLOCK) {
        printk(KERN_INFO "[phoenix] BLOCKED write pid=%d comm=%s fd=%lu count=%lu (soft block)\n",
            current->pid, current->comm, fd, count);
    }
    return 0;
}
```

**What is `struct pt_regs *regs`?**

When a system call is made, the CPU saves all its register values. `pt_regs` is a struct that holds all of them. For `x86-64` Linux:

| Register | Syscall Argument |
|---|---|
| `rdi` (`regs->di`) | 1st argument → `fd` (file descriptor) |
| `rsi` (`regs->si`) | 2nd argument → `buf` (buffer pointer) |
| `rdx` (`regs->dx`) | 3rd argument → `count` (number of bytes) |

So `regs->di` = the file descriptor being written to, `regs->dx` = how many bytes.

**What is `current->comm`?**

`comm` is the **command name** of the process (up to 15 characters). E.g., `bash`, `firefox`, `test`.

> **Important:** "BLOCK" here is a **soft block** — it logs the word "BLOCKED" but does NOT actually prevent the syscall from happening. A real block would require returning a non-zero value from the pre-handler or using a different mechanism. This is intentional for safety.

---

#### kprobes — The Hooks

```c
static struct kprobe kp_write = {
    .symbol_name = "ksys_write",   // The kernel function to intercept
    .pre_handler = write_handler,  // Our function to run BEFORE it executes
};

static struct kprobe kp_read = {
    .symbol_name = "ksys_read",
    .pre_handler = read_handler,
};
```

A `kprobe` is like a **breakpoint** in the kernel. You tell it:
- Which function to intercept (`ksys_write` / `ksys_read`)
- Which function of yours to call before (`.pre_handler`) or after (`.post_handler`) it runs

`ksys_write` is the actual internal kernel function that handles every `write()` system call from every process on the system.

---

#### `phoenix_ioctl()` — The Kernel Side of the Remote Control

```c
static long phoenix_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int val;

    switch (cmd) {
    case IOCTL_SET_MODE:
        if (copy_from_user(&val, (int __user *)arg, sizeof(int)))
            return -EFAULT;        // If memory copy fails, return error
        mode = val;
        printk(KERN_INFO "[phoenix] mode=%d\n", mode);
        break;

    case IOCTL_GET_EVENT:
        val = last_event;
        last_event = SYSCALL_NONE; // Reset after reading (like clearing a mailbox)
        if (copy_to_user((int __user *)arg, &val, sizeof(int)))
            return -EFAULT;
        break;
    // ... etc
    }
    return 0;
}
```

**Why `copy_from_user()` and not just `*arg`?**

This is critical. User space and kernel space have **separate memory address spaces**. You **cannot** directly dereference a pointer from user space in the kernel — it could be invalid, cause a fault, or be a security attack. `copy_from_user()` safely copies the data across the boundary.

---

#### The Device Registration

```c
static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = phoenix_ioctl,  // Our ioctl handler
};

static struct miscdevice phoenix_device = {
    .minor = MISC_DYNAMIC_MINOR,  // Let the kernel assign a device number
    .name  = "phoenix_ctl",       // Creates /dev/phoenix_ctl
    .fops  = &fops,
};
```

`file_operations` is the kernel's way of saying "when someone opens/reads/writes/ioctls this device file, call these functions." We only implement `ioctl` — we don't need `read` or `write` for the device itself.

---

#### `phoenix_init()` and `phoenix_exit()` — Module Lifecycle

```c
static int __init phoenix_init(void)
{
    // 1. Register the /dev/phoenix_ctl device
    ret = misc_register(&phoenix_device);

    // 2. Hook into ksys_write
    ret = register_kprobe(&kp_write);

    // 3. Hook into ksys_read
    ret = register_kprobe(&kp_read);

    printk(KERN_INFO "[phoenix] ready (/dev/phoenix_ctl)\n");
    return 0;
}

static void __exit phoenix_exit(void)
{
    // Cleanup in reverse order of init (important!)
    unregister_kprobe(&kp_read);
    unregister_kprobe(&kp_write);
    misc_deregister(&phoenix_device);
    printk(KERN_INFO "[phoenix] unloaded\n");
}

module_init(phoenix_init);  // Tell kernel: call this on insmod
module_exit(phoenix_exit);  // Tell kernel: call this on rmmod
```

- `__init` = this function is only needed during initialization, the kernel can free its memory after
- `__exit` = only needed during removal
- `module_init` / `module_exit` = macros that register your functions with the kernel's module system

**Notice the error handling in init:** If `register_kprobe(&kp_write)` fails, we have to `misc_deregister` first (undo step 1) before returning. This is the **reverse-cleanup pattern** — always undo in reverse order.

---

### 4.2 `phoenix_ctl.c` — The Userspace Controller

This is a **normal C program** (not kernel code). It runs in userspace and talks to the kernel module via the `/dev/phoenix_ctl` device file.

**Full path:** `phoenix_ctl.c` → compiled into `phoenix_ctl` binary

#### Opening the Device

```c
fd = open("/dev/phoenix_ctl", O_RDWR);
```

`/dev/phoenix_ctl` looks like a regular file, but it's actually a **device file** — a special file that the kernel created when we registered our `miscdevice`. Opening it gives us a file descriptor we use for all ioctl calls.

---

#### The IOCTL Helper Functions

```c
static int set_mode_ioctl(int fd, int mode) {
    ioctl(fd, IOCTL_SET_MODE, &mode);
}

static int set_syscall_ioctl(int fd, int syscall_choice) {
    ioctl(fd, IOCTL_SET_SYSCALL, &syscall_choice);
}

static int set_pid_ioctl(int fd, int pid_value) {
    ioctl(fd, IOCTL_SET_PID, &pid_value);
}

static int get_event_ioctl(int fd) {
    int event_value;
    ioctl(fd, IOCTL_GET_EVENT, &event_value);
    return event_value;
}
```

Each of these wraps a single `ioctl()` system call. The `ioctl()` function takes:
1. `fd` — the file descriptor for the device
2. The command number (e.g., `IOCTL_SET_MODE`)
3. A pointer to the data to send or receive

---

#### The FSM Runner — `run_fsm()`

```c
static int run_fsm(int fd, const char *file_name, int pid_value)
{
    int states[MAX_STATES];               // Array of expected syscalls in order
    int state_count = load_fsm_file(...); // Parse fsm.json
    int current_state = 0;               // We start at state 0

    set_mode_ioctl(fd, MODE_LOG);         // Enable logging mode
    set_syscall_ioctl(fd, states[0]);     // Watch for the first expected syscall

    while (1) {
        sleep(1);                         // Poll every second
        int event = get_event_ioctl(fd);  // Ask: what happened?

        if (event == states[current_state]) {   // We saw what we expected!
            current_state++;                    // Advance the state machine
            if (current_state >= state_count)
                current_state = 0;             // Wrap around (cyclic FSM)

            set_syscall_ioctl(fd, states[current_state]); // Watch for next
        }
    }
}
```

This is a **polling loop**. Every 1 second it asks the kernel "did you see the syscall I'm waiting for?" If yes, it advances to the next state in the FSM.

---

#### The JSON Parser — `load_fsm_file()`

```c
while (fscanf(fp, "%1023s", buffer) == 1) {
    if (strstr(buffer, "write") != NULL) {
        states[count++] = SYSCALL_WRITE;
    } else if (strstr(buffer, "read") != NULL) {
        states[count++] = SYSCALL_READ;
    }
}
```

This is a very simple "parser" — it reads the JSON file word by word and looks for the words `"write"` or `"read"`. It doesn't actually parse JSON structure — it just finds the keywords. Given `fsm.json` is:

```json
{ "states": ["write", "read"] }
```

It will find "write" and "read" (with their quotes, but `strstr` finds substrings, so `"write"` contains `write`) and build the states array `[SYSCALL_WRITE, SYSCALL_READ]`.

---

#### The `main()` Function — Argument Parsing

```c
int main(int argc, char *argv[])
{
    // Open /dev/phoenix_ctl
    // Parse command-line arguments (--log, --block, --syscall, --pid, --file, etc.)
    // Send appropriate IOCTL calls to the kernel
    // Close the device
}
```

The program supports these modes:
- **Direct mode:** Set mode + syscall + PID and exit immediately
- **Event query:** Ask for the last event and print it
- **FSM mode:** Load fsm.json and run the state machine loop

---

### 4.3 `fsm.json` — The State Machine Config

```json
{
  "states": ["write", "read"]
}
```

This tiny file defines a **sequence** of syscalls the FSM should follow:
1. First, watch for `write`
2. When seen, switch to watching `read`
3. When seen, wrap back to watching `write`
4. Repeat forever

You can add more entries (up to 16) and change the order. This is the **policy** for what syscall sequence is "expected" or "normal" behavior.

---

### 4.4 `test.c` — The Target Test Program

```c
#include <stdio.h>

int main() {
    for (int i = 0; i < 10000; i++) {
        printf("hello\n");   // printf → internally calls write()
    }
    return 0;
}
```

`printf("hello\n")` doesn't go directly to the screen. Under the hood:
1. `printf` buffers the text
2. Eventually calls the C library's `write()` function
3. Which triggers the `write` **system call**
4. Which calls `ksys_write()` in the kernel
5. Which hits our **kprobe** → our `write_handler()` runs

So this tiny program generates ~10,000 `write` syscalls — perfect for testing the monitor.

---

### 4.5 `Makefile` — The Build System

```makefile
obj-m += phoenix.o          # Tell the kernel build system: build phoenix.c as a module

all: kernel userspace       # Default target builds both

kernel:
    make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
    # -C = change to the kernel build directory
    # M=$(PWD) = but build the module from our directory
    # This is the standard way to build out-of-tree kernel modules

userspace:
    gcc -Wall -o phoenix_ctl phoenix_ctl.c
    # Normal GCC compilation for the userspace program

clean:
    make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
    rm -f phoenix_ctl
```

The kernel compilation line is special. It uses the **kernel's own build system** (Kbuild). You point it to `/lib/modules/<your-kernel-version>/build` which contains all the kernel headers and build infrastructure. You can't just `gcc phoenix.c` a kernel module — it needs special flags, headers, and linking.

---

### 4.6 Generated Build Artefacts

After running `make`, you'll see several generated files:

| File | What it is |
|---|---|
| `phoenix.ko` | The compiled kernel module — this is what you `insmod` |
| `phoenix.o` | Object file for `phoenix.c` before linking |
| `phoenix.mod.c` | Auto-generated by Kbuild — adds module version info |
| `phoenix.mod.o` | Compiled version of `phoenix.mod.c` |
| `Module.symvers` | Exported symbol table (empty here, we don't export symbols) |
| `modules.order` | Build order of modules (just one here) |
| `.phoenix.o.cmd` | Exact compiler command used (useful for debugging build issues) |
| `phoenix_ctl` | The compiled userspace binary |
| `test` | The compiled test binary |

---

## 5. Key Concepts Explained Simply

### 5.1 System Calls (syscalls)

A **system call** is how a user program asks the kernel to do something privileged. Examples:

| Syscall | What it does |
|---|---|
| `write(fd, buf, n)` | Write n bytes to file descriptor fd |
| `read(fd, buf, n)` | Read n bytes from file descriptor fd |
| `open(path, flags)` | Open a file, get a file descriptor |
| `fork()` | Create a new process |
| `exit()` | Terminate the process |

When you call `printf()` in C, the C library eventually calls the `write` syscall. The kernel receives it, executes it, and returns.

### 5.2 Kernel Modules (.ko files)

A **kernel module** (`.ko` = Kernel Object) is a piece of code that can be loaded into a running kernel **without rebooting**. Think of it as a plugin for the kernel.

```bash
sudo insmod phoenix.ko   # Load the module
lsmod | grep phoenix     # Check it's loaded
sudo rmmod phoenix       # Unload it
dmesg | tail -20         # See kernel log messages (from printk)
```

Key differences from normal programs:
- Runs with **full kernel privileges** (can crash the machine)
- No `main()` function — uses `module_init()` and `module_exit()`
- No standard library (no `printf`, `malloc`, etc.)
- Uses `printk()` for logging, which goes to the **kernel ring buffer**

### 5.3 kprobes — Hooking into the Kernel

**kprobes** is a kernel debugging/instrumentation mechanism. It lets you insert "probes" (breakpoints) at any kernel address or function.

```
Normal execution:           With kprobe:
                            
ksys_write() called         ksys_write() called
  ↓                           ↓
  actual write code           YOUR pre_handler() runs first
  ↓                           ↓
  returns                     actual write code
                              ↓
                              returns
```

It works by **patching the first byte** of the target function with a breakpoint instruction. When it triggers, the CPU raises an exception, kprobe's exception handler runs your function, then execution continues normally.

This is why it's powerful: you can intercept **any** kernel function without modifying the kernel source.

### 5.4 IOCTL — Talking to the Kernel from Userspace

**IOCTL** (Input/Output Control) is a mechanism for device-specific commands. It lets a userspace program send arbitrary commands to a kernel driver.

```
Userspace:                      Kernel:
                                
ioctl(fd, IOCTL_SET_MODE, &3)  
  │                             
  │ (crosses kernel boundary)  
  │                             
  ▼                             
                                phoenix_ioctl(..., IOCTL_SET_MODE, 3)
                                  mode = 3;
                                  return 0;
```

The IOCTL number is encoded using macros:
- `_IOW(magic, number, type)` = write to kernel (userspace → kernel)
- `_IOR(magic, number, type)` = read from kernel (kernel → userspace)
- `_IOWR(magic, number, type)` = both directions

The `magic` byte (`'P'` here) acts as a namespace to avoid collision with other drivers' IOCTL numbers.

### 5.5 Misc Devices (/dev/phoenix_ctl)

A **misc device** (miscellaneous character device) is the simplest way to create a `/dev/` entry in Linux. When you call `misc_register()`:

1. The kernel creates `/dev/phoenix_ctl` automatically
2. When any program does `open("/dev/phoenix_ctl")`, the kernel calls your `file_operations` functions
3. Think of it as creating a "special file" that's actually a portal into your kernel module

The `MISC_DYNAMIC_MINOR` means "pick any available minor device number" — you don't need to hardcode one.

### 5.6 Finite State Machine (FSM)

A **Finite State Machine** is a model where:
- There are a fixed number of **states**
- The machine is always in exactly **one state** at a time
- **Events** (syscalls seen) cause **transitions** between states

In Phoenix's FSM mode with `fsm.json = {"states": ["write", "read"]}`:

```
State 0: Watching for "write"
   │
   │ (write syscall observed)
   ▼
State 1: Watching for "read"
   │
   │ (read syscall observed)
   ▼
State 0 (wraps back): Watching for "write" again
   │
   └─► repeat forever...
```

This lets you define an expected **behavioral pattern** for a process and observe whether it follows that pattern.

---

## 6. Data Flow — What Happens Step by Step

### Scenario: Monitor all write syscalls of PID 1234 in LOG mode

```
Step 1: Load the module
  $ sudo insmod phoenix.ko
  → phoenix_init() runs
  → /dev/phoenix_ctl appears
  → kprobes hooked on ksys_write and ksys_read
  → module is in MODE_OFF, watching SYSCALL_NONE

Step 2: Run phoenix_ctl
  $ sudo ./phoenix_ctl --log --syscall write --pid 1234
  → phoenix_ctl opens /dev/phoenix_ctl
  → ioctl(fd, IOCTL_SET_MODE, MODE_LOG)
      → kernel: mode = 1
  → ioctl(fd, IOCTL_SET_SYSCALL, SYSCALL_WRITE)
      → kernel: selected_syscall = 1
  → ioctl(fd, IOCTL_SET_PID, 1234)
      → kernel: target_pid = 1234
  → phoenix_ctl closes and exits

Step 3: PID 1234 calls write()
  → ksys_write() is called in the kernel
  → kprobe fires → write_handler() is called
  → syscall_matches_target() checks:
      - selected_syscall == SYSCALL_WRITE? YES
      - current->pid == 1234? YES (or NO if it's a different process)
  → mode == MODE_LOG → printk("[phoenix] write syscall pid=1234 ...")

Step 4: See the log
  $ sudo dmesg | grep phoenix
  [phone] write syscall pid=1234 comm=myapp fd=1 count=6
```

---

## 7. Operating Modes

| Mode | Value | What happens when a matching syscall is seen |
|---|---|---|
| `MODE_OFF` | 0 | Nothing (but `last_event` is still recorded) |
| `MODE_LOG` | 1 | Logs to kernel ring buffer via `printk()` |
| `MODE_BLOCK` | 2 | Logs "BLOCKED" message (note: does NOT actually block the syscall — it's a soft/logging block) |

> **Why doesn't BLOCK actually block?** Truly blocking a syscall with kprobes requires the pre_handler to modify the return registers and skip the original function. This is complex and risky, as it can destabilize the system. This implementation logs the intent but lets the syscall proceed safely. A real implementation would use `kretprobes` or `seccomp` for actual enforcement.

---

## 8. How to Build and Run

### Prerequisites

```bash
# Install kernel headers for your current kernel
sudo apt-get install linux-headers-$(uname -r)

# Install build tools
sudo apt-get install build-essential
```

### Build

```bash
cd /home/vboxuser/Desktop/kernel-lab/kernel-lab

# Build everything (kernel module + userspace tools)
make

# You should now see:
#   phoenix.ko   — kernel module
#   phoenix_ctl  — userspace controller
#   test         — test program (compile separately if needed)
```

### Compile the test binary (if not already done)

```bash
gcc -o test test.c
```

### Load the Module

```bash
sudo insmod phoenix.ko

# Verify it loaded
lsmod | grep phoenix

# Verify the device file exists
ls -la /dev/phoenix_ctl

# See the kernel log
sudo dmesg | tail -5
```

### Use the Controller

```bash
# Enable LOG mode, watch write syscalls (all processes)
sudo ./phoenix_ctl --log --syscall write

# Enable LOG mode, watch read syscalls from a specific PID
sudo ./phoenix_ctl --log --syscall read --pid $(pgrep my_program)

# Disable monitoring
sudo ./phoenix_ctl --off

# Query the last observed syscall event
sudo ./phoenix_ctl --get-event

# Run FSM mode (watches write → read → write → ... sequence)
sudo ./phoenix_ctl --log --file fsm.json
```

### Run the Test Program

In one terminal:
```bash
# Start logging write syscalls for our test program
sudo ./phoenix_ctl --log --syscall write

# Run the test program
./test

# Check the kernel log
sudo dmesg | grep phoenix | tail -20
```

### Unload the Module

```bash
sudo rmmod phoenix

# Verify it's gone
lsmod | grep phoenix
sudo dmesg | tail -3
```

---

## 9. Command-Line Reference

### `phoenix_ctl` Options

| Flag | Argument | Description |
|---|---|---|
| `--log` | — | Set mode to LOG (intercept and log matching syscalls) |
| `--block` | — | Set mode to BLOCK (log as blocked, but doesn't actually block) |
| `--off` | — | Turn off monitoring |
| `--syscall` | `write` or `read` | Which syscall to watch |
| `--pid` | `<number>` | Filter to a specific process ID (-1 = all processes) |
| `--get-event` | — | Print the last observed syscall event and exit |
| `--file` | `<path>` | Load FSM from JSON file (must be used with `--log`) |

### Examples

```bash
# Watch all write calls from bash (find bash's PID first)
sudo ./phoenix_ctl --log --syscall write --pid $(pgrep bash | head -1)

# Watch all reads from any process
sudo ./phoenix_ctl --log --syscall read

# Run FSM mode
sudo ./phoenix_ctl --log --file fsm.json

# Poll for last event (could use this in a script)
sudo ./phoenix_ctl --get-event
```

---

## 10. Common Kernel Development Gotchas

### 1. `printk` vs `printf`
In the kernel, **never** use `printf`. Use `printk(KERN_INFO "message\n")`. The `KERN_INFO` prefix sets the log level. View output with `dmesg`.

### 2. No sleeping in interrupt context
Our kprobe handlers run in **interrupt context** (or with interrupts disabled). You CANNOT sleep, block, or do anything that might block here. Sleeping would deadlock the kernel.

### 3. Memory access with `copy_from_user` / `copy_to_user`
Never dereference user-space pointers directly in kernel code. Always use these functions. They:
- Check the pointer is valid
- Handle page faults safely
- Prevent security attacks (time-of-check to time-of-use)

### 4. `static` variables are persistent
Unlike a regular program where globals reset on each run, kernel module globals persist in memory for as long as the module is loaded. Multiple `ioctl` calls share the same `mode`, `selected_syscall`, etc.

### 5. The `current` pointer
`current` is a per-CPU pointer that always refers to the process currently executing on that CPU. In a kprobe handler, `current` is the process that just made the syscall — extremely useful.

### 6. Cleanup order matters
Always clean up in **reverse initialization order**:
```
init:  A → B → C
exit:  C → B → A
```
Failing to do this can cause use-after-free panics.

### 7. `dmesg` is your debugger
Since you can't use `printf` or GDB in kernel space, `dmesg` (which shows the kernel ring buffer) is your primary debugging tool. Use `printk` liberally.

---

## 11. Glossary

| Term | Meaning |
|---|---|
| **Kernel module** | A plugin loaded into a running kernel; file extension `.ko` |
| **kprobe** | A kernel mechanism to insert breakpoints at any function |
| **syscall** | A request from a user program to the kernel to do something |
| **IOCTL** | Input/Output Control — a way for userspace to send custom commands to kernel drivers |
| **Miscdevice** | The simplest type of Linux character device; creates a `/dev/` entry |
| **`/dev/phoenix_ctl`** | The "portal" file our module creates; writing IOCTL to this talks to our module |
| **`insmod`** | "Insert module" — loads a `.ko` file into the kernel |
| **`rmmod`** | "Remove module" — unloads a module from the kernel |
| **`lsmod`** | Lists currently loaded kernel modules |
| **`dmesg`** | Displays the kernel's log ring buffer (where `printk` output goes) |
| **`printk`** | The kernel's equivalent of `printf`; output goes to `dmesg` |
| **`pt_regs`** | A struct holding CPU register values at the time of a syscall |
| **`current`** | A kernel magic pointer to the currently running process |
| **PID** | Process ID — a unique number assigned to each running process |
| **`copy_from_user`** | Safely copies data from user space into kernel space |
| **`copy_to_user`** | Safely copies data from kernel space back to user space |
| **`ksys_write`** | The internal kernel function that handles the `write()` syscall |
| **`ksys_read`** | The internal kernel function that handles the `read()` syscall |
| **FSM** | Finite State Machine — a model with states and transitions |
| **`file_operations`** | A kernel struct that maps device file operations to your handler functions |
| **`KERN_INFO`** | A log level prefix for `printk`; other levels: `KERN_ERR`, `KERN_WARNING`, `KERN_DEBUG` |
| **`GPL`** | GNU General Public License — many kernel APIs require your module to declare this license |
| **`__init`** / `__exit`** | Compiler hints that these functions are only needed at startup/shutdown |
| **`MODULE_LICENSE`** | Declares the module's license to the kernel (required) |
| **`static`** | In kernel modules, limits the symbol's scope to this file (prevents naming conflicts) |
| **Ring buffer** | A fixed-size circular buffer; the kernel log is stored here and viewed via `dmesg` |

---

*This README was written for Pritesh Lathia's Phoenix Kernel Module project (Mitacs Phoenix Test).*
*Author: Pritesh Lathia | License: GPL*
