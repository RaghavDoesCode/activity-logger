#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/sched.h>

/* A kprobe lets us run our function whenever the kernel executes
 * a chosen kernel function. We attach it to do_exit(), which the
 * kernel runs whenever a task (thread/process) terminates. */
static struct kprobe kp = {
    .symbol_name = "do_exit",
};

/* Runs just before do_exit() executes, in the context of the
 * exiting task. 'current' points to that task's task_struct. */
static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    /* pid is the thread id; tgid is the process id.
     * Only report the main thread to avoid duplicate lines. */
    if (current->pid != current->tgid)
        return 0;

    pr_info("proc_watch: EXIT pid=%d ppid=%d comm=%s\n",
            current->tgid, task_ppid_nr(current), current->comm);
    return 0;
}

static int __init proc_watch_init(void)
{
    int ret;

    kp.pre_handler = handler_pre;
    ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("proc_watch: register_kprobe failed (%d)\n", ret);
        return ret;
    }
    pr_info("proc_watch: loaded, watching do_exit\n");
    return 0;
}

static void __exit proc_watch_exit(void)
{
    unregister_kprobe(&kp);
    pr_info("proc_watch: unloaded\n");
}

module_init(proc_watch_init);
module_exit(proc_watch_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Reports process exits via a kprobe");
