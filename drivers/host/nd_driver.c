// SPDX-License-Identifier: GPL-2.0
/* One ND device, one transaction at a time. QEMU sees an unframed byte stream;
 * this driver reconstructs legacy READ commands or complete runtime RPC frames.
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <asm/unaligned.h>
#include "nd_driver.h"
#include "nd_protocol.h"

#define ND_USB_PACKET (sizeof(struct nd_command) + ND_MAX_READ)
struct nd_dev {
    struct usb_device *udev;
    u8 out_ep, in_ep;
    u8 tx[ND_USB_PACKET] __aligned(8);
    u8 rx[ND_USB_PACKET] __aligned(8);
    u8 header[ND_RPC_HEADER_SIZE];
    size_t header_used, header_need;
    u8 *frame, *response;
    size_t frame_used, frame_size;
    size_t response_size, response_offset, response_capacity;
};
static struct nd_dev *nd;
static DEFINE_MUTEX(nd_lock);
static DECLARE_WAIT_QUEUE_HEAD(nd_read_wait);

static void nd_clear_request(struct nd_dev *d)
{
    kvfree(d->frame);
    d->frame = NULL;
    d->frame_used = d->frame_size = d->header_used = 0;
    d->header_need = 4;
}

/* OUT RPC packets have a fixed transfer size, so even exact 512-byte payload
 * boundaries complete the gadget's receive request without depending on a ZLP.
 */
static int nd_exchange(struct nd_dev *d, u16 op, u64 sector,
                       const void *data, u32 length)
{
    struct nd_command *cmd = (struct nd_command *)d->tx;
    struct nd_response *r = (struct nd_response *)d->rx;
    u32 payload;
    s32 status;
    int ret, actual, out_size;

    memset(d->tx, 0, sizeof(d->tx));
    cmd->magic = cpu_to_le16(ND_PROTOCOL_MAGIC);
    cmd->opcode = cpu_to_le16(op);
    cmd->length = cpu_to_le32(length);
    cmd->sector = cpu_to_le64(sector);
    out_size = sizeof(*cmd);
    if (op == ND_CMD_RPC_OUT) {
        memcpy(d->tx + sizeof(*cmd), data, length);
        out_size = sizeof(d->tx);
    }
    ret = usb_bulk_msg(d->udev, usb_sndbulkpipe(d->udev, d->out_ep),
                       d->tx, out_size, &actual, 5000);
    if (ret)
        return ret;
    if (actual != out_size)
        return -EIO;
    ret = usb_bulk_msg(d->udev, usb_rcvbulkpipe(d->udev, d->in_ep),
                       d->rx, sizeof(d->rx), &actual, 30000);
    if (ret)
        return ret;
    if (actual < sizeof(*r))
        return -EPROTO;
    payload = le32_to_cpu(r->length);
    status = (s32)le32_to_cpu(r->status);
    if (le16_to_cpu(r->magic) != ND_PROTOCOL_MAGIC ||
        le16_to_cpu(r->version) != ND_RESPONSE_VERSION || r->reserved ||
        payload > ND_MAX_READ || actual != sizeof(*r) + payload ||
        status > 0 || status < -4095 || (status && payload))
        return -EPROTO;
    if (status)
        return status;
    if ((op == ND_CMD_RPC_OUT || op == ND_CMD_RPC_RESET) && payload)
        return -EPROTO;
    if ((op == ND_CMD_READ || op == ND_CMD_RPC_IN) && payload > length)
        return -EPROTO;
    return payload;
}

static int nd_response_space(struct nd_dev *d, size_t size)
{
    u8 *p;
    if (size <= d->response_capacity)
        return 0;
    p = kvmalloc(size, GFP_KERNEL);
    if (!p)
        return -ENOMEM;
    kvfree(d->response);
    d->response = p;
    d->response_capacity = size;
    return 0;
}

static int nd_rpc_receive(struct nd_dev *d, u8 *buf, size_t length)
{
    int n;
    while (length) {
        n = nd_exchange(d, ND_CMD_RPC_IN, 0, NULL,
                         min_t(size_t, length, ND_MAX_READ));
        if (n <= 0)
            return n ? n : -EPROTO;
        memcpy(buf, d->rx + sizeof(struct nd_response), n);
        buf += n;
        length -= n;
    }
    return 0;
}

