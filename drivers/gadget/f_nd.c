// SPDX-License-Identifier: GPL-2.0
/*
* f_nd.c - Near-Data USB Gadget Function
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/workqueue.h>
#include <linux/miscdevice.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/usb/composite.h>

#include "u_f.h"
#include "nd_protocol.h"

struct f_nd {
    struct usb_function function;
    struct usb_ep *out_ep;
    struct usb_ep *in_ep;
    struct work_struct read_work;
    struct usb_request *pending_out;
    bool enabled;
    bool dying;
    struct miscdevice rpc_misc;
    struct kfifo rx_fifo, tx_fifo;
    spinlock_t fifo_lock;
    wait_queue_head_t stream_wait;
    atomic_t opened;
    unsigned int generation;
    struct kref refs;
};

static inline struct f_nd *func_to_nd(struct usb_function *f)
{
    return container_of(f, struct f_nd, function);
}


/* USB workers produce RX bytes and consume TX bytes; one userspace executor
 * owns /dev/nd_rpc. A session reset invalidates its fd so the supervisor can
 * restart it and discard image/region state from an interrupted client.
 */
struct nd_stream_file {
    struct f_nd *nd;
    unsigned int generation;
};
static void nd_destroy(struct kref *ref)
{
    struct f_nd *nd = container_of(ref, struct f_nd, refs);
    kfifo_free(&nd->rx_fifo);
    kfifo_free(&nd->tx_fifo);
    kfree(nd);
}
static void nd_stream_reset(struct f_nd *nd)
{
    unsigned long flags;
    spin_lock_irqsave(&nd->fifo_lock, flags);
    ++nd->generation;
    kfifo_reset(&nd->rx_fifo);
    kfifo_reset(&nd->tx_fifo);
    spin_unlock_irqrestore(&nd->fifo_lock, flags);
    wake_up_interruptible(&nd->stream_wait);
}
static bool nd_stale(struct nd_stream_file *ctx)
{
    return READ_ONCE(ctx->nd->dying) ||
           ctx->generation != READ_ONCE(ctx->nd->generation);
}
static int nd_stream_open(struct inode *inode, struct file *file)
{
    struct miscdevice *misc = file->private_data;
    struct f_nd *nd = container_of(misc, struct f_nd, rpc_misc);
    struct nd_stream_file *ctx;
    if (READ_ONCE(nd->dying))
        return -ENODEV;
    if (atomic_cmpxchg(&nd->opened, 0, 1))
        return -EBUSY;
    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx) { atomic_set(&nd->opened, 0); return -ENOMEM; }
    kref_get(&nd->refs);
    ctx->nd = nd;
    ctx->generation = READ_ONCE(nd->generation);
    file->private_data = ctx;
    return nonseekable_open(inode, file);
}
static int nd_stream_release(struct inode *inode, struct file *file)
{
    struct nd_stream_file *ctx = file->private_data;
    atomic_set(&ctx->nd->opened, 0);
    wake_up_interruptible(&ctx->nd->stream_wait);
    kref_put(&ctx->nd->refs, nd_destroy);
    kfree(ctx);
    return 0;
}
static ssize_t nd_stream_read(struct file *file, char __user *buf,
                              size_t count, loff_t *pos)
{
    struct nd_stream_file *ctx = file->private_data;
    struct f_nd *nd = ctx->nd;
    unsigned long flags;
    unsigned int n;
    u8 *tmp;
    int ret;
    if (!count) return 0;
    count = min_t(size_t, count, ND_MAX_READ);
    for (;;) {
        if (nd_stale(ctx)) return -EPIPE;
        if (READ_ONCE(nd->enabled) && !kfifo_is_empty(&nd->rx_fifo)) break;
        if (file->f_flags & O_NONBLOCK) return -EAGAIN;
        ret = wait_event_interruptible(nd->stream_wait,
            nd_stale(ctx) || (READ_ONCE(nd->enabled) && !kfifo_is_empty(&nd->rx_fifo)));
        if (ret) return ret;
    }
    tmp = kmalloc(count, GFP_KERNEL);
    if (!tmp) return -ENOMEM;
    spin_lock_irqsave(&nd->fifo_lock, flags);
    n = nd_stale(ctx) ? 0 : kfifo_out(&nd->rx_fifo, tmp, count);
    spin_unlock_irqrestore(&nd->fifo_lock, flags);
    ret = n ? (copy_to_user(buf, tmp, n) ? -EFAULT : n) : -EPIPE;
    kfree(tmp);
    wake_up_interruptible(&nd->stream_wait);
    return ret;
}
static ssize_t nd_stream_write(struct file *file, const char __user *buf,
                               size_t count, loff_t *pos)
{
    struct nd_stream_file *ctx = file->private_data;
    struct f_nd *nd = ctx->nd;
    unsigned long flags;
    unsigned int n;
    u8 *tmp;
    int ret;
    if (!count) return 0;
    count = min_t(size_t, count, ND_MAX_READ);
    tmp = memdup_user(buf, count);
    if (IS_ERR(tmp)) return PTR_ERR(tmp);
    for (;;) {
        if (nd_stale(ctx)) { ret = -EPIPE; goto out; }
        if (READ_ONCE(nd->enabled) && kfifo_avail(&nd->tx_fifo)) break;
        if (file->f_flags & O_NONBLOCK) { ret = -EAGAIN; goto out; }
        ret = wait_event_interruptible(nd->stream_wait,
            nd_stale(ctx) || (READ_ONCE(nd->enabled) && kfifo_avail(&nd->tx_fifo)));
        if (ret) goto out;
    }
    spin_lock_irqsave(&nd->fifo_lock, flags);
    n = nd_stale(ctx) ? 0 : kfifo_in(&nd->tx_fifo, tmp, count);
    spin_unlock_irqrestore(&nd->fifo_lock, flags);
    ret = n ? n : -EPIPE;
out:
    kfree(tmp);
    wake_up_interruptible(&nd->stream_wait);
    return ret;
}
static __poll_t nd_stream_poll(struct file *file, poll_table *wait)
{
    struct nd_stream_file *ctx = file->private_data;
    struct f_nd *nd = ctx->nd;
    __poll_t mask = 0;
    poll_wait(file, &nd->stream_wait, wait);
    if (nd_stale(ctx)) return EPOLLERR | EPOLLHUP;
    if (!READ_ONCE(nd->enabled)) return 0;
    if (!kfifo_is_empty(&nd->rx_fifo)) mask |= EPOLLIN | EPOLLRDNORM;
    if (kfifo_avail(&nd->tx_fifo)) mask |= EPOLLOUT | EPOLLWRNORM;
    return mask;
}
static const struct file_operations nd_stream_fops = {
    .owner = THIS_MODULE, .open = nd_stream_open, .release = nd_stream_release,
    .read = nd_stream_read, .write = nd_stream_write, .poll = nd_stream_poll,
    .llseek = no_llseek,
};

