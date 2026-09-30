#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>

#include "nd_ioctl.h"

static long nd_ioctl(struct file *file,
                     unsigned int cmd,
                     unsigned long arg)
{
    struct nd_read_request req;

    switch (cmd) {
    case ND_IOCTL_READ:
        if (copy_from_user(&req,
                           (void __user *)arg,
                           sizeof(req)))
            return -EFAULT;

        pr_info("nd: READ sector=%llu length=%u\n",
                (unsigned long long)req.sector,
                req.length);

        /*
         * TODO:
         * Send ND_READ request to Luckfox through USB.
         */

        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations nd_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = nd_ioctl,
};

static struct miscdevice nd_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "nd0",
    .fops  = &nd_fops,
};

static int __init nd_init(void)
{
    int ret;

    ret = misc_register(&nd_device);
    if (ret) {
        pr_err("nd: failed to register misc device: %d\n", ret);
        return ret;
    }

    pr_info("nd: registered /dev/nd0\n");

    return 0;
}

static void __exit nd_exit(void)
{
    misc_deregister(&nd_device);

    pr_info("nd: unloaded\n");
}

module_init(nd_init);
module_exit(nd_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Luckfox Near-Data Project");
MODULE_DESCRIPTION("Near-Data host driver");