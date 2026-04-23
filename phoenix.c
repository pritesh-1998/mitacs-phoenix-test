/*
 * phoenix.c
 *
 * Syscall monitor/blocker using kprobes.
 * Supports three modes: OFF, LOG, BLOCK.
 * Controlled from userspace via ioctl.
 *
 * Author: Pritesh Lathia
 * Technical Test - Security Research Lab Mitacs 2026
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/sched.h>
#include <linux/string.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pritesh Lathia");
MODULE_DESCRIPTION("Phoenix syscall monitor - Technical Test 2026");

/* =====================================================
   Mode definitions
   OFF   → module disabled, does nothing
   LOG   → logs syscall activity to dmesg
   BLOCK → blocks target syscall for target pid
   ===================================================== */

#define MODE_OFF   0
#define MODE_LOG   1
#define MODE_BLOCK 2

/* =====================================================
   ioctl definitions
   These must match exactly in controller.c
   ===================================================== */

#define PHX_MAGIC         'P'
#define IOCTL_SET_MODE    _IOW(PHX_MAGIC, 1, int)
#define IOCTL_SET_SYSCALL _IOW(PHX_MAGIC, 2, char[16])
#define IOCTL_SET_PID     _IOW(PHX_MAGIC, 3, int)
#define IOCTL_GET_EVENT   _IOR(PHX_MAGIC, 4, int)

/* =====================================================
   Global state
   Configured at runtime via ioctl from userspace
   ===================================================== */

static int  mode         = MODE_OFF;  /* current operating mode      */
static char syscall_name[16] = "open"; /* syscall to watch/block      */
static int  watched_pid  = -1;        /* -1 means all processes       */
static int  event_seen   = 0;         /* flag for FSM: syscall fired  */

/* =====================================================
   Helper: should this process be acted on?
   In block mode we only block the watched pid.
   watched_pid == -1 means block everyone.
   ===================================================== */

static int pid_matches(void)
{
    if (watched_pid == -1)
        return 1;
    return (current->pid == watched_pid);
}

/* =====================================================
   kprobe pre-handlers
   Called just BEFORE the real syscall executes.
   Returning 1 tells kprobe to skip the original
   function (used for blocking).
   ===================================================== */

/* ---------- OPEN ---------- */
static int handle_open(struct kprobe *p, struct pt_regs *regs)
{
    /* do nothing when module is off */
    if (mode == MODE_OFF)
        return 0;

    /* only act when open is the configured syscall */
    if (strcmp(syscall_name, "open") != 0)
        return 0;

    if (mode == MODE_LOG) {
        printk(KERN_INFO
               "[phoenix] LOG  open  | pid=%-6d comm=%s\n",
               current->pid, current->comm);
        event_seen = 1;   /* notify FSM that syscall was observed */
        return 0;
    }

    if (mode == MODE_BLOCK && pid_matches()) {
        printk(KERN_INFO
               "[phoenix] BLOCK open | pid=%-6d comm=%s\n",
               current->pid, current->comm);
        regs->ax = -EPERM;  /* override return value with error  */
        return 1;           /* skip original syscall             */
    }

    return 0;
}

/* ---------- READ ---------- */
static int handle_read(struct kprobe *p, struct pt_regs *regs)
{
    if (mode == MODE_OFF)
        return 0;

    if (strcmp(syscall_name, "read") != 0)
        return 0;

    if (mode == MODE_LOG) {
        printk(KERN_INFO
               "[phoenix] LOG  read  | pid=%-6d comm=%s\n",
               current->pid, current->comm);
        event_seen = 1;
        return 0;
    }

    if (mode == MODE_BLOCK && pid_matches()) {
        printk(KERN_INFO
               "[phoenix] BLOCK read | pid=%-6d comm=%s\n",
               current->pid, current->comm);
        regs->ax = -EPERM;
        return 1;
    }

    return 0;
}

/* ---------- WRITE ---------- */
static int handle_write(struct kprobe *p, struct pt_regs *regs)
{
    if (mode == MODE_OFF)
        return 0;

    if (strcmp(syscall_name, "write") != 0)
        return 0;

    if (mode == MODE_LOG) {
        printk(KERN_INFO
               "[phoenix] LOG  write | pid=%-6d comm=%s\n",
               current->pid, current->comm);
        event_seen = 1;
        return 0;
    }

    if (mode == MODE_BLOCK && pid_matches()) {
        printk(KERN_INFO
               "[phoenix] BLOCK write| pid=%-6d comm=%s\n",
               current->pid, current->comm);
        regs->ax = -EPERM;
        return 1;
    }

    return 0;
}