/* -------------------------------------------------------------------------- */
/* USB descriptors                                                            */

static struct usb_interface_descriptor nd_intf_desc = {
    .bLength            = USB_DT_INTERFACE_SIZE,
    .bDescriptorType    = USB_DT_INTERFACE,
    .bAlternateSetting  = 0,
    .bNumEndpoints      = 2,
    .bInterfaceClass    = USB_CLASS_VENDOR_SPEC,
    .bInterfaceSubClass = 0,
    .bInterfaceProtocol = 0,
};


/* Full Speed */

static struct usb_endpoint_descriptor nd_fs_out_desc = {
    .bLength          = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType  = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_OUT,
    .bmAttributes     = USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor nd_fs_in_desc = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_IN,
    .bmAttributes = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize = cpu_to_le16(64),
};

static struct usb_descriptor_header *nd_fs_descs[] = {
    (struct usb_descriptor_header *)&nd_intf_desc,
    (struct usb_descriptor_header *)&nd_fs_out_desc,
    (struct usb_descriptor_header *)&nd_fs_in_desc,
    NULL,
};


/* High Speed */

static struct usb_endpoint_descriptor nd_hs_out_desc = {
    .bLength          = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType  = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_OUT,
    .bmAttributes     = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize   = cpu_to_le16(512),
};

