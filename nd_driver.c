#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/slab.h>

#include "nd_ioctl.h"
#include "nd_protocol.h"


/* -------------------------------------------------------------------------- */
/* ND USB device                                                              */
/* -------------------------------------------------------------------------- */

struct nd_dev {
    struct usb_device *udev;
    struct usb_interface *interface;

    unsigned char bulk_out_ep;
};

/*
 * 第一版先只支援一個 ND device。
 *
 * 未來若需要同時支援多個 Luckfox，
 * 就不能再使用這個 global pointer。
 */
static struct nd_dev *nd;


/* -------------------------------------------------------------------------- */
/* Character device operations                                                */
/* -------------------------------------------------------------------------- */

static long nd_ioctl(struct file *file,
                     unsigned int cmd,
                     unsigned long arg)
{
    struct nd_read_request req;
    struct nd_command usb_cmd;

    /*
     * /dev/nd0 理論上只有 USB device 存在時才會出現，
     * 但仍做一次防禦性檢查。
     */
    if (!nd || !nd->udev)
        return -ENODEV;

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
         * Convert userspace request into ND wire protocol.
         *
         * 目前先保留原本行為。
         * 下一步再把 usb_cmd 用 usb_bulk_msg()
         * 送到 Luckfox f_nd。
         */
        usb_cmd.magic  = cpu_to_le16(ND_PROTOCOL_MAGIC);
        usb_cmd.opcode = cpu_to_le16(ND_CMD_READ);
        usb_cmd.length = cpu_to_le32(req.length);
        usb_cmd.sector = cpu_to_le64(req.sector);

        pr_info("nd: command prepared: opcode=%u sector=%llu length=%u\n",
                ND_CMD_READ,
                (unsigned long long)req.sector,
                req.length);

        /*
         * TODO:
         *
         * usb_bulk_msg(
         *     nd->udev,
         *     usb_sndbulkpipe(nd->udev, nd->bulk_out_ep),
         *     &usb_cmd,
         *     sizeof(usb_cmd),
         *     &actual_length,
         *     timeout
         * );
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


/* -------------------------------------------------------------------------- */
/* /dev/nd0                                                                   */
/* -------------------------------------------------------------------------- */

static struct miscdevice nd_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "nd0",
    .fops  = &nd_fops,
};


/* -------------------------------------------------------------------------- */
/* USB probe                                                                  */
/* -------------------------------------------------------------------------- */

static int nd_probe(struct usb_interface *interface,
                    const struct usb_device_id *id)
{
    struct usb_host_interface *iface_desc;
    struct usb_endpoint_descriptor *endpoint;
    struct nd_dev *dev;

    int i;
    int ret;

    pr_info("nd: f_nd interface detected\n");

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    /*
     * Keep a reference to the underlying USB device.
     */
    dev->udev = usb_get_dev(interface_to_usbdev(interface));
    dev->interface = interface;

    iface_desc = interface->cur_altsetting;

    pr_info("nd: interface number=%u endpoints=%u\n",
            iface_desc->desc.bInterfaceNumber,
            iface_desc->desc.bNumEndpoints);

    /*
     * Search for f_nd Bulk OUT endpoint.
     *
     * 目前 f_nd descriptor:
     *
     *     bNumEndpoints = 1
     *     endpoint       = Bulk OUT
     */
    for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {

        endpoint = &iface_desc->endpoint[i].desc;

        pr_info("nd: endpoint[%d] address=0x%02x attributes=0x%02x\n",
                i,
                endpoint->bEndpointAddress,
                endpoint->bmAttributes);

        if (usb_endpoint_is_bulk_out(endpoint)) {

            dev->bulk_out_ep =
                endpoint->bEndpointAddress;

            pr_info("nd: found Bulk OUT endpoint 0x%02x\n",
                    dev->bulk_out_ep);
        }
    }

    /*
     * f_nd 沒有 Bulk OUT 就不是我們預期的 interface。
     */
    if (!dev->bulk_out_ep) {

        pr_err("nd: Bulk OUT endpoint not found\n");

        ret = -ENODEV;
        goto error;
    }

    /*
     * 第一版只允許一個 ND device。
     */
    if (nd) {

        pr_err("nd: another ND device is already connected\n");

        ret = -EBUSY;
        goto error;
    }

    /*
     * Associate this USB interface with our nd_dev.
     *
     * disconnect() 可以再用 usb_get_intfdata()
     * 找回這個 pointer。
     */
    usb_set_intfdata(interface, dev);

    /*
     * 現在才建立 /dev/nd0。
     *
     * 也就是：
     *
     *     f_nd exists
     *         ↓
     *     probe()
     *         ↓
     *     misc_register()
     *         ↓
     *     /dev/nd0
     */
    ret = misc_register(&nd_device);

    if (ret) {

        pr_err("nd: failed to register /dev/nd0: %d\n",
               ret);

        usb_set_intfdata(interface, NULL);

        goto error;
    }

    nd = dev;

    pr_info("nd: registered /dev/nd0\n");

    return 0;


error:

    usb_put_dev(dev->udev);
    kfree(dev);

    return ret;
}


