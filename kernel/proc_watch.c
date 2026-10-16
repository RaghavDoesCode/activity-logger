#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/sched.h>

/* ---- Probe 1: process exit (kprobe on do_exit) ---- */
static int exit_pre(struct kprobe *p, struct pt_regs *regs)
{
    /* only report the main thread to avoid duplicate lines */
    if (current->pid != current->tgid)
        return 0;

    pr_info("proc_watch: EXIT pid=%d ppid=%d comm=%s\n",
            current->tgid, task_ppid_nr(current), current->comm);
    return 0;
}

static struct kprobe exit_probe = {
    .symbol_name = "do_exit",
    .pre_handler = exit_pre,
};

/* ---- Probe 2: process start (kretprobe on begin_new_exec) ---- */
/* Runs when begin_new_exec() returns, so comm is already the new name */
static int exec_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    if ((long)regs_return_value(regs) != 0)   /* exec failed, ignore */
        return 0;

    pr_info("proc_watch: EXEC pid=%d ppid=%d comm=%s\n",
            current->tgid, task_ppid_nr(current), current->comm);
    return 0;
}

static struct kretprobe exec_probe = {
    .kp.symbol_name = "begin_new_exec",
    .handler        = exec_ret,
    .maxactive      = 32,   /* how many calls can be in flight at once */
};

static int __init proc_watch_init(void)
{
    int ret;

    ret = register_kprobe(&exit_probe);
    if (ret < 0) {
        pr_err("proc_watch: exit probe failed (%d)\n", ret);
        return ret;
    }

    ret = register_kretprobe(&exec_probe);
    if (ret < 0) {
        pr_err("proc_watch: exec probe failed (%d)\n", ret);
        unregister_kprobe(&exit_probe);
        return ret;
    }

    pr_info("proc_watch: loaded, watching exec and exit\n");
    return 0;
}

static void __exit proc_watch_exit(void)
{
    unregister_kretprobe(&exec_probe);
    unregister_kprobe(&exit_probe);
    pr_info("proc_watch: unloaded\n");
}

module_init(proc_watch_init);
module_exit(proc_watch_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Reports process start (exec) and exit via kprobes");