static struct usb_endpoint_descriptor nd_hs_in_desc = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_IN,
    .bmAttributes = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize = cpu_to_le16(512),
};

static struct usb_descriptor_header *nd_hs_descs[] = {
    (struct usb_descriptor_header *)&nd_intf_desc,
    (struct usb_descriptor_header *)&nd_hs_out_desc,
    (struct usb_descriptor_header *)&nd_hs_in_desc,
    NULL,
};


/* -------------------------------------------------------------------------- */
/* Receive command                                                            */

/* One request in flight: OUT -> worker SD read -> IN -> requeue OUT.
 * Filesystem/block reads run in process context, never USB completion context.
 */
static void nd_requeue_out(struct f_nd *nd, struct usb_request *out)
{
    if (!READ_ONCE(nd->enabled) ||
        usb_ep_queue(nd->out_ep, out, GFP_ATOMIC))
        free_ep_req(nd->out_ep, out);
}
static void nd_in_complete(struct usb_ep *ep, struct usb_request *req)
{
    struct f_nd *nd = ep->driver_data;
    struct usb_request *out = req->context;
    int status = req->status;
    free_ep_req(ep, req);
    if (!status) nd_requeue_out(nd, out);
    else free_ep_req(nd->out_ep, out);
}
static ssize_t nd_rpc_push(struct f_nd *nd, const void *data, u32 length)
{
    unsigned long flags;
    unsigned int n;
    long ret;
    ret = wait_event_interruptible_timeout(nd->stream_wait,
        !READ_ONCE(nd->enabled) || READ_ONCE(nd->dying) ||
        kfifo_avail(&nd->rx_fifo) >= length, msecs_to_jiffies(20000));
    if (ret < 0) return ret;
    if (!ret) return -ETIMEDOUT;
    spin_lock_irqsave(&nd->fifo_lock, flags);
    n = nd->enabled && !nd->dying ? kfifo_in(&nd->rx_fifo, data, length) : 0;
    spin_unlock_irqrestore(&nd->fifo_lock, flags);
    wake_up_interruptible(&nd->stream_wait);
    return n == length ? 0 : -ESHUTDOWN;
}
static ssize_t nd_rpc_pull(struct f_nd *nd, void *data, u32 length)
{
    unsigned long flags;
    unsigned int n;
    long ret;
    ret = wait_event_interruptible_timeout(nd->stream_wait,
        !READ_ONCE(nd->enabled) || READ_ONCE(nd->dying) ||
        !kfifo_is_empty(&nd->tx_fifo), msecs_to_jiffies(20000));
    if (ret < 0) return ret;
    if (!ret) return -ETIMEDOUT;
    spin_lock_irqsave(&nd->fifo_lock, flags);
    n = nd->enabled && !nd->dying ? kfifo_out(&nd->tx_fifo, data, length) : 0;
    spin_unlock_irqrestore(&nd->fifo_lock, flags);
    wake_up_interruptible(&nd->stream_wait);
    return n ? n : -ESHUTDOWN;
}
static void nd_read_work(struct work_struct *work)
{
    struct f_nd *nd = container_of(work, struct f_nd, read_work);
    struct usb_request *out = nd->pending_out, *in;
    struct nd_command cmd;
    struct nd_response *response;
    struct file *sd;
    loff_t offset;
    u64 sector;
    u32 length;
    ssize_t result = -EPROTO;
    nd->pending_out = NULL;
    in = alloc_ep_req(nd->in_ep, sizeof(*response) + ND_MAX_READ);
    if (!in) { nd_requeue_out(nd, out); return; }
    response = in->buf;
    memset(response, 0, sizeof(*response));
    if (out->actual < sizeof(cmd)) goto respond;
    memcpy(&cmd, out->buf, sizeof(cmd));
    if (le16_to_cpu(cmd.magic) != ND_PROTOCOL_MAGIC) goto respond;
    length = le32_to_cpu(cmd.length);
    switch (le16_to_cpu(cmd.opcode)) {
    case ND_CMD_RPC_RESET:
        if (out->actual != sizeof(cmd) || length) goto respond;
        nd_stream_reset(nd);
        result = 0;
        break;
    case ND_CMD_RPC_OUT:
        if (!length || length > ND_MAX_READ || out->actual != sizeof(cmd) + ND_MAX_READ)
            goto respond;
        result = nd_rpc_push(nd, (u8 *)out->buf + sizeof(cmd), length);
        break;
    case ND_CMD_RPC_IN:
        if (!length || length > ND_MAX_READ || out->actual != sizeof(cmd))
            goto respond;
        result = nd_rpc_pull(nd, (u8 *)in->buf + sizeof(*response), length);
        break;
    case ND_CMD_READ:
        sector = le64_to_cpu(cmd.sector);
        if (out->actual != sizeof(cmd) || !length || length > ND_MAX_READ ||
            sector > ((u64)S64_MAX - length) / 512) {
            result = -EINVAL;
            break;
        }
        offset = sector * 512;
        sd = filp_open("/dev/mmcblk1", O_RDONLY | O_LARGEFILE, 0);
        if (IS_ERR(sd)) { result = PTR_ERR(sd); break; }
        result = kernel_read(sd, (u8 *)in->buf + sizeof(*response), length, &offset);
        filp_close(sd, NULL);
        pr_info("nd: READ sector=%llu length=%u result=%zd\n",
                (unsigned long long)sector, length, result);
        break;
    default:
        result = -EOPNOTSUPP;
    }
respond:
    response->magic = cpu_to_le16(ND_PROTOCOL_MAGIC);
    response->version = cpu_to_le16(ND_RESPONSE_VERSION);
    response->status = cpu_to_le32(result < 0 ? (s32)result : 0);
    response->length = cpu_to_le32(result > 0 ? result : 0);
    in->length = sizeof(*response) + (result > 0 ? result : 0);
    in->zero = 1;
    in->complete = nd_in_complete;
    in->context = out;
    if (!READ_ONCE(nd->enabled) || usb_ep_queue(nd->in_ep, in, GFP_KERNEL)) {
        free_ep_req(nd->in_ep, in);
        nd_requeue_out(nd, out);
    }
}
static void nd_out_complete(struct usb_ep *ep, struct usb_request *req)
{
    struct f_nd *nd = ep->driver_data;
    if (req->status || !READ_ONCE(nd->enabled)) { free_ep_req(ep, req); return; }
    nd->pending_out = req;
    schedule_work(&nd->read_work);
}