/* -------------------------------------------------------------------------- */
/* USB disconnect                                                             */
/* -------------------------------------------------------------------------- */

static void nd_disconnect(struct usb_interface *interface)
{
    struct nd_dev *dev;

    dev = usb_get_intfdata(interface);

    usb_set_intfdata(interface, NULL);

    /*
     * USB interface 消失時，
     * /dev/nd0 也一起移除。
     */
    if (nd == dev) {

        misc_deregister(&nd_device);

        nd = NULL;

        pr_info("nd: removed /dev/nd0\n");
    }

    if (dev) {

        usb_put_dev(dev->udev);

        kfree(dev);
    }

    pr_info("nd: f_nd interface disconnected\n");
}


/* -------------------------------------------------------------------------- */
/* USB device matching                                                        */
/* -------------------------------------------------------------------------- */

/*
 * 目前 Luckfox f_nd descriptor:
 *
 *     bInterfaceClass    = USB_CLASS_VENDOR_SPEC (0xff)
 *     bInterfaceSubClass = 0
 *     bInterfaceProtocol = 0
 *
 * 因此第一版直接用 interface descriptor match。
 *
 * 注意：
 * 如果未來 composite gadget 裡還有其他
 * class=0xff/subclass=0/protocol=0 的 interface，
 * 這個 match 就太寬了。
 *
 * 之後建議把 f_nd 改成 ND 專屬 subclass/protocol。
 */
static const struct usb_device_id nd_usb_ids[] = {

    {
        USB_INTERFACE_INFO(
            USB_CLASS_VENDOR_SPEC,
            0,
            0
        )
    },

    { }
};

MODULE_DEVICE_TABLE(usb, nd_usb_ids);


/* -------------------------------------------------------------------------- */
/* USB driver                                                                 */
/* -------------------------------------------------------------------------- */

static struct usb_driver nd_usb_driver = {
    .name       = "nd_host",
    .probe      = nd_probe,
    .disconnect = nd_disconnect,
    .id_table   = nd_usb_ids,
};


/* -------------------------------------------------------------------------- */
/* Module init / exit                                                         */
/* -------------------------------------------------------------------------- */

static int __init nd_init(void)
{
    int ret;

    /*
     * 注意：
     *
     * 這裡不再 misc_register(&nd_device)。
     *
     * insmod 只註冊 USB driver。
     * /dev/nd0 要等 f_nd 被 probe 到才建立。
     */
    ret = usb_register(&nd_usb_driver);

    if (ret) {

        pr_err("nd: failed to register USB driver: %d\n",
               ret);

        return ret;
    }

    pr_info("nd: USB driver registered\n");

    return 0;
}


static void __exit nd_exit(void)
{
    /*
     * 如果 f_nd 還存在，
     * usb_deregister() 會觸發 disconnect，
     * disconnect 再負責 misc_deregister()。
     */
    usb_deregister(&nd_usb_driver);

    pr_info("nd: USB driver unloaded\n");
}


module_init(nd_init);
module_exit(nd_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Luckfox Near-Data Project");
MODULE_DESCRIPTION("Near-Data USB host driver");