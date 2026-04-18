/* monitor.c - Kernel Memory Monitor LKM */
#define pr_fmt(fmt) "container_monitor: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/signal.h>
#include <linux/jiffies.h>
#include <linux/version.h>
#include "monitor_ioctl.h"

#define DEVICE_NAME       "container_monitor"
#define CHECK_INTERVAL_MS  2000

/* Per-container tracking entry */
struct monitored_container {
    struct list_head list;
    pid_t  pid;
    char   id[MONITOR_NAME_LEN];
    long   soft_limit_bytes;
    long   hard_limit_bytes;
    bool   soft_warned;
};

/*
 * Global list of monitored containers, protected by list_mutex.
 * A mutex is used (rather than a spinlock) because the timer callback
 * calls get_task_mm / mmput which can sleep, making a spinlock unsafe.
 */
static LIST_HEAD(container_list);
static DEFINE_MUTEX(list_mutex);

static dev_t            dev_num;
static struct cdev      cdev;
static struct class    *dev_class;
static struct timer_list check_timer;

/* ---------------------------------------------------------------
 * RSS Helper — returns resident set size in bytes for pid,
 * or -1 if the task no longer exists.
 * --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss = get_mm_rss(mm) << PAGE_SHIFT;
        mmput(mm);
    }
    put_task_struct(task);
    return rss;
}

/* ---------------------------------------------------------------
 * Send signal to a process by PID.
 * --------------------------------------------------------------- */
static int send_signal_to_pid(pid_t pid, int sig)
{
    struct task_struct *task;
    int ret = -ESRCH;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task) {
        get_task_struct(task);
        rcu_read_unlock();
        ret = send_sig(sig, task, 1);
        put_task_struct(task);
    } else {
        rcu_read_unlock();
    }
    return ret;
}

/* ---------------------------------------------------------------
 * Timer callback — fires every CHECK_INTERVAL_MS milliseconds.
 * Iterates the list, checks RSS, emits warnings, enforces limits.
 * --------------------------------------------------------------- */
static void check_containers(struct timer_list *t)
{
    struct monitored_container *entry, *tmp;

    mutex_lock(&list_mutex);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        long rss = get_rss_bytes(entry->pid);

        if (rss < 0) {
            /* Process has exited — clean up the entry */
            pr_info("container %s (pid %d) exited, removing\n",
                    entry->id, entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (rss > entry->hard_limit_bytes) {
            pr_warn("HARD LIMIT: container=%s pid=%d rss=%ldMiB hard=%ldMiB — killing\n",
                    entry->id, entry->pid,
                    rss / (1024 * 1024),
                    entry->hard_limit_bytes / (1024 * 1024));
            send_signal_to_pid(entry->pid, SIGKILL);
            list_del(&entry->list);
            kfree(entry);
        } else if (!entry->soft_warned && rss > entry->soft_limit_bytes) {
            pr_warn("SOFT LIMIT: container=%s pid=%d rss=%ldMiB soft=%ldMiB — warning\n",
                    entry->id, entry->pid,
                    rss / (1024 * 1024),
                    entry->soft_limit_bytes / (1024 * 1024));
            entry->soft_warned = true;
        }
    }
    mutex_unlock(&list_mutex);

    mod_timer(&check_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));
}

/* ---------------------------------------------------------------
 * ioctl handler — MONITOR_REGISTER and MONITOR_UNREGISTER.
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    (void)file;

    switch (cmd) {

    case MONITOR_REGISTER: {
        struct container_reg reg;
        struct monitored_container *entry;

        if (copy_from_user(&reg, (void __user *)arg, sizeof(reg)))
            return -EFAULT;

        if (reg.soft_limit_bytes > reg.hard_limit_bytes) {
            pr_warn("register rejected: soft > hard for container %s\n", reg.id);
            return -EINVAL;
        }

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid              = reg.pid;
        entry->soft_limit_bytes = reg.soft_limit_bytes;
        entry->hard_limit_bytes = reg.hard_limit_bytes;
        entry->soft_warned      = false;
        strncpy(entry->id, reg.id, MONITOR_NAME_LEN - 1);
        entry->id[MONITOR_NAME_LEN - 1] = '\0';
        INIT_LIST_HEAD(&entry->list);

        mutex_lock(&list_mutex);
        list_add_tail(&entry->list, &container_list);
        mutex_unlock(&list_mutex);

        pr_info("registered container=%s pid=%d soft=%ldMiB hard=%ldMiB\n",
                entry->id, entry->pid,
                entry->soft_limit_bytes / (1024 * 1024),
                entry->hard_limit_bytes / (1024 * 1024));
        return 0;
    }

    case MONITOR_UNREGISTER: {
        struct container_unreg req;
        struct monitored_container *entry, *tmp;
        int found = 0;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;

        mutex_lock(&list_mutex);
        list_for_each_entry_safe(entry, tmp, &container_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                pr_info("unregistered pid=%d\n", req.pid);
                found = 1;
                break;
            }
        }
        mutex_unlock(&list_mutex);

        return found ? 0 : -ENOENT;
    }

    default:
        return -ENOTTY;
    }
}

static int monitor_open(struct inode *inode, struct file *file)
{
    (void)inode; (void)file;
    return 0;
}

static int monitor_release(struct inode *inode, struct file *file)
{
    (void)inode; (void)file;
    return 0;
}

static const struct file_operations monitor_fops = {
    .owner          = THIS_MODULE,
    .open           = monitor_open,
    .release        = monitor_release,
    .unlocked_ioctl = monitor_ioctl,
};

/* ---------------------------------------------------------------
 * Module init / exit
 * --------------------------------------------------------------- */
static int __init monitor_init(void)
{
    int ret;
    struct device *dev;

    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    cdev_init(&cdev, &monitor_fops);
    ret = cdev_add(&cdev, dev_num, 1);
    if (ret < 0) {
        unregister_chrdev_region(dev_num, 1);
        return ret;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    dev_class = class_create(DEVICE_NAME);
#else
    dev_class = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(dev_class)) {
        cdev_del(&cdev);
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(dev_class);
    }

    dev = device_create(dev_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(dev)) {
        class_destroy(dev_class);
        cdev_del(&cdev);
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(dev);
    }

    timer_setup(&check_timer, check_containers, 0);
    mod_timer(&check_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));

    pr_info("loaded — /dev/%s ready\n", DEVICE_NAME);
    return 0;
}

static void __exit monitor_exit(void)
{
    struct monitored_container *entry, *tmp;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
    timer_delete_sync(&check_timer);
#else
    del_timer_sync(&check_timer);
#endif

    /* Free all remaining tracked entries */
    mutex_lock(&list_mutex);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&list_mutex);

    device_destroy(dev_class, dev_num);
    class_destroy(dev_class);
    cdev_del(&cdev);
    unregister_chrdev_region(dev_num, 1);

    pr_info("unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS-Jackfruit Team");
MODULE_DESCRIPTION("Multi-container memory monitor LKM");