static int nd_runtime_transaction(struct nd_dev *d)
{
    u8 header[ND_RPC_HEADER_SIZE];
    size_t offset = 0, bytes;
    u32 payload;
    int ret;

    /* HELLO begins a new executor session. Invalidates stale board fds and
     * drains any interrupted transaction before a fresh runtime connects.
     */
    if (get_unaligned_le32(d->frame + 4) == ND_RPC_HELLO) {
        ret = nd_exchange(d, ND_CMD_RPC_RESET, 0, NULL, 0);
        if (ret < 0)
            return ret;
    }
    while (offset < d->frame_size) {
        bytes = min_t(size_t, d->frame_size - offset, ND_MAX_READ);
        ret = nd_exchange(d, ND_CMD_RPC_OUT, 0, d->frame + offset, bytes);
        if (ret < 0)
            return ret;
        offset += bytes;
    }
    ret = nd_rpc_receive(d, header, sizeof(header));
    if (ret)
        return ret;
    payload = get_unaligned_le32(header + 16);
    if (memcmp(header, d->frame, 12) || get_unaligned_le32(header + 20) ||
        payload > ND_RPC_MAX_BODY)
        return -EPROTO;
    ret = nd_response_space(d, sizeof(header) + payload);
    if (ret)
        return ret;
    memcpy(d->response, header, sizeof(header));
    ret = nd_rpc_receive(d, d->response + sizeof(header), payload);
    if (ret)
        return ret;
    d->response_size = sizeof(header) + payload;
    d->response_offset = 0;
    return 0;
}

static int nd_legacy_transaction(struct nd_dev *d)
{
    struct nd_driver_command cmd;
    int n, ret;
    memcpy(&cmd, d->header, sizeof(cmd));
    if (cmd.magic != ND_DRIVER_MAGIC || cmd.command != ND_DRIVER_CMD_READ ||
        !cmd.length || cmd.length > ND_MAX_READ)
        return -EINVAL;
    n = nd_exchange(d, ND_CMD_READ, cmd.sector, NULL, cmd.length);
    if (n < 0)
        return n;
    ret = nd_response_space(d, sizeof(struct nd_response) + n);
    if (ret)
        return ret;
    d->response_size = sizeof(struct nd_response) + n;
    d->response_offset = 0;
    memcpy(d->response, d->rx, d->response_size);
    return 0;
}

static ssize_t nd_write(struct file *f, const char __user *buf,
                         size_t count, loff_t *pos)
{
    size_t used = 0, chunk;
    u32 magic, payload;
    int ret = 0;
    mutex_lock(&nd_lock);
    if (!nd) { ret = -ENODEV; goto out; }
    if (nd->response_size != nd->response_offset) { ret = -EBUSY; goto out; }
    if (!nd->in_ep) { ret = -EOPNOTSUPP; goto out; }
    while (used < count) {
        if (!nd->frame) {
            chunk = min(count - used, nd->header_need - nd->header_used);
            if (copy_from_user(nd->header + nd->header_used, buf + used, chunk)) {
                ret = -EFAULT; goto reset;
            }
            used += chunk;
            nd->header_used += chunk;
            if (nd->header_used != nd->header_need)
                continue;
            if (nd->header_need == 4) {
                magic = get_unaligned_le32(nd->header);
                if (magic == ND_RPC_MAGIC)
                    nd->header_need = ND_RPC_HEADER_SIZE;
                else if ((magic & 0xffff) == ND_DRIVER_MAGIC)
                    nd->header_need = sizeof(struct nd_driver_command);
                else { ret = -EPROTO; goto reset; }
                continue;
            }
            if (nd->header_need == sizeof(struct nd_driver_command)) {
                ret = nd_legacy_transaction(nd);
                goto complete;
            }
            payload = get_unaligned_le32(nd->header + 16);
            if (payload > ND_RPC_MAX_BODY || get_unaligned_le32(nd->header + 12) ||
                get_unaligned_le32(nd->header + 20)) {
                ret = -EPROTO; goto reset;
            }
            nd->frame_size = ND_RPC_HEADER_SIZE + payload;
            nd->frame = kvmalloc(nd->frame_size, GFP_KERNEL);
            if (!nd->frame) { ret = -ENOMEM; goto reset; }
            memcpy(nd->frame, nd->header, ND_RPC_HEADER_SIZE);
            nd->frame_used = ND_RPC_HEADER_SIZE;
        }
        chunk = min(count - used, nd->frame_size - nd->frame_used);
        if (chunk && copy_from_user(nd->frame + nd->frame_used, buf + used, chunk)) {
            ret = -EFAULT; goto reset;
        }
        used += chunk;
        nd->frame_used += chunk;
        if (nd->frame_used != nd->frame_size)
            continue;
        ret = nd_runtime_transaction(nd);
complete:
        nd_clear_request(nd);
        if (!ret)
            wake_up_interruptible(&nd_read_wait);
        break;
reset:
        nd_clear_request(nd);
        break;
    }
out:
    mutex_unlock(&nd_lock);
    return ret ? ret : used;
}

static ssize_t nd_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    ssize_t ret;
    if (!count)
        return 0;
    mutex_lock(&nd_lock);
    if (!nd) { ret = -ENODEV; goto out; }
    count = min(count, nd->response_size - nd->response_offset);
    if (!count) { ret = -EAGAIN; goto out; }
    if (copy_to_user(buf, nd->response + nd->response_offset, count)) {
        ret = -EFAULT; goto out;
    }
    nd->response_offset += count;
    ret = count;
out:
    mutex_unlock(&nd_lock);
    return ret;
}
static __poll_t nd_poll(struct file *f, poll_table *wait)
{
    __poll_t mask;
    poll_wait(f, &nd_read_wait, wait);
    mutex_lock(&nd_lock);
    if (!nd)
        mask = EPOLLERR | EPOLLHUP;
    else if (nd->response_size > nd->response_offset)
        mask = EPOLLIN | EPOLLRDNORM;
    else
        mask = EPOLLOUT | EPOLLWRNORM;
    mutex_unlock(&nd_lock);
    return mask;
}
static const struct file_operations nd_fops = {
    .owner = THIS_MODULE, .read = nd_read, .write = nd_write, .poll = nd_poll,
    .llseek = no_llseek,
};
static struct miscdevice nd_misc = {
    .minor = MISC_DYNAMIC_MINOR, .name = "nd0", .fops = &nd_fops,
};
static int nd_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
    struct nd_dev *d;
    struct usb_host_interface *alt = intf->cur_altsetting;
    int i, ret;
    mutex_lock(&nd_lock);
    if (nd) { ret = -EBUSY; goto out; }
    d = kzalloc(sizeof(*d), GFP_KERNEL);
    if (!d) { ret = -ENOMEM; goto out; }
    d->udev = usb_get_dev(interface_to_usbdev(intf));
    d->header_need = 4;
    for (i = 0; i < alt->desc.bNumEndpoints; ++i) {
        struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;
        if (usb_endpoint_is_bulk_in(ep)) d->in_ep = ep->bEndpointAddress;
        if (usb_endpoint_is_bulk_out(ep)) d->out_ep = ep->bEndpointAddress;
    }
    if (!d->out_ep) { ret = -ENODEV; goto free; }
    ret = misc_register(&nd_misc);
    if (ret) goto free;
    nd = d;
    usb_set_intfdata(intf, d);
    dev_info(&intf->dev, "ND READ and runtime stream: /dev/nd0\n");
    goto out;
free:
    usb_put_dev(d->udev);
    kfree(d);
out:
    mutex_unlock(&nd_lock);
    return ret;
}
static void nd_disconnect(struct usb_interface *intf)
{
    struct nd_dev *d;
    mutex_lock(&nd_lock);
    d = usb_get_intfdata(intf);
    usb_set_intfdata(intf, NULL);
    if (nd == d) {
        misc_deregister(&nd_misc);
        nd = NULL;
    }
    if (d) {
        nd_clear_request(d);
        kvfree(d->response);
        usb_put_dev(d->udev);
        kfree(d);
    }
    mutex_unlock(&nd_lock);
    wake_up_interruptible(&nd_read_wait);
}
static const struct usb_device_id nd_ids[] = {
    { USB_DEVICE_AND_INTERFACE_INFO(0x2207, 0x0000, USB_CLASS_VENDOR_SPEC, 0, 0) },
    { }
};
MODULE_DEVICE_TABLE(usb, nd_ids);
static struct usb_driver nd_usb = {
    .name = "nd_host", .probe = nd_probe, .disconnect = nd_disconnect,
    .id_table = nd_ids,
};
module_usb_driver(nd_usb);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ND USB READ and runtime byte-stream host transport");
