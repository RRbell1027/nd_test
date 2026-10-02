// SPDX-License-Identifier: GPL-2.0
/*
* f_nd.c - Near-Data USB Gadget Function
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb/composite.h>

#include "u_f.h"
#include "nd_protocol.h"

struct f_nd {
    struct usb_function function;
    struct usb_ep *out_ep;
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
    .bNumEndpoints      = 1,
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

static struct usb_descriptor_header *nd_fs_descs[] = {
    (struct usb_descriptor_header *)&nd_intf_desc,
    (struct usb_descriptor_header *)&nd_fs_out_desc,
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

static struct usb_descriptor_header *nd_hs_descs[] = {
    (struct usb_descriptor_header *)&nd_intf_desc,
    (struct usb_descriptor_header *)&nd_hs_out_desc,
    NULL,
};


/* -------------------------------------------------------------------------- */
/* Receive command                                                            */

static void nd_out_complete(struct usb_ep *ep,
                            struct usb_request *req)
{
    struct f_nd *nd = ep->driver_data;
    struct nd_command cmd;
    int ret;

    if (!nd)
        return;

    switch (req->status) {
    case 0:
        break;

    case -ECONNABORTED:
    case -ECONNRESET:
    case -ESHUTDOWN:
        free_ep_req(ep, req);
        return;

    default:
        pr_err("nd: OUT request failed: %d\n",
            req->status);
        goto requeue;
    }

    if (req->actual != sizeof(cmd)) {
        pr_err("nd: invalid command size: %u\n",
            req->actual);
        goto requeue;
    }

    memcpy(&cmd, req->buf, sizeof(cmd));

    if (le16_to_cpu(cmd.magic) != ND_PROTOCOL_MAGIC) {
        pr_err("nd: invalid magic: 0x%04x\n",
            le16_to_cpu(cmd.magic));
        goto requeue;
    }

    switch (le16_to_cpu(cmd.opcode)) {
    case ND_CMD_READ:
        pr_info("nd: READ sector=%llu length=%u\n",
                (unsigned long long)
                    le64_to_cpu(cmd.sector),
                le32_to_cpu(cmd.length));

        /*
        * Phase 2:
        *
        * Read:
        *     /dev/mmcblk1
        *
        * starting from:
        *     sector
        *
        * for:
        *     length bytes
        *
        * For now we only verify that the USB command
        * reached the Luckfox kernel.
        */
        break;

    default:
        pr_err("nd: unknown opcode: %u\n",
            le16_to_cpu(cmd.opcode));
        break;
    }

requeue:
    ret = usb_ep_queue(ep, req, GFP_ATOMIC);
    if (ret)
        pr_err("nd: failed to requeue OUT request: %d\n",
            ret);
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

    usb_ep_disable(nd->out_ep);

    ret = config_ep_by_speed(cdev->gadget,
                            f,
                            nd->out_ep);
    if (ret)
        return ret;

    ret = usb_ep_enable(nd->out_ep);
    if (ret)
        return ret;

    nd->out_ep->driver_data = nd;

    ret = nd_start_out(nd);
    if (ret) {
        nd->out_ep->driver_data = NULL;
        usb_ep_disable(nd->out_ep);
        return ret;
    }

    pr_info("nd: OUT endpoint enabled\n");

    return 0;
}


static void nd_disable(struct usb_function *f)
{
    struct f_nd *nd = func_to_nd(f);

    if (!nd->out_ep)
        return;

    usb_ep_disable(nd->out_ep);
    nd->out_ep->driver_data = NULL;

    pr_info("nd: gadget function disabled\n");
}


static void nd_free_func(struct usb_function *f)
{
    struct f_nd *nd = func_to_nd(f);

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