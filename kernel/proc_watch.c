#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/kfifo.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/timekeeping.h>
#include <linux/math64.h>

/* ---- shared buffer: probes write, the device read() consumes ---- */
static DEFINE_KFIFO(event_fifo, char, 65536);   /* size must be a power of 2 */
static DEFINE_SPINLOCK(fifo_lock);
static DECLARE_WAIT_QUEUE_HEAD(read_wq);
static unsigned long dropped;                   /* events lost when full */

/* Format one event line and put it in the buffer. Called from probes,
 * which run in atomic context, so nothing here may sleep. */
static void push_event(const char *type)
{
    char line[128];
    unsigned long flags;
    int len;

    len = scnprintf(line, sizeof(line), "%s %d %d %llu %s\n",
                    type, current->tgid, task_ppid_nr(current),
                    div_u64(ktime_get_real_ns(), 1000), current->comm);

    spin_lock_irqsave(&fifo_lock, flags);
    if (kfifo_avail(&event_fifo) >= len)
        kfifo_in(&event_fifo, line, len);
    else
        dropped++;
    spin_unlock_irqrestore(&fifo_lock, flags);

    wake_up_interruptible(&read_wq);            /* wake a sleeping reader */
}

/* ---- Probe 1: process exit ---- */
static int exit_pre(struct kprobe *p, struct pt_regs *regs)
{
    if (current->pid != current->tgid)          /* main thread only */
        return 0;
    push_event("EXIT");
    return 0;
}

static struct kprobe exit_probe = {
    .symbol_name = "do_exit",
    .pre_handler = exit_pre,
};

/* ---- Probe 2: process start (runs when begin_new_exec returns) ---- */
static int exec_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    if ((long)regs_return_value(regs) != 0)     /* exec failed */
        return 0;
    push_event("EXEC");
    return 0;
}

static struct kretprobe exec_probe = {
    .kp.symbol_name = "begin_new_exec",
    .handler        = exec_ret,
    .maxactive      = 32,
};

/* ---- the device file: /dev/activity_logger ---- */
static ssize_t dev_read(struct file *f, char __user *ubuf,
                        size_t count, loff_t *off)
{
    char kbuf[256];
    unsigned long flags;
    unsigned int n;
    int ret;

    if (count > sizeof(kbuf))
        count = sizeof(kbuf);

    for (;;) {
        /* sleep until the buffer has data (or a signal arrives) */
        ret = wait_event_interruptible(read_wq, !kfifo_is_empty(&event_fifo));
        if (ret)
            return ret;

        spin_lock_irqsave(&fifo_lock, flags);
        n = kfifo_out(&event_fifo, kbuf, count);
        spin_unlock_irqrestore(&fifo_lock, flags);

        if (n)
            break;
    }

    /* copy_to_user crosses the kernel/user boundary; it may fault,
     * so it must be done outside the spinlock */
    if (copy_to_user(ubuf, kbuf, n))
        return -EFAULT;
    return n;
}

/* A new reader starts with an empty buffer, so a session never sees
 * events that happened before it opened the device. */
static int dev_open(struct inode *inode, struct file *f)
{
    unsigned long flags;

    spin_lock_irqsave(&fifo_lock, flags);
    kfifo_reset(&event_fifo);
    spin_unlock_irqrestore(&fifo_lock, flags);
    return 0;
}

static const struct file_operations dev_fops = {
    .owner = THIS_MODULE,
    .open  = dev_open,
    .read  = dev_read,
};

static struct miscdevice dev_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "activity_logger",
    .fops  = &dev_fops,
    .mode  = 0400,                              /* root only */
};

static int __init proc_watch_init(void)
{
    int ret;

    ret = misc_register(&dev_misc);
    if (ret) {
        pr_err("proc_watch: misc_register failed (%d)\n", ret);
        return ret;
    }

    ret = register_kprobe(&exit_probe);
    if (ret < 0) {
        pr_err("proc_watch: exit probe failed (%d)\n", ret);
        goto err_misc;
    }

    ret = register_kretprobe(&exec_probe);
    if (ret < 0) {
        pr_err("proc_watch: exec probe failed (%d)\n", ret);
        goto err_exit_probe;
    }

    pr_info("proc_watch: loaded, reading from /dev/activity_logger\n");
    return 0;

err_exit_probe:
    unregister_kprobe(&exit_probe);
err_misc:
    misc_deregister(&dev_misc);
    return ret;
}

static void __exit proc_watch_exit(void)
{
    unregister_kretprobe(&exec_probe);
    unregister_kprobe(&exit_probe);
    misc_deregister(&dev_misc);
    pr_info("proc_watch: unloaded, %lu events dropped\n", dropped);
}

module_init(proc_watch_init);
module_exit(proc_watch_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Streams process exec/exit events via /dev/activity_logger");