/* -------------------------------------------------------------------------- */
/* Endpoint                                                                   */

static int nd_start_out(struct f_nd *nd)
{
    struct usb_request *req;
    int ret;

    req = alloc_ep_req(nd->out_ep,
                    sizeof(struct nd_command) + ND_MAX_READ);
    if (!req)
        return -ENOMEM;

    req->complete = nd_out_complete;

    ret = usb_ep_queue(nd->out_ep, req, GFP_ATOMIC);
    if (ret) {
        free_ep_req(nd->out_ep, req);
        return ret;
    }

    return 0;
}


/* -------------------------------------------------------------------------- */
/* USB function                                                               */

static int nd_bind(struct usb_configuration *c,
                struct usb_function *f)
{
    struct usb_composite_dev *cdev = c->cdev;
    struct f_nd *nd = func_to_nd(f);
    int id;
    int ret;

    id = usb_interface_id(c, f);
    if (id < 0)
        return id;

    nd_intf_desc.bInterfaceNumber = id;

    nd->out_ep =
        usb_ep_autoconfig(cdev->gadget,
                        &nd_fs_out_desc);

    if (!nd->out_ep) {
        pr_err("nd: cannot autoconfigure OUT endpoint\n");
        return -ENODEV;
    }

    /*
    * High-speed endpoint must use the same endpoint
    * address selected by usb_ep_autoconfig().
    */
    nd_hs_out_desc.bEndpointAddress =
        nd_fs_out_desc.bEndpointAddress;
    nd->in_ep = usb_ep_autoconfig(cdev->gadget, &nd_fs_in_desc);
    if (!nd->in_ep)
        return -ENODEV;
    nd_hs_in_desc.bEndpointAddress = nd_fs_in_desc.bEndpointAddress;

    ret = usb_assign_descriptors(
        f,
        nd_fs_descs,
        nd_hs_descs,
        NULL,
        NULL);

    if (ret)
        return ret;

    pr_info("nd: gadget function bound, OUT=%s\n",
            nd->out_ep->name);

    return 0;
}


static int nd_set_alt(struct usb_function *f,
                    unsigned intf,
                    unsigned alt)
{
    struct f_nd *nd = func_to_nd(f);
    struct usb_composite_dev *cdev = f->config->cdev;
    int ret;

    if (alt != 0)
        return -EINVAL;
    /* A reset during a block read is retried after that worker finishes. */
    if (work_busy(&nd->read_work))
        return -EBUSY;
    WRITE_ONCE(nd->enabled, false);
    nd_stream_reset(nd);

    usb_ep_disable(nd->out_ep);
    usb_ep_disable(nd->in_ep);

    ret = config_ep_by_speed(cdev->gadget,
                            f,
                            nd->out_ep);
    if (ret)
        return ret;

    ret = usb_ep_enable(nd->out_ep);
    if (ret)
        return ret;

    ret = config_ep_by_speed(cdev->gadget, f, nd->in_ep);
    if (ret)
        goto disable_out;
    ret = usb_ep_enable(nd->in_ep);
    if (ret)
        goto disable_out;
    nd->in_ep->driver_data = nd;
    nd->out_ep->driver_data = nd;
    WRITE_ONCE(nd->enabled, true);
    wake_up_interruptible(&nd->stream_wait);

    ret = nd_start_out(nd);
    if (ret) {
        WRITE_ONCE(nd->enabled, false);
        usb_ep_disable(nd->in_ep);
        usb_ep_disable(nd->out_ep);
        return ret;
    }

    pr_info("nd: OUT and IN endpoints enabled (READ + runtime stream)\n");

    return 0;
disable_out:
    usb_ep_disable(nd->out_ep);
    return ret;
}


static void nd_disable(struct usb_function *f)
{
    struct f_nd *nd = func_to_nd(f);

    if (!nd->out_ep)
        return;

    WRITE_ONCE(nd->enabled, false);
    nd_stream_reset(nd);
    usb_ep_disable(nd->out_ep);
    usb_ep_disable(nd->in_ep);
    /* driver_data lives until free_func; workers can still finish after reset. */

    pr_info("nd: gadget function disabled\n");
}


static void nd_free_func(struct usb_function *f)
{
    struct f_nd *nd = func_to_nd(f);

    WRITE_ONCE(nd->dying, true);
    WRITE_ONCE(nd->enabled, false);
    nd_stream_reset(nd);
    misc_deregister(&nd->rpc_misc);
    cancel_work_sync(&nd->read_work);
    if (nd->pending_out) {
        free_ep_req(nd->out_ep, nd->pending_out);
        nd->pending_out = NULL;
    }
    usb_free_all_descriptors(f);
    kref_put(&nd->refs, nd_destroy);
}
 

/* -------------------------------------------------------------------------- */
/* ConfigFS instance                                                          */

struct f_nd_opts {
    struct usb_function_instance func_inst;
};


static inline struct f_nd_opts *
to_f_nd_opts(struct config_item *item)
{
    return container_of(to_config_group(item),
                        struct f_nd_opts,
                        func_inst.group);
}


static void nd_attr_release(struct config_item *item)
{
    struct f_nd_opts *opts = to_f_nd_opts(item);

    usb_put_function_instance(&opts->func_inst);
}


static struct configfs_item_operations nd_item_ops = {
    .release = nd_attr_release,
};


static const struct config_item_type nd_func_type = {
    .ct_item_ops = &nd_item_ops,
    .ct_owner = THIS_MODULE,
};


static void nd_free_instance(struct usb_function_instance *fi)
{
    struct f_nd_opts *opts;

    opts = container_of(fi,
                        struct f_nd_opts,
                        func_inst);

    kfree(opts);
}


static struct usb_function_instance *nd_alloc_inst(void)
{
    struct f_nd_opts *opts;

    opts = kzalloc(sizeof(*opts), GFP_KERNEL);
    if (!opts)
        return ERR_PTR(-ENOMEM);

    opts->func_inst.free_func_inst =
        nd_free_instance;

    config_group_init_type_name(
        &opts->func_inst.group,
        "",
        &nd_func_type);

    return &opts->func_inst;
}


static struct usb_function *
nd_alloc_func(struct usb_function_instance *fi)
{
    struct f_nd *nd;

    nd = kzalloc(sizeof(*nd), GFP_KERNEL);
    if (!nd)
        return ERR_PTR(-ENOMEM);

    spin_lock_init(&nd->fifo_lock);
    init_waitqueue_head(&nd->stream_wait);
    atomic_set(&nd->opened, 0);
    kref_init(&nd->refs);
    if (kfifo_alloc(&nd->rx_fifo, 64 * 1024, GFP_KERNEL)) {
        kfree(nd);
        return ERR_PTR(-ENOMEM);
    }
    if (kfifo_alloc(&nd->tx_fifo, 64 * 1024, GFP_KERNEL)) {
        kfifo_free(&nd->rx_fifo);
        kfree(nd);
        return ERR_PTR(-ENOMEM);
    }
    nd->rpc_misc.minor = MISC_DYNAMIC_MINOR;
    nd->rpc_misc.name = "nd_rpc";
    nd->rpc_misc.fops = &nd_stream_fops;
    {
        int ret = misc_register(&nd->rpc_misc);
        if (ret) {
            kref_put(&nd->refs, nd_destroy);
            return ERR_PTR(ret);
        }
    }
    INIT_WORK(&nd->read_work, nd_read_work);
    nd->function.name     = "nd";
    nd->function.bind     = nd_bind;
    nd->function.set_alt  = nd_set_alt;
    nd->function.disable  = nd_disable;
    nd->function.free_func = nd_free_func;

    return &nd->function;
}


/*
* Registers the ConfigFS function name "nd".
*
* This makes:
*
*     mkdir functions/nd.0
*
* possible.
*/
DECLARE_USB_FUNCTION(
    nd,
    nd_alloc_inst,
    nd_alloc_func);

static int __init ndmod_init(void)
{
    int ret;

    pr_info("nd: registering USB function\n");

    ret = usb_function_register(&ndusb_func);

    pr_info("nd: usb_function_register returned %d\n", ret);

    return ret;
}

static void __exit ndmod_exit(void)
{
    pr_info("nd: unregistering USB function\n");
    usb_function_unregister(&ndusb_func);
}

module_init(ndmod_init);
module_exit(ndmod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Luckfox Near-Data Project");
MODULE_DESCRIPTION("Near-Data USB Gadget Function");