/* =====================================================
   kprobe structs
   Symbol names verified on Ubuntu 24.04 kernel 6.x
   Check with: grep <symbol> /proc/kallsyms
   ===================================================== */

static struct kprobe kp_open = {
    .symbol_name = "do_sys_openat2",
    .pre_handler = handle_open,
};

static struct kprobe kp_read = {
    .symbol_name = "ksys_read",
    .pre_handler = handle_read,
};

static struct kprobe kp_write = {
    .symbol_name = "ksys_write",
    .pre_handler = handle_write,
};

/* =====================================================
   ioctl handler
   Receives commands from the userspace controller
   ===================================================== */

static long phoenix_ioctl(struct file *f,
                          unsigned int cmd,
                          unsigned long arg)
{
    int  value;
    char buf[16];

    switch (cmd) {

    /* change operating mode (OFF / LOG / BLOCK) */
    case IOCTL_SET_MODE:
        if (copy_from_user(&value, (int __user *)arg, sizeof(int)))
            return -EFAULT;
        mode = value;
        printk(KERN_INFO "[phoenix] mode changed to %d\n", mode);
        break;

    /* set which syscall to watch or block */
    case IOCTL_SET_SYSCALL:
        if (copy_from_user(buf, (char __user *)arg, sizeof(buf)))
            return -EFAULT;
        /* safe copy - always null terminate */
        strncpy(syscall_name, buf, sizeof(syscall_name) - 1);
        syscall_name[sizeof(syscall_name) - 1] = '\0';
        printk(KERN_INFO "[phoenix] target syscall = %s\n", syscall_name);
        break;

    /* set which pid to block (-1 = all) */
    case IOCTL_SET_PID:
        if (copy_from_user(&value, (int __user *)arg, sizeof(int)))
            return -EFAULT;
        watched_pid = value;
        printk(KERN_INFO "[phoenix] target pid = %d\n", watched_pid);
        break;

    /*
     * userspace polls this to check if the watched
     * syscall has fired (used by FSM controller).
     * Flag is reset to 0 after each read.
     */
    case IOCTL_GET_EVENT:
        if (copy_to_user((int __user *)arg,
                         &event_seen, sizeof(int)))
            return -EFAULT;
        event_seen = 0;  /* reset after delivery */
        break;

    default:
        return -EINVAL;
    }

    return 0;
}

/* =====================================================
   Character device: /dev/phoenix_ctl
   This is how userspace opens and talks to the module
   ===================================================== */

static const struct file_operations phoenix_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = phoenix_ioctl,
};

static struct miscdevice phoenix_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "phoenix_ctl",
    .fops  = &phoenix_fops,
};

/* =====================================================
   Module init
   Registers device first, then each kprobe.
   On any failure, already-registered resources
   are cleaned up before returning.
   ===================================================== */

static int __init phoenix_init(void)
{
    int ret;

    printk(KERN_INFO "[phoenix] loading...\n");

    /* step 1: register character device */
    ret = misc_register(&phoenix_device);
    if (ret) {
        printk(KERN_ERR "[phoenix] misc_register failed: %d\n", ret);
        return ret;
    }

    /* step 2: register open kprobe */
    ret = register_kprobe(&kp_open);
    if (ret) {
        printk(KERN_ERR "[phoenix] kprobe open failed: %d\n", ret);
        misc_deregister(&phoenix_device);
        return ret;
    }

    /* step 3: register read kprobe */
    ret = register_kprobe(&kp_read);
    if (ret) {
        printk(KERN_ERR "[phoenix] kprobe read failed: %d\n", ret);
        unregister_kprobe(&kp_open);
        misc_deregister(&phoenix_device);
        return ret;
    }

    /* step 4: register write kprobe */
    ret = register_kprobe(&kp_write);
    if (ret) {
        printk(KERN_ERR "[phoenix] kprobe write failed: %d\n", ret);
        unregister_kprobe(&kp_read);
        unregister_kprobe(&kp_open);
        misc_deregister(&phoenix_device);
        return ret;
    }

    printk(KERN_INFO
           "[phoenix] ready | device=/dev/phoenix_ctl\n");
    return 0;
}

/* =====================================================
   Module exit
   Unregister in reverse order of registration
   ===================================================== */

static void __exit phoenix_exit(void)
{
    unregister_kprobe(&kp_write);
    unregister_kprobe(&kp_read);
    unregister_kprobe(&kp_open);
    misc_deregister(&phoenix_device);
    printk(KERN_INFO "[phoenix] unloaded\n");
}

module_init(phoenix_init);
module_exit(phoenix_exit);