#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/sched.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pritesh Lathia");
MODULE_DESCRIPTION("Phoenix syscall monitor");

/* ---------- MODES ---------- */

#define MODE_OFF   0
#define MODE_LOG   1
#define MODE_BLOCK 2

static int mode = MODE_OFF;

/* ---------- SYSCALL CHOICES ---------- */

#define SYSCALL_NONE  0
#define SYSCALL_WRITE 1
#define SYSCALL_READ  2

static int selected_syscall = SYSCALL_NONE;

/* ---------- PID FILTER ---------- */

static int target_pid = -1;

/* ---------- IOCTL ---------- */

#define PHX_MAGIC 'P'
#define IOCTL_SET_MODE    _IOW(PHX_MAGIC, 1, int)
#define IOCTL_SET_SYSCALL _IOW(PHX_MAGIC, 2, int)
#define IOCTL_SET_PID     _IOW(PHX_MAGIC, 3, int)

/* ---------- COMMON MATCH CHECK ---------- */

static int syscall_matches_target(int wanted_syscall)
{
    if (selected_syscall != wanted_syscall)
        return 0;

    if (target_pid != -1 && current->pid != target_pid)
        return 0;

    return 1;
}

/* ---------- HANDLERS ---------- */

static int write_handler(struct kprobe *p, struct pt_regs *regs)
{
    unsigned long fd;
    unsigned long count;

    if (mode == MODE_OFF)
        return 0;

    if (!syscall_matches_target(SYSCALL_WRITE))
        return 0;

    fd = regs->di;
    count = regs->dx;

    if (mode == MODE_LOG) {
        printk(KERN_INFO "[phoenix] write syscall pid=%d comm=%s fd=%lu count=%lu\n",
               current->pid, current->comm, fd, count);
    } else if (mode == MODE_BLOCK) {
        printk(KERN_INFO "[phoenix] BLOCK match for write pid=%d comm=%s fd=%lu count=%lu\n",
               current->pid, current->comm, fd, count);
    }

    return 0;
}

static int read_handler(struct kprobe *p, struct pt_regs *regs)
{
    unsigned long fd;
    unsigned long count;

    if (mode == MODE_OFF)
        return 0;

    if (!syscall_matches_target(SYSCALL_READ))
        return 0;

    fd = regs->di;
    count = regs->dx;

    if (mode == MODE_LOG) {
        printk(KERN_INFO "[phoenix] read syscall pid=%d comm=%s fd=%lu count=%lu\n",
               current->pid, current->comm, fd, count);
    } else if (mode == MODE_BLOCK) {
        printk(KERN_INFO "[phoenix] BLOCK match for read pid=%d comm=%s fd=%lu count=%lu\n",
               current->pid, current->comm, fd, count);
    }

    return 0;
}

/* ---------- KPROBES ---------- */

static struct kprobe kp_write = {
    .symbol_name = "ksys_write",
    .pre_handler = write_handler,
};

static struct kprobe kp_read = {
    .symbol_name = "ksys_read",
    .pre_handler = read_handler,
};

/* ---------- IOCTL ---------- */

static long phoenix_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int val;

    switch (cmd) {
    case IOCTL_SET_MODE:
        if (copy_from_user(&val, (int __user *)arg, sizeof(int)))
            return -EFAULT;
        mode = val;
        printk(KERN_INFO "[phoenix] mode=%d\n", mode);
        break;

    case IOCTL_SET_SYSCALL:
        if (copy_from_user(&val, (int __user *)arg, sizeof(int)))
            return -EFAULT;
        selected_syscall = val;
        printk(KERN_INFO "[phoenix] selected_syscall=%d\n", selected_syscall);
        break;

    case IOCTL_SET_PID:
        if (copy_from_user(&val, (int __user *)arg, sizeof(int)))
            return -EFAULT;
        target_pid = val;
        printk(KERN_INFO "[phoenix] target_pid=%d\n", target_pid);
        break;

    default:
        return -EINVAL;
    }

    return 0;
}

/* ---------- DEVICE ---------- */

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = phoenix_ioctl,
};

static struct miscdevice phoenix_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "phoenix_ctl",
    .fops  = &fops,
};

/* ---------- INIT / EXIT ---------- */

static int __init phoenix_init(void)
{
    int ret;

    printk(KERN_INFO "[phoenix] loading\n");

    ret = misc_register(&phoenix_device);
    if (ret)
        return ret;

    ret = register_kprobe(&kp_write);
    if (ret) {
        misc_deregister(&phoenix_device);
        return ret;
    }

    ret = register_kprobe(&kp_read);
    if (ret) {
        unregister_kprobe(&kp_write);
        misc_deregister(&phoenix_device);
        return ret;
    }

    printk(KERN_INFO "[phoenix] ready (/dev/phoenix_ctl)\n");
    return 0;
}

static void __exit phoenix_exit(void)
{
    unregister_kprobe(&kp_read);
    unregister_kprobe(&kp_write);
    misc_deregister(&phoenix_device);
    printk(KERN_INFO "[phoenix] unloaded\n");
}

module_init(phoenix_init);
module_exit(phoenix_exit);