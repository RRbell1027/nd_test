// SPDX-License-Identifier: GPL-2.0
/*
* f_nd.c - Near-Data USB Gadget Function
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/workqueue.h>
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
};

static inline struct f_nd *func_to_nd(struct usb_function *f)
{
    return container_of(f, struct f_nd, function);
}


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
    if (!status)
        nd_requeue_out(nd, out);
    else
        free_ep_req(nd->out_ep, out);
}

static void nd_read_work(struct work_struct *work)
{
    struct f_nd *nd = container_of(work, struct f_nd, read_work);
    struct usb_request *out = nd->pending_out;
    struct usb_request *in;
    struct nd_command cmd;
    struct nd_response *response;
    struct file *sd;
    loff_t offset;
    u64 sector;
    u32 length;
    ssize_t result = -EPROTO;

    in = alloc_ep_req(nd->in_ep, sizeof(*response) + ND_MAX_READ);
    if (!in) {
        nd_requeue_out(nd, out);
        return;
    }
    response = in->buf;
    memset(response, 0, sizeof(*response));
    if (out->actual != sizeof(cmd))
        goto respond;
    memcpy(&cmd, out->buf, sizeof(cmd));
    if (le16_to_cpu(cmd.magic) != ND_PROTOCOL_MAGIC)
        goto respond;
    if (le16_to_cpu(cmd.opcode) != ND_CMD_READ) {
        result = -EOPNOTSUPP;
        goto respond;
    }
    sector = le64_to_cpu(cmd.sector);
    length = le32_to_cpu(cmd.length);
    if (!length || length > ND_MAX_READ ||
        sector > ((u64)S64_MAX - length) / 512) {
        result = -EINVAL;
        goto respond;
    }
    offset = sector * 512;
    sd = filp_open("/dev/mmcblk1", O_RDONLY | O_LARGEFILE, 0);
    if (IS_ERR(sd)) {
        result = PTR_ERR(sd);
        goto respond;
    }
    result = kernel_read(sd, (u8 *)in->buf + sizeof(*response), length, &offset);
    filp_close(sd, NULL);
    pr_info("nd: READ sector=%llu length=%u result=%zd\n",
            (unsigned long long)sector, length, result);
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

    if (req->status || !READ_ONCE(nd->enabled)) {
        free_ep_req(ep, req);
        return;
    }
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
                    sizeof(struct nd_command));
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

    ret = nd_start_out(nd);
    if (ret) {
        WRITE_ONCE(nd->enabled, false);
        usb_ep_disable(nd->in_ep);
        usb_ep_disable(nd->out_ep);
        return ret;
    }

    pr_info("nd: OUT and IN endpoints enabled (response v1)\n");

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
    usb_ep_disable(nd->out_ep);
    usb_ep_disable(nd->in_ep);
    /* driver_data lives until free_func; workers can still finish after reset. */

    pr_info("nd: gadget function disabled\n");
}


static void nd_free_func(struct usb_function *f)
{
    struct f_nd *nd = func_to_nd(f);

    cancel_work_sync(&nd->read_work);
    usb_free_all_descriptors(f);
    kfree(nd);
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