/*
 * USB hub driver.
 *
 * (C) Copyright 1999 Linus Torvalds
 * (C) Copyright 1999 Johannes Erdfelt
 * (C) Copyright 1999 Gregory P. Smith
 * (C) Copyright 2001 Brad Hards (bhards@bigpond.net.au)
 *
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/completion.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/ioctl.h>
#include <linux/usb.h>
#include <linux/usbdevice_fs.h>
#include <linux/usb/hcd.h>
#include <linux/usb/otg.h>
#include <linux/usb/quirks.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/freezer.h>
#include <linux/random.h>
#include <linux/pm_qos.h>

#include <asm/uaccess.h>
#include <asm/byteorder.h>

#include "hub.h"

#ifdef DEBUG
#ifndef CONFIG_USB_ANNOUNCE_NEW_DEVICES
#define CONFIG_USB_ANNOUNCE_NEW_DEVICES
#endif
#endif

#define USB_VENDOR_GENESYS_LOGIC		0x05e3
#define HUB_QUIRK_CHECK_PORT_AUTOSUSPEND	0x01

static inline int hub_is_superspeed(struct usb_device *hdev)
{
	return (hdev->descriptor.bDeviceProtocol == USB_HUB_PR_SS);
}

static DEFINE_SPINLOCK(device_state_lock);

static DEFINE_SPINLOCK(hub_event_lock);
static LIST_HEAD(hub_event_list);	

static DECLARE_WAIT_QUEUE_HEAD(khubd_wait);

static struct task_struct *khubd_task;

static bool blinkenlights = 0;
module_param (blinkenlights, bool, S_IRUGO);
MODULE_PARM_DESC (blinkenlights, "true to cycle leds on hubs");

static int initial_descriptor_timeout = USB_CTRL_GET_TIMEOUT;
module_param(initial_descriptor_timeout, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(initial_descriptor_timeout,
		"initial 64-byte descriptor request timeout in milliseconds "
		"(default 5000 - 5.0 seconds)");

static bool old_scheme_first = 0;
module_param(old_scheme_first, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(old_scheme_first,
		 "start with the old device initialization scheme");

static bool use_both_schemes = 1;
module_param(use_both_schemes, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(use_both_schemes,
		"try the other device initialization scheme if the "
		"first one fails");

DECLARE_RWSEM(ehci_cf_port_reset_rwsem);
EXPORT_SYMBOL_GPL(ehci_cf_port_reset_rwsem);

#define HUB_DEBOUNCE_TIMEOUT	2000
#define HUB_DEBOUNCE_STEP	  25
#define HUB_DEBOUNCE_STABLE	 100

static int usb_reset_and_verify_device(struct usb_device *udev);

static inline char *portspeed(struct usb_hub *hub, int portstatus)
{
	if (hub_is_superspeed(hub->hdev))
		return "5.0 Gb/s";
	if (portstatus & USB_PORT_STAT_HIGH_SPEED)
    		return "480 Mb/s";
	else if (portstatus & USB_PORT_STAT_LOW_SPEED)
		return "1.5 Mb/s";
	else
		return "12 Mb/s";
}

struct usb_hub *usb_hub_to_struct_hub(struct usb_device *hdev)
{
	if (!hdev || !hdev->actconfig || !hdev->maxchild)
		return NULL;
	return usb_get_intfdata(hdev->actconfig->interface[0]);
}

static int usb_device_supports_lpm(struct usb_device *udev)
{
	if (udev->speed == USB_SPEED_HIGH) {
		if (udev->bos->ext_cap &&
			(USB_LPM_SUPPORT &
			 le32_to_cpu(udev->bos->ext_cap->bmAttributes)))
			return 1;
		return 0;
	}

	if (!udev->bos->ss_cap) {
		dev_warn(&udev->dev, "No LPM exit latency info found.  "
				"Power management will be impacted.\n");
		return 0;
	}
	if (udev->parent->lpm_capable)
		return 1;

	dev_warn(&udev->dev, "Parent hub missing LPM exit latency info.  "
			"Power management will be impacted.\n");
	return 0;
}

static void usb_set_lpm_mel(struct usb_device *udev,
		struct usb3_lpm_parameters *udev_lpm_params,
		unsigned int udev_exit_latency,
		struct usb_hub *hub,
		struct usb3_lpm_parameters *hub_lpm_params,
		unsigned int hub_exit_latency)
{
	unsigned int total_mel;
	unsigned int device_mel;
	unsigned int hub_mel;

	total_mel = hub_lpm_params->mel +
		(hub->descriptor->u.ss.bHubHdrDecLat * 100);

	device_mel = udev_exit_latency * 1000;
	hub_mel = hub_exit_latency * 1000;
	if (device_mel > hub_mel)
		total_mel += device_mel;
	else
		total_mel += hub_mel;

	udev_lpm_params->mel = total_mel;
}

static void usb_set_lpm_pel(struct usb_device *udev,
		struct usb3_lpm_parameters *udev_lpm_params,
		unsigned int udev_exit_latency,
		struct usb_hub *hub,
		struct usb3_lpm_parameters *hub_lpm_params,
		unsigned int hub_exit_latency,
		unsigned int port_to_port_exit_latency)
{
	unsigned int first_link_pel;
	unsigned int hub_pel;

	if (udev_exit_latency > hub_exit_latency)
		first_link_pel = udev_exit_latency * 1000;
	else
		first_link_pel = hub_exit_latency * 1000;

	hub_pel = port_to_port_exit_latency * 1000 + hub_lpm_params->pel;

	if (first_link_pel > hub_pel)
		udev_lpm_params->pel = first_link_pel;
	else
		udev_lpm_params->pel = hub_pel;
}

static void usb_set_lpm_sel(struct usb_device *udev,
		struct usb3_lpm_parameters *udev_lpm_params)
{
	struct usb_device *parent;
	unsigned int num_hubs;
	unsigned int total_sel;

	/* t1 = device PEL */
	total_sel = udev_lpm_params->pel;
	/* How many external hubs are in between the device & the root port. */
	for (parent = udev->parent, num_hubs = 0; parent->parent;
			parent = parent->parent)
		num_hubs++;
	/* t2 = 2.1us + 250ns * (num_hubs - 1) */
	if (num_hubs > 0)
		total_sel += 2100 + 250 * (num_hubs - 1);

	/* t4 = 250ns * num_hubs */
	total_sel += 250 * num_hubs;

	udev_lpm_params->sel = total_sel;
}

static void usb_set_lpm_parameters(struct usb_device *udev)
{
	struct usb_hub *hub;
	unsigned int port_to_port_delay;
	unsigned int udev_u1_del;
	unsigned int udev_u2_del;
	unsigned int hub_u1_del;
	unsigned int hub_u2_del;

	if (!udev->lpm_capable || udev->speed != USB_SPEED_SUPER)
		return;

	hub = usb_hub_to_struct_hub(udev->parent);
	if (!hub)
		return;

	udev_u1_del = udev->bos->ss_cap->bU1devExitLat;
	udev_u2_del = udev->bos->ss_cap->bU2DevExitLat;
	hub_u1_del = udev->parent->bos->ss_cap->bU1devExitLat;
	hub_u2_del = udev->parent->bos->ss_cap->bU2DevExitLat;

	usb_set_lpm_mel(udev, &udev->u1_params, udev_u1_del,
			hub, &udev->parent->u1_params, hub_u1_del);

	usb_set_lpm_mel(udev, &udev->u2_params, udev_u2_del,
			hub, &udev->parent->u2_params, hub_u2_del);

	port_to_port_delay = 1;

	usb_set_lpm_pel(udev, &udev->u1_params, udev_u1_del,
			hub, &udev->parent->u1_params, hub_u1_del,
			port_to_port_delay);

	if (hub_u2_del > hub_u1_del)
		port_to_port_delay = 1 + hub_u2_del - hub_u1_del;
	else
		port_to_port_delay = 1 + hub_u1_del;

	usb_set_lpm_pel(udev, &udev->u2_params, udev_u2_del,
			hub, &udev->parent->u2_params, hub_u2_del,
			port_to_port_delay);

	
	usb_set_lpm_sel(udev, &udev->u1_params);
	usb_set_lpm_sel(udev, &udev->u2_params);
}

static int get_hub_descriptor(struct usb_device *hdev, void *data)
{
	int i, ret, size;
	unsigned dtype;

	if (hub_is_superspeed(hdev)) {
		dtype = USB_DT_SS_HUB;
		size = USB_DT_SS_HUB_SIZE;
	} else {
		dtype = USB_DT_HUB;
		size = sizeof(struct usb_hub_descriptor);
	}

	for (i = 0; i < 3; i++) {
		ret = usb_control_msg(hdev, usb_rcvctrlpipe(hdev, 0),
			USB_REQ_GET_DESCRIPTOR, USB_DIR_IN | USB_RT_HUB,
			dtype << 8, 0, data, size,
			USB_CTRL_GET_TIMEOUT);
		if (ret >= (USB_DT_HUB_NONVAR_SIZE + 2))
			return ret;
	}
	return -EINVAL;
}

static int clear_hub_feature(struct usb_device *hdev, int feature)
{
	return usb_control_msg(hdev, usb_sndctrlpipe(hdev, 0),
		USB_REQ_CLEAR_FEATURE, USB_RT_HUB, feature, 0, NULL, 0, 1000);
}

int usb_clear_port_feature(struct usb_device *hdev, int port1, int feature)
{
	return usb_control_msg(hdev, usb_sndctrlpipe(hdev, 0),
		USB_REQ_CLEAR_FEATURE, USB_RT_PORT, feature, port1,
		NULL, 0, 1000);
}

static int set_port_feature(struct usb_device *hdev, int port1, int feature)
{
	return usb_control_msg(hdev, usb_sndctrlpipe(hdev, 0),
		USB_REQ_SET_FEATURE, USB_RT_PORT, feature, port1,
		NULL, 0, 1000);
}

static void set_port_led(
	struct usb_hub *hub,
	int port1,
	int selector
)
{
	int status = set_port_feature(hub->hdev, (selector << 8) | port1,
			USB_PORT_FEAT_INDICATOR);
	if (status < 0)
		dev_dbg (hub->intfdev,
			"port %d indicator %s status %d\n",
			port1,
			({ char *s; switch (selector) {
			case HUB_LED_AMBER: s = "amber"; break;
			case HUB_LED_GREEN: s = "green"; break;
			case HUB_LED_OFF: s = "off"; break;
			case HUB_LED_AUTO: s = "auto"; break;
			default: s = "??"; break;
			}; s; }),
			status);
}

#define	LED_CYCLE_PERIOD	((2*HZ)/3)

static void led_work (struct work_struct *work)
{
	struct usb_hub		*hub =
		container_of(work, struct usb_hub, leds.work);
	struct usb_device	*hdev = hub->hdev;
	unsigned		i;
	unsigned		changed = 0;
	int			cursor = -1;

	if (hdev->state != USB_STATE_CONFIGURED || hub->quiescing)
		return;

	for (i = 0; i < hub->descriptor->bNbrPorts; i++) {
		unsigned	selector, mode;

		/* 30%-50% duty cycle */

		switch (hub->indicator[i]) {
		/* cycle marker */
		case INDICATOR_CYCLE:
			cursor = i;
			selector = HUB_LED_AUTO;
			mode = INDICATOR_AUTO;
			break;
		/* blinking green = sw attention */
		case INDICATOR_GREEN_BLINK:
			selector = HUB_LED_GREEN;
			mode = INDICATOR_GREEN_BLINK_OFF;
			break;
		case INDICATOR_GREEN_BLINK_OFF:
			selector = HUB_LED_OFF;
			mode = INDICATOR_GREEN_BLINK;
			break;
		/* blinking amber = hw attention */
		case INDICATOR_AMBER_BLINK:
			selector = HUB_LED_AMBER;
			mode = INDICATOR_AMBER_BLINK_OFF;
			break;
		case INDICATOR_AMBER_BLINK_OFF:
			selector = HUB_LED_OFF;
			mode = INDICATOR_AMBER_BLINK;
			break;
		/* blink green/amber = reserved */
		case INDICATOR_ALT_BLINK:
			selector = HUB_LED_GREEN;
			mode = INDICATOR_ALT_BLINK_OFF;
			break;
		case INDICATOR_ALT_BLINK_OFF:
			selector = HUB_LED_AMBER;
			mode = INDICATOR_ALT_BLINK;
			break;
		default:
			continue;
		}
		if (selector != HUB_LED_AUTO)
			changed = 1;
		set_port_led(hub, i + 1, selector);
		hub->indicator[i] = mode;
	}
	if (!changed && blinkenlights) {
		cursor++;
		cursor %= hub->descriptor->bNbrPorts;
		set_port_led(hub, cursor + 1, HUB_LED_GREEN);
		hub->indicator[cursor] = INDICATOR_CYCLE;
		changed++;
	}
	if (changed)
		schedule_delayed_work(&hub->leds, LED_CYCLE_PERIOD);
}

#define	USB_STS_TIMEOUT		1000
#define	USB_STS_RETRIES		5

static int get_hub_status(struct usb_device *hdev,
		struct usb_hub_status *data)
{
	int i, status = -ETIMEDOUT;

	for (i = 0; i < USB_STS_RETRIES &&
			(status == -ETIMEDOUT || status == -EPIPE); i++) {
		status = usb_control_msg(hdev, usb_rcvctrlpipe(hdev, 0),
			USB_REQ_GET_STATUS, USB_DIR_IN | USB_RT_HUB, 0, 0,
			data, sizeof(*data), USB_STS_TIMEOUT);
	}
	return status;
}

static int get_port_status(struct usb_device *hdev, int port1,
		struct usb_port_status *data)
{
	int i, status = -ETIMEDOUT;

	for (i = 0; i < USB_STS_RETRIES &&
			(status == -ETIMEDOUT || status == -EPIPE); i++) {
		status = usb_control_msg(hdev, usb_rcvctrlpipe(hdev, 0),
			USB_REQ_GET_STATUS, USB_DIR_IN | USB_RT_PORT, 0, port1,
			data, sizeof(*data), USB_STS_TIMEOUT);
	}
	return status;
}

static int hub_port_status(struct usb_hub *hub, int port1,
		u16 *status, u16 *change)
{
	int ret;

	mutex_lock(&hub->status_mutex);
	ret = get_port_status(hub->hdev, port1, &hub->status->port);
	if (ret < 4) {
		if (ret != -ENODEV)
			dev_err(hub->intfdev,
				"%s failed (err = %d)\n", __func__, ret);
		if (ret >= 0)
			ret = -EIO;
	} else {
		*status = le16_to_cpu(hub->status->port.wPortStatus);
		*change = le16_to_cpu(hub->status->port.wPortChange);

		ret = 0;
	}
	mutex_unlock(&hub->status_mutex);
	return ret;
}

static void kick_khubd(struct usb_hub *hub)
{
	unsigned long	flags;

	spin_lock_irqsave(&hub_event_lock, flags);
	if (!hub->disconnected && list_empty(&hub->event_list)) {
		list_add_tail(&hub->event_list, &hub_event_list);

		/* Suppress autosuspend until khubd runs */
		usb_autopm_get_interface_no_resume(
				to_usb_interface(hub->intfdev));
		wake_up(&khubd_wait);
	}
	spin_unlock_irqrestore(&hub_event_lock, flags);
}

void usb_kick_khubd(struct usb_device *hdev)
{
	struct usb_hub *hub = usb_hub_to_struct_hub(hdev);

	if (hub)
		kick_khubd(hub);
}

void usb_wakeup_notification(struct usb_device *hdev,
		unsigned int portnum)
{
	struct usb_hub *hub;

	if (!hdev)
		return;

	hub = usb_hub_to_struct_hub(hdev);
	if (hub) {
		set_bit(portnum, hub->wakeup_bits);
		kick_khubd(hub);
	}
}
EXPORT_SYMBOL_GPL(usb_wakeup_notification);

static void hub_irq(struct urb *urb)
{
	struct usb_hub *hub = urb->context;
	int status = urb->status;
	unsigned i;
	unsigned long bits;

	switch (status) {
	case -ENOENT:		/* synchronous unlink */
	case -ECONNRESET:	/* async unlink */
	case -ESHUTDOWN:	/* hardware going away */
		return;

	default:		/* presumably an error */
		/* Cause a hub reset after 10 consecutive errors */
		dev_dbg (hub->intfdev, "transfer --> %d\n", status);
		if ((++hub->nerrors < 10) || hub->error)
			goto resubmit;
		hub->error = status;
		/* FALL THROUGH */

	/* let khubd handle things */
	case 0:			/* we got data:  port status changed */
		bits = 0;
		for (i = 0; i < urb->actual_length; ++i)
			bits |= ((unsigned long) ((*hub->buffer)[i]))
					<< (i*8);
		hub->event_bits[0] = bits;
		break;
	}

	hub->nerrors = 0;

	/* Something happened, let khubd figure it out */
	kick_khubd(hub);

resubmit:
	if (hub->quiescing)
		return;

	if ((status = usb_submit_urb (hub->urb, GFP_ATOMIC)) != 0
			&& status != -ENODEV && status != -EPERM)
		dev_err (hub->intfdev, "resubmit --> %d\n", status);
}

static inline int
hub_clear_tt_buffer (struct usb_device *hdev, u16 devinfo, u16 tt)
{
	
	if (((devinfo >> 11) & USB_ENDPOINT_XFERTYPE_MASK) ==
			USB_ENDPOINT_XFER_CONTROL) {
		int status = usb_control_msg(hdev, usb_sndctrlpipe(hdev, 0),
				HUB_CLEAR_TT_BUFFER, USB_RT_PORT,
				devinfo ^ 0x8000, tt, NULL, 0, 1000);
		if (status)
			return status;
	}
	return usb_control_msg(hdev, usb_sndctrlpipe(hdev, 0),
			       HUB_CLEAR_TT_BUFFER, USB_RT_PORT, devinfo,
			       tt, NULL, 0, 1000);
}

static void hub_tt_work(struct work_struct *work)
{
	struct usb_hub		*hub =
		container_of(work, struct usb_hub, tt.clear_work);
	unsigned long		flags;

	spin_lock_irqsave (&hub->tt.lock, flags);
	while (!list_empty(&hub->tt.clear_list)) {
		struct list_head	*next;
		struct usb_tt_clear	*clear;
		struct usb_device	*hdev = hub->hdev;
		const struct hc_driver	*drv;
		int			status;

		next = hub->tt.clear_list.next;
		clear = list_entry (next, struct usb_tt_clear, clear_list);
		list_del (&clear->clear_list);

		/* drop lock so HCD can concurrently report other TT errors */
		spin_unlock_irqrestore (&hub->tt.lock, flags);
		status = hub_clear_tt_buffer (hdev, clear->devinfo, clear->tt);
		if (status && status != -ENODEV)
			dev_err (&hdev->dev,
				"clear tt %d (%04x) error %d\n",
				clear->tt, clear->devinfo, status);

		/* Tell the HCD, even if the operation failed */
		drv = clear->hcd->driver;
		if (drv->clear_tt_buffer_complete)
			(drv->clear_tt_buffer_complete)(clear->hcd, clear->ep);

		kfree(clear);
		spin_lock_irqsave(&hub->tt.lock, flags);
	}
	spin_unlock_irqrestore (&hub->tt.lock, flags);
}

int usb_hub_set_port_power(struct usb_device *hdev, struct usb_hub *hub,
			   int port1, bool set)
{
	int ret;
	struct usb_port *port_dev = hub->ports[port1 - 1];

	if (set)
		ret = set_port_feature(hdev, port1, USB_PORT_FEAT_POWER);
	else
		ret = usb_clear_port_feature(hdev, port1, USB_PORT_FEAT_POWER);

	if (!ret)
		port_dev->power_is_on = set;
	return ret;
}

int usb_hub_clear_tt_buffer(struct urb *urb)
{
	struct usb_device	*udev = urb->dev;
	int			pipe = urb->pipe;
	struct usb_tt		*tt = udev->tt;
	unsigned long		flags;
	struct usb_tt_clear	*clear;

	if ((clear = kmalloc (sizeof *clear, GFP_ATOMIC)) == NULL) {
		dev_err (&udev->dev, "can't save CLEAR_TT_BUFFER state\n");
		
		return -ENOMEM;
	}

	
	clear->tt = tt->multi ? udev->ttport : 1;
	clear->devinfo = usb_pipeendpoint (pipe);
	clear->devinfo |= udev->devnum << 4;
	clear->devinfo |= usb_pipecontrol (pipe)
			? (USB_ENDPOINT_XFER_CONTROL << 11)
			: (USB_ENDPOINT_XFER_BULK << 11);
	if (usb_pipein (pipe))
		clear->devinfo |= 1 << 15;

	/* info for completion callback */
	clear->hcd = bus_to_hcd(udev->bus);
	clear->ep = urb->ep;

	/* tell keventd to clear state for this TT */
	spin_lock_irqsave (&tt->lock, flags);
	list_add_tail (&clear->clear_list, &tt->clear_list);
	schedule_work(&tt->clear_work);
	spin_unlock_irqrestore (&tt->lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(usb_hub_clear_tt_buffer);

static unsigned hub_power_on(struct usb_hub *hub, bool do_delay)
{
	int port1;
	unsigned pgood_delay = hub->descriptor->bPwrOn2PwrGood * 2;
	unsigned delay;
	u16 wHubCharacteristics =
			le16_to_cpu(hub->descriptor->wHubCharacteristics);

	if ((wHubCharacteristics & HUB_CHAR_LPSM) < 2)
		dev_dbg(hub->intfdev, "enabling power on all ports\n");
	else
		dev_dbg(hub->intfdev, "trying to enable port power on "
				"non-switchable hub\n");
	for (port1 = 1; port1 <= hub->descriptor->bNbrPorts; port1++)
		if (hub->ports[port1 - 1]->power_is_on)
			set_port_feature(hub->hdev, port1, USB_PORT_FEAT_POWER);
		else
			usb_clear_port_feature(hub->hdev, port1,
						USB_PORT_FEAT_POWER);

	/* Wait at least 100 msec for power to become stable */
	delay = max(pgood_delay, (unsigned) 100);
	if (do_delay)
		msleep(delay);
	return delay;
}

static int hub_hub_status(struct usb_hub *hub,
		u16 *status, u16 *change)
{
	int ret;

	mutex_lock(&hub->status_mutex);
	ret = get_hub_status(hub->hdev, &hub->status->hub);
	if (ret < 0) {
		if (ret != -ENODEV)
			dev_err(hub->intfdev,
				"%s failed (err = %d)\n", __func__, ret);
	} else {
		*status = le16_to_cpu(hub->status->hub.wHubStatus);
		*change = le16_to_cpu(hub->status->hub.wHubChange); 
		ret = 0;
	}
	mutex_unlock(&hub->status_mutex);
	return ret;
}

static int hub_set_port_link_state(struct usb_hub *hub, int port1,
			unsigned int link_status)
{
	return set_port_feature(hub->hdev,
			port1 | (link_status << 3),
			USB_PORT_FEAT_LINK_STATE);
}

static int hub_usb3_port_disable(struct usb_hub *hub, int port1)
{
	int ret;
	int total_time;
	u16 portchange, portstatus;

	if (!hub_is_superspeed(hub->hdev))
		return -EINVAL;

	ret = hub_set_port_link_state(hub, port1, USB_SS_PORT_LS_SS_DISABLED);
	if (ret)
		return ret;

	/* Wait for the link to enter the disabled state. */
	for (total_time = 0; ; total_time += HUB_DEBOUNCE_STEP) {
		ret = hub_port_status(hub, port1, &portstatus, &portchange);
		if (ret < 0)
			return ret;

		if ((portstatus & USB_PORT_STAT_LINK_STATE) ==
				USB_SS_PORT_LS_SS_DISABLED)
			break;
		if (total_time >= HUB_DEBOUNCE_TIMEOUT)
			break;
		msleep(HUB_DEBOUNCE_STEP);
	}
	if (total_time >= HUB_DEBOUNCE_TIMEOUT)
		dev_warn(hub->intfdev, "Could not disable port %d after %d ms\n",
				port1, total_time);

	return hub_set_port_link_state(hub, port1, USB_SS_PORT_LS_RX_DETECT);
}

static int hub_port_disable(struct usb_hub *hub, int port1, int set_state)
{
	struct usb_device *hdev = hub->hdev;
	int ret = 0;

	if (hub->ports[port1 - 1]->child && set_state)
		usb_set_device_state(hub->ports[port1 - 1]->child,
				USB_STATE_NOTATTACHED);
	if (!hub->error) {
		if (hub_is_superspeed(hub->hdev))
			ret = hub_usb3_port_disable(hub, port1);
		else
			ret = usb_clear_port_feature(hdev, port1,
					USB_PORT_FEAT_ENABLE);
	}
	if (ret && ret != -ENODEV)
		dev_err(hub->intfdev, "cannot disable port %d (err = %d)\n",
				port1, ret);
	return ret;
}

static void hub_port_logical_disconnect(struct usb_hub *hub, int port1)
{
	dev_dbg(hub->intfdev, "logical disconnect on port %d\n", port1);
	hub_port_disable(hub, port1, 1);


	set_bit(port1, hub->change_bits);
 	kick_khubd(hub);
}

int usb_remove_device(struct usb_device *udev)
{
	struct usb_hub *hub;
	struct usb_interface *intf;

	if (!udev->parent)	
		return -EINVAL;
	hub = usb_hub_to_struct_hub(udev->parent);
	if (!hub)
		return -ENODEV;

	intf = to_usb_interface(hub->intfdev);

	usb_autopm_get_interface(intf);
	set_bit(udev->portnum, hub->removed_bits);
	hub_port_logical_disconnect(hub, udev->portnum);
	usb_autopm_put_interface(intf);
	return 0;
}

enum hub_activation_type {
	HUB_INIT, HUB_INIT2, HUB_INIT3,		/* INITs must come first */
	HUB_POST_RESET, HUB_RESUME, HUB_RESET_RESUME,
};

static void hub_init_func2(struct work_struct *ws);
static void hub_init_func3(struct work_struct *ws);

static void hub_activate(struct usb_hub *hub, enum hub_activation_type type)
{
	struct usb_device *hdev = hub->hdev;
	struct usb_hcd *hcd;
	int ret;
	int port1;
	int status;
	bool need_debounce_delay = false;
	unsigned delay;

	
	if (type == HUB_INIT2)
		goto init2;
	if (type == HUB_INIT3)
		goto init3;

	if (type != HUB_RESUME) {
		if (hdev->parent && hub_is_superspeed(hdev)) {
			ret = usb_control_msg(hdev, usb_sndctrlpipe(hdev, 0),
					HUB_SET_DEPTH, USB_RT_HUB,
					hdev->level - 1, 0, NULL, 0,
					USB_CTRL_SET_TIMEOUT);
			if (ret < 0)
				dev_err(hub->intfdev,
						"set hub depth failed\n");
		}

		if (type == HUB_INIT) {
			delay = hub_power_on(hub, false);
#ifdef CONFIG_USB_OTG
			if (hdev->bus->is_b_host)
				goto init2;
#endif
			PREPARE_DELAYED_WORK(&hub->init_work, hub_init_func2);
			schedule_delayed_work(&hub->init_work,
					msecs_to_jiffies(delay));

			
			usb_autopm_get_interface_no_resume(
					to_usb_interface(hub->intfdev));
			return;		
		} else if (type == HUB_RESET_RESUME) {
			hcd = bus_to_hcd(hdev->bus);
			if (hcd->driver->update_hub_device) {
				ret = hcd->driver->update_hub_device(hcd, hdev,
						&hub->tt, GFP_NOIO);
				if (ret < 0) {
					dev_err(hub->intfdev, "Host not "
							"accepting hub info "
							"update.\n");
					dev_err(hub->intfdev, "LS/FS devices "
							"and hubs may not work "
							"under this hub\n.");
				}
			}
			hub_power_on(hub, true);
		} else {
			hub_power_on(hub, true);
		}
	}
 init2:

	for (port1 = 1; port1 <= hdev->maxchild; ++port1) {
		struct usb_device *udev = hub->ports[port1 - 1]->child;
		u16 portstatus, portchange;

		portstatus = portchange = 0;
		status = hub_port_status(hub, port1, &portstatus, &portchange);
		if (udev || (portstatus & USB_PORT_STAT_CONNECTION))
			dev_dbg(hub->intfdev,
					"port %d: status %04x change %04x\n",
					port1, portstatus, portchange);

		if ((portstatus & USB_PORT_STAT_ENABLE) && (
				type != HUB_RESUME ||
				!(portstatus & USB_PORT_STAT_CONNECTION) ||
				!udev ||
				udev->state == USB_STATE_NOTATTACHED)) {
			if (!hub_is_superspeed(hdev)) {
				usb_clear_port_feature(hdev, port1,
						   USB_PORT_FEAT_ENABLE);
				portstatus &= ~USB_PORT_STAT_ENABLE;
			} else {
				
				portstatus &= ~USB_PORT_STAT_ENABLE;
			}
		}

		
		if (portchange & USB_PORT_STAT_C_CONNECTION) {
			need_debounce_delay = true;
			usb_clear_port_feature(hub->hdev, port1,
					USB_PORT_FEAT_C_CONNECTION);
		}
		if (portchange & USB_PORT_STAT_C_ENABLE) {
			need_debounce_delay = true;
			usb_clear_port_feature(hub->hdev, port1,
					USB_PORT_FEAT_C_ENABLE);
		}
		if (portchange & USB_PORT_STAT_C_RESET) {
			need_debounce_delay = true;
			usb_clear_port_feature(hub->hdev, port1,
					USB_PORT_FEAT_C_RESET);
		}
		if ((portchange & USB_PORT_STAT_C_BH_RESET) &&
				hub_is_superspeed(hub->hdev)) {
			need_debounce_delay = true;
			usb_clear_port_feature(hub->hdev, port1,
					USB_PORT_FEAT_C_BH_PORT_RESET);
		}
		if (!(portstatus & USB_PORT_STAT_CONNECTION) ||
				(portchange & USB_PORT_STAT_C_CONNECTION))
			clear_bit(port1, hub->removed_bits);

		if (!udev || udev->state == USB_STATE_NOTATTACHED) {
			if (udev || (portstatus & USB_PORT_STAT_CONNECTION))
				set_bit(port1, hub->change_bits);

		} else if (portstatus & USB_PORT_STAT_ENABLE) {
			bool port_resumed = (portstatus &
					USB_PORT_STAT_LINK_STATE) ==
				USB_SS_PORT_LS_U0;
			if (portchange || (hub_is_superspeed(hub->hdev) &&
						port_resumed))
				set_bit(port1, hub->change_bits);

		} else if (udev->persist_enabled) {
			struct usb_port *port_dev = hub->ports[port1 - 1];

#ifdef CONFIG_PM
			udev->reset_resume = 1;
#endif
			if (port_dev->power_is_on)
				set_bit(port1, hub->change_bits);

		} else {
			
			usb_set_device_state(udev, USB_STATE_NOTATTACHED);
			set_bit(port1, hub->change_bits);
		}
	}

	if (need_debounce_delay) {
#ifdef CONFIG_USB_OTG
		if (hdev->bus->is_b_host && type == HUB_INIT)
			goto init3;
#endif

		delay = HUB_DEBOUNCE_STABLE;

		/* Don't do a long sleep inside a workqueue routine */
		if (type == HUB_INIT2) {
			PREPARE_DELAYED_WORK(&hub->init_work, hub_init_func3);
			schedule_delayed_work(&hub->init_work,
					msecs_to_jiffies(delay));
			return;		/* Continues at init3: below */
		} else {
			msleep(delay);
		}
	}
 init3:
	hub->quiescing = 0;

	status = usb_submit_urb(hub->urb, GFP_NOIO);
	if (status < 0)
		dev_err(hub->intfdev, "activate --> %d\n", status);
	if (hub->has_indicators && blinkenlights)
		schedule_delayed_work(&hub->leds, LED_CYCLE_PERIOD);

	
	kick_khubd(hub);

	
	if (type <= HUB_INIT3)
		usb_autopm_put_interface_async(to_usb_interface(hub->intfdev));
}

static void hub_init_func2(struct work_struct *ws)
{
	struct usb_hub *hub = container_of(ws, struct usb_hub, init_work.work);

	hub_activate(hub, HUB_INIT2);
}

static void hub_init_func3(struct work_struct *ws)
{
	struct usb_hub *hub = container_of(ws, struct usb_hub, init_work.work);

	hub_activate(hub, HUB_INIT3);
}

enum hub_quiescing_type {
	HUB_DISCONNECT, HUB_PRE_RESET, HUB_SUSPEND
};

static void hub_quiesce(struct usb_hub *hub, enum hub_quiescing_type type)
{
	struct usb_device *hdev = hub->hdev;
	int i;

	cancel_delayed_work_sync(&hub->init_work);

	/* khubd and related activity won't re-trigger */
	hub->quiescing = 1;

	if (type != HUB_SUSPEND) {
		/* Disconnect all the children */
		for (i = 0; i < hdev->maxchild; ++i) {
			if (hub->ports[i]->child)
				usb_disconnect(&hub->ports[i]->child);
		}
	}

	/* Stop khubd and related activity */
	usb_kill_urb(hub->urb);
	if (hub->has_indicators)
		cancel_delayed_work_sync(&hub->leds);
	if (hub->tt.hub)
		flush_work(&hub->tt.clear_work);
}

static int hub_pre_reset(struct usb_interface *intf)
{
	struct usb_hub *hub = usb_get_intfdata(intf);

	hub_quiesce(hub, HUB_PRE_RESET);
	return 0;
}

static int hub_post_reset(struct usb_interface *intf)
{
	struct usb_hub *hub = usb_get_intfdata(intf);

	hub_activate(hub, HUB_POST_RESET);
	return 0;
}

static int hub_configure(struct usb_hub *hub,
	struct usb_endpoint_descriptor *endpoint)
{
	struct usb_hcd *hcd;
	struct usb_device *hdev = hub->hdev;
	struct device *hub_dev = hub->intfdev;
	u16 hubstatus, hubchange;
	u16 wHubCharacteristics;
	unsigned int pipe;
	int maxp, ret, i;
	char *message = "out of memory";
	unsigned unit_load;
	unsigned full_load;

	hub->buffer = kmalloc(sizeof(*hub->buffer), GFP_KERNEL);
	if (!hub->buffer) {
		ret = -ENOMEM;
		goto fail;
	}

	hub->status = kmalloc(sizeof(*hub->status), GFP_KERNEL);
	if (!hub->status) {
		ret = -ENOMEM;
		goto fail;
	}
	mutex_init(&hub->status_mutex);

	hub->descriptor = kmalloc(sizeof(*hub->descriptor), GFP_KERNEL);
	if (!hub->descriptor) {
		ret = -ENOMEM;
		goto fail;
	}

	ret = get_hub_descriptor(hdev, hub->descriptor);
	if (ret < 0) {
		message = "can't read hub descriptor";
		goto fail;
	} else if (hub->descriptor->bNbrPorts > USB_MAXCHILDREN) {
		message = "hub has too many ports!";
		ret = -ENODEV;
		goto fail;
	} else if (hub->descriptor->bNbrPorts == 0) {
		message = "hub doesn't have any ports!";
		ret = -ENODEV;
		goto fail;
	}

	hdev->maxchild = hub->descriptor->bNbrPorts;
	dev_info (hub_dev, "%d port%s detected\n", hdev->maxchild,
		(hdev->maxchild == 1) ? "" : "s");

	hub->ports = kzalloc(hdev->maxchild * sizeof(struct usb_port *),
			     GFP_KERNEL);
	if (!hub->ports) {
		ret = -ENOMEM;
		goto fail;
	}

	wHubCharacteristics = le16_to_cpu(hub->descriptor->wHubCharacteristics);
	if (hub_is_superspeed(hdev)) {
		unit_load = 150;
		full_load = 900;
	} else {
		unit_load = 100;
		full_load = 500;
	}

	/* FIXME for USB 3.0, skip for now */
	if ((wHubCharacteristics & HUB_CHAR_COMPOUND) &&
			!(hub_is_superspeed(hdev))) {
		int	i;
		char	portstr [USB_MAXCHILDREN + 1];

		for (i = 0; i < hdev->maxchild; i++)
			portstr[i] = hub->descriptor->u.hs.DeviceRemovable
				    [((i + 1) / 8)] & (1 << ((i + 1) % 8))
				? 'F' : 'R';
		portstr[hdev->maxchild] = 0;
		dev_dbg(hub_dev, "compound device; port removable status: %s\n", portstr);
	} else
		dev_dbg(hub_dev, "standalone hub\n");

	switch (wHubCharacteristics & HUB_CHAR_LPSM) {
	case HUB_CHAR_COMMON_LPSM:
		dev_dbg(hub_dev, "ganged power switching\n");
		break;
	case HUB_CHAR_INDV_PORT_LPSM:
		dev_dbg(hub_dev, "individual port power switching\n");
		break;
	case HUB_CHAR_NO_LPSM:
	case HUB_CHAR_LPSM:
		dev_dbg(hub_dev, "no power switching (usb 1.0)\n");
		break;
	}

	switch (wHubCharacteristics & HUB_CHAR_OCPM) {
	case HUB_CHAR_COMMON_OCPM:
		dev_dbg(hub_dev, "global over-current protection\n");
		break;
	case HUB_CHAR_INDV_PORT_OCPM:
		dev_dbg(hub_dev, "individual port over-current protection\n");
		break;
	case HUB_CHAR_NO_OCPM:
	case HUB_CHAR_OCPM:
		dev_dbg(hub_dev, "no over-current protection\n");
		break;
	}

	spin_lock_init (&hub->tt.lock);
	INIT_LIST_HEAD (&hub->tt.clear_list);
	INIT_WORK(&hub->tt.clear_work, hub_tt_work);
	switch (hdev->descriptor.bDeviceProtocol) {
	case USB_HUB_PR_FS:
		break;
	case USB_HUB_PR_HS_SINGLE_TT:
		dev_dbg(hub_dev, "Single TT\n");
		hub->tt.hub = hdev;
		break;
	case USB_HUB_PR_HS_MULTI_TT:
		ret = usb_set_interface(hdev, 0, 1);
		if (ret == 0) {
			dev_dbg(hub_dev, "TT per port\n");
			hub->tt.multi = 1;
		} else
			dev_err(hub_dev, "Using single TT (err %d)\n",
				ret);
		hub->tt.hub = hdev;
		break;
	case USB_HUB_PR_SS:
		/* USB 3.0 hubs don't have a TT */
		break;
	default:
		dev_dbg(hub_dev, "Unrecognized hub protocol %d\n",
			hdev->descriptor.bDeviceProtocol);
		break;
	}

	/* Note 8 FS bit times == (8 bits / 12000000 bps) ~= 666ns */
	switch (wHubCharacteristics & HUB_CHAR_TTTT) {
		case HUB_TTTT_8_BITS:
			if (hdev->descriptor.bDeviceProtocol != 0) {
				hub->tt.think_time = 666;
				dev_dbg(hub_dev, "TT requires at most %d "
						"FS bit times (%d ns)\n",
					8, hub->tt.think_time);
			}
			break;
		case HUB_TTTT_16_BITS:
			hub->tt.think_time = 666 * 2;
			dev_dbg(hub_dev, "TT requires at most %d "
					"FS bit times (%d ns)\n",
				16, hub->tt.think_time);
			break;
		case HUB_TTTT_24_BITS:
			hub->tt.think_time = 666 * 3;
			dev_dbg(hub_dev, "TT requires at most %d "
					"FS bit times (%d ns)\n",
				24, hub->tt.think_time);
			break;
		case HUB_TTTT_32_BITS:
			hub->tt.think_time = 666 * 4;
			dev_dbg(hub_dev, "TT requires at most %d "
					"FS bit times (%d ns)\n",
				32, hub->tt.think_time);
			break;
	}

	/* probe() zeroes hub->indicator[] */
	if (wHubCharacteristics & HUB_CHAR_PORTIND) {
		hub->has_indicators = 1;
		dev_dbg(hub_dev, "Port indicators are supported\n");
	}

	dev_dbg(hub_dev, "power on to power good time: %dms\n",
		hub->descriptor->bPwrOn2PwrGood * 2);

	ret = usb_get_status(hdev, USB_RECIP_DEVICE, 0, &hubstatus);
	if (ret < 2) {
		message = "can't get hub status";
		goto fail;
	}
	le16_to_cpus(&hubstatus);
	hcd = bus_to_hcd(hdev->bus);
	if (hdev == hdev->bus->root_hub) {
		if (hcd->power_budget > 0)
			hdev->bus_mA = hcd->power_budget;
		else
			hdev->bus_mA = full_load * hdev->maxchild;
		if (hdev->bus_mA >= full_load)
			hub->mA_per_port = full_load;
		else {
			hub->mA_per_port = hdev->bus_mA;
			hub->limited_power = 1;
		}
	} else if ((hubstatus & (1 << USB_DEVICE_SELF_POWERED)) == 0) {
		int remaining = hdev->bus_mA -
			hub->descriptor->bHubContrCurrent;

		dev_dbg(hub_dev, "hub controller current requirement: %dmA\n",
			hub->descriptor->bHubContrCurrent);
		hub->limited_power = 1;

		if (remaining < hdev->maxchild * unit_load)
			dev_warn(hub_dev,
					"insufficient power available "
					"to use all downstream ports\n");
		hub->mA_per_port = unit_load;	

	} else {	
		hub->mA_per_port = full_load;
	}
	if (hub->mA_per_port < full_load)
		dev_dbg(hub_dev, "%umA bus power budget for each child\n",
				hub->mA_per_port);

	if (hcd->driver->update_hub_device) {
		ret = hcd->driver->update_hub_device(hcd, hdev,
				&hub->tt, GFP_KERNEL);
		if (ret < 0) {
			message = "can't update HCD hub info";
			goto fail;
		}
	}

	ret = hub_hub_status(hub, &hubstatus, &hubchange);
	if (ret < 0) {
		message = "can't get hub status";
		goto fail;
	}

	/* local power status reports aren't always correct */
	if (hdev->actconfig->desc.bmAttributes & USB_CONFIG_ATT_SELFPOWER)
		dev_dbg(hub_dev, "local power source is %s\n",
			(hubstatus & HUB_STATUS_LOCAL_POWER)
			? "lost (inactive)" : "good");

	if ((wHubCharacteristics & HUB_CHAR_OCPM) == 0)
		dev_dbg(hub_dev, "%sover-current condition exists\n",
			(hubstatus & HUB_STATUS_OVERCURRENT) ? "" : "no ");

	pipe = usb_rcvintpipe(hdev, endpoint->bEndpointAddress);
	maxp = usb_maxpacket(hdev, pipe, usb_pipeout(pipe));

	if (maxp > sizeof(*hub->buffer))
		maxp = sizeof(*hub->buffer);

	hub->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!hub->urb) {
		ret = -ENOMEM;
		goto fail;
	}

	usb_fill_int_urb(hub->urb, hdev, pipe, *hub->buffer, maxp, hub_irq,
		hub, endpoint->bInterval);

	/* maybe cycle the hub leds */
	if (hub->has_indicators && blinkenlights)
		hub->indicator [0] = INDICATOR_CYCLE;

	for (i = 0; i < hdev->maxchild; i++) {
		ret = usb_hub_create_port_device(hub, i + 1);
		if (ret < 0) {
			dev_err(hub->intfdev,
				"couldn't create port%d device.\n", i + 1);
			hdev->maxchild = i;
			goto fail_keep_maxchild;
		}
	}

	usb_hub_adjust_deviceremovable(hdev, hub->descriptor);

	hub_activate(hub, HUB_INIT);
	return 0;

fail:
	hdev->maxchild = 0;
fail_keep_maxchild:
	dev_err (hub_dev, "config failed, %s (err %d)\n",
			message, ret);
	/* hub_disconnect() frees urb and descriptor */
	return ret;
}

static void hub_release(struct kref *kref)
{
	struct usb_hub *hub = container_of(kref, struct usb_hub, kref);

	usb_put_intf(to_usb_interface(hub->intfdev));
	kfree(hub);
}

static unsigned highspeed_hubs;

static void hub_disconnect(struct usb_interface *intf)
{
	struct usb_hub *hub = usb_get_intfdata(intf);
	struct usb_device *hdev = interface_to_usbdev(intf);
	int port1;

	/* Take the hub off the event list and don't let it be added again */
	spin_lock_irq(&hub_event_lock);
	if (!list_empty(&hub->event_list)) {
		list_del_init(&hub->event_list);
		usb_autopm_put_interface_no_suspend(intf);
	}
	hub->disconnected = 1;
	spin_unlock_irq(&hub_event_lock);

	/* Disconnect all children and quiesce the hub */
	hub->error = 0;
	hub_quiesce(hub, HUB_DISCONNECT);

	/* Avoid races with recursively_mark_NOTATTACHED() */
	spin_lock_irq(&device_state_lock);
	port1 = hdev->maxchild;
	hdev->maxchild = 0;
	usb_set_intfdata(intf, NULL);
	spin_unlock_irq(&device_state_lock);

	for (; port1 > 0; --port1)
		usb_hub_remove_port_device(hub, port1);

	if (hub->hdev->speed == USB_SPEED_HIGH)
		highspeed_hubs--;

	usb_free_urb(hub->urb);
	kfree(hub->ports);
	kfree(hub->descriptor);
	kfree(hub->status);
	kfree(hub->buffer);

	pm_suspend_ignore_children(&intf->dev, false);
	kref_put(&hub->kref, hub_release);
}

static int hub_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_host_interface *desc;
	struct usb_endpoint_descriptor *endpoint;
	struct usb_device *hdev;
	struct usb_hub *hub;

	desc = intf->cur_altsetting;
	hdev = interface_to_usbdev(intf);

	pm_runtime_set_autosuspend_delay(&hdev->dev, 0);

	if (hdev->parent) {		
		usb_enable_autosuspend(hdev);
	} else {			
		const struct hc_driver *drv = bus_to_hcd(hdev->bus)->driver;

		if (drv->bus_suspend && drv->bus_resume)
			usb_enable_autosuspend(hdev);
	}

	if (hdev->level == MAX_TOPO_LEVEL) {
		dev_err(&intf->dev,
			"Unsupported bus topology: hub nested too deep\n");
		return -E2BIG;
	}

#ifdef	CONFIG_USB_OTG_BLACKLIST_HUB
	if (hdev->parent) {
		dev_warn(&intf->dev, "ignoring external hub\n");
		otg_send_event(OTG_EVENT_HUB_NOT_SUPPORTED);
		return -ENODEV;
	}
#endif

	/* Some hubs have a subclass of 1, which AFAICT according to the */
	/*  specs is not defined, but it works */
	if ((desc->desc.bInterfaceSubClass != 0) &&
	    (desc->desc.bInterfaceSubClass != 1)) {
descriptor_error:
		dev_err (&intf->dev, "bad descriptor, ignoring hub\n");
		return -EIO;
	}

	/* Multiple endpoints? What kind of mutant ninja-hub is this? */
	if (desc->desc.bNumEndpoints != 1)
		goto descriptor_error;

	endpoint = &desc->endpoint[0].desc;

	/* If it's not an interrupt in endpoint, we'd better punt! */
	if (!usb_endpoint_is_int_in(endpoint))
		goto descriptor_error;

	/* We found a hub */
	dev_info (&intf->dev, "USB hub found\n");

	hub = kzalloc(sizeof(*hub), GFP_KERNEL);
	if (!hub) {
		dev_dbg (&intf->dev, "couldn't kmalloc hub struct\n");
		return -ENOMEM;
	}

	kref_init(&hub->kref);
	INIT_LIST_HEAD(&hub->event_list);
	hub->intfdev = &intf->dev;
	hub->hdev = hdev;
	INIT_DELAYED_WORK(&hub->leds, led_work);
	INIT_DELAYED_WORK(&hub->init_work, NULL);
	usb_get_intf(intf);

	usb_set_intfdata (intf, hub);
	intf->needs_remote_wakeup = 1;
	pm_suspend_ignore_children(&intf->dev, true);

	if (hdev->speed == USB_SPEED_HIGH)
		highspeed_hubs++;

	if (id->driver_info & HUB_QUIRK_CHECK_PORT_AUTOSUSPEND)
		hub->quirk_check_port_auto_suspend = 1;

	if (hub_configure(hub, endpoint) >= 0)
		return 0;

	hub_disconnect (intf);
	return -ENODEV;
}

static int
hub_ioctl(struct usb_interface *intf, unsigned int code, void *user_data)
{
	struct usb_device *hdev = interface_to_usbdev (intf);
	struct usb_hub *hub = usb_hub_to_struct_hub(hdev);

	if (!hub)
		return -ENODEV;

	/* assert ifno == 0 (part of hub spec) */
	switch (code) {
	case USBDEVFS_HUB_PORTINFO: {
		struct usbdevfs_hub_portinfo *info = user_data;
		int i;

		spin_lock_irq(&device_state_lock);
		if (hdev->devnum <= 0)
			info->nports = 0;
		else {
			info->nports = hdev->maxchild;
			for (i = 0; i < info->nports; i++) {
				if (hub->ports[i]->child == NULL)
					info->port[i] = 0;
				else
					info->port[i] =
						hub->ports[i]->child->devnum;
			}
		}
		spin_unlock_irq(&device_state_lock);

		return info->nports + 1;
		}

	default:
		return -ENOSYS;
	}
}

static int find_port_owner(struct usb_device *hdev, unsigned port1,
		struct dev_state ***ppowner)
{
	struct usb_hub *hub = usb_hub_to_struct_hub(hdev);

	if (!hub || (hdev->state == USB_STATE_NOTATTACHED))
		return -ENODEV;
	if (port1 == 0 || port1 > hdev->maxchild)
		return -EINVAL;

	*ppowner = &(hub->ports[port1 - 1]->port_owner);
	return 0;
}

int usb_hub_claim_port(struct usb_device *hdev, unsigned port1,
		       struct dev_state *owner)
{
	int rc;
	struct dev_state **powner;

	rc = find_port_owner(hdev, port1, &powner);
	if (rc)
		return rc;
	if (*powner)
		return -EBUSY;
	*powner = owner;
	return rc;
}

int usb_hub_release_port(struct usb_device *hdev, unsigned port1,
			 struct dev_state *owner)
{
	int rc;
	struct dev_state **powner;

	rc = find_port_owner(hdev, port1, &powner);
	if (rc)
		return rc;
	if (*powner != owner)
		return -ENOENT;
	*powner = NULL;
	return rc;
}

void usb_hub_release_all_ports(struct usb_device *hdev, struct dev_state *owner)
{
	struct usb_hub *hub = usb_hub_to_struct_hub(hdev);
	int n;

	if (!hub)
		return;

	for (n = 0; n < hdev->maxchild; n++) {
		if (hub->ports[n]->port_owner == owner)
			hub->ports[n]->port_owner = NULL;
	}

}

bool usb_device_is_owned(struct usb_device *udev)
{
	struct usb_hub *hub;

	if (udev->state == USB_STATE_NOTATTACHED || !udev->parent)
		return false;

	hub = usb_hub_to_struct_hub(udev->parent);
	if (!hub)
		return false;

	return !!hub->ports[udev->portnum - 1]->port_owner;
}

static void recursively_mark_NOTATTACHED(struct usb_device *udev)
{
	struct usb_hub *hub = usb_hub_to_struct_hub(udev);
	int i;

	if (!hub)
		return;

	for (i = 0; i < udev->maxchild; ++i) {
		if (hub->ports[i]->child)
			recursively_mark_NOTATTACHED(hub->ports[i]->child);
	}
	if (udev->state == USB_STATE_SUSPENDED)
		udev->active_duration -= jiffies;
	udev->state = USB_STATE_NOTATTACHED;
}

void usb_set_device_state(struct usb_device *udev,
		enum usb_device_state new_state)
{
	unsigned long flags;
	int wakeup = -1;

	spin_lock_irqsave(&device_state_lock, flags);
	if (udev->state == USB_STATE_NOTATTACHED)
		;	
	else if (new_state != USB_STATE_NOTATTACHED) {

		if (udev->parent) {
			if (udev->state == USB_STATE_SUSPENDED
					|| new_state == USB_STATE_SUSPENDED)
				;	
			else if (new_state == USB_STATE_CONFIGURED)
				wakeup = udev->actconfig->desc.bmAttributes
					 & USB_CONFIG_ATT_WAKEUP;
			else
				wakeup = 0;
		}
		if (udev->state == USB_STATE_SUSPENDED &&
			new_state != USB_STATE_SUSPENDED)
			udev->active_duration -= jiffies;
		else if (new_state == USB_STATE_SUSPENDED &&
				udev->state != USB_STATE_SUSPENDED)
			udev->active_duration += jiffies;
		udev->state = new_state;
	} else
		recursively_mark_NOTATTACHED(udev);
	spin_unlock_irqrestore(&device_state_lock, flags);
	if (wakeup >= 0)
		device_set_wakeup_capable(&udev->dev, wakeup);
}
EXPORT_SYMBOL_GPL(usb_set_device_state);

static void choose_devnum(struct usb_device *udev)
{
	int		devnum;
	struct usb_bus	*bus = udev->bus;

	
	if (udev->wusb) {
		devnum = udev->portnum + 1;
		BUG_ON(test_bit(devnum, bus->devmap.devicemap));
	} else {
		devnum = find_next_zero_bit(bus->devmap.devicemap, 128,
					    bus->devnum_next);
		if (devnum >= 128)
			devnum = find_next_zero_bit(bus->devmap.devicemap,
						    128, 1);
		bus->devnum_next = ( devnum >= 127 ? 1 : devnum + 1);
	}
	if (devnum < 128) {
		set_bit(devnum, bus->devmap.devicemap);
		udev->devnum = devnum;
	}
}

static void release_devnum(struct usb_device *udev)
{
	if (udev->devnum > 0) {
		clear_bit(udev->devnum, udev->bus->devmap.devicemap);
		udev->devnum = -1;
	}
}

static void update_devnum(struct usb_device *udev, int devnum)
{
	/* The address for a WUSB device is managed by wusbcore. */
	if (!udev->wusb)
		udev->devnum = devnum;
}

static void hub_free_dev(struct usb_device *udev)
{
	struct usb_hcd *hcd = bus_to_hcd(udev->bus);

	
	if (hcd->driver->free_dev && udev->parent)
		hcd->driver->free_dev(hcd, udev);
}

void usb_disconnect(struct usb_device **pdev)
{
	struct usb_device	*udev = *pdev;
	struct usb_hub		*hub = usb_hub_to_struct_hub(udev);
	int			i;

	usb_set_device_state(udev, USB_STATE_NOTATTACHED);
	dev_info(&udev->dev, "USB disconnect, device number %d\n",
			udev->devnum);

#ifdef CONFIG_USB_OTG
	if (udev->bus->hnp_support && udev->portnum == udev->bus->otg_port) {
		cancel_delayed_work_sync(&udev->bus->hnp_polling);
		udev->bus->hnp_support = 0;
	}
#endif

	usb_lock_device(udev);

	
	for (i = 0; i < udev->maxchild; i++) {
		if (hub && hub->ports[i]->child)
			usb_disconnect(&hub->ports[i]->child);
	}

	dev_dbg (&udev->dev, "unregistering device\n");
	usb_disable_device(udev, 0);
	usb_hcd_synchronize_unlinks(udev);

	if (udev->parent) {
		struct usb_hub *hub = usb_hub_to_struct_hub(udev->parent);
		struct usb_port	*port_dev = NULL;

		if (hub)
			port_dev = hub->ports[udev->portnum - 1];

		sysfs_remove_link(&udev->dev.kobj, "port");

		if (port_dev) {
			sysfs_remove_link(&port_dev->dev.kobj, "device");
			if (!port_dev->did_runtime_put)
				pm_runtime_put(&port_dev->dev);
			else
				port_dev->did_runtime_put = false;
		}
	}

	usb_remove_ep_devs(&udev->ep0);
	usb_unlock_device(udev);

	device_del(&udev->dev);

	release_devnum(udev);

	
	spin_lock_irq(&device_state_lock);
	*pdev = NULL;
	spin_unlock_irq(&device_state_lock);

	hub_free_dev(udev);

	put_device(&udev->dev);
}

#ifdef CONFIG_USB_ANNOUNCE_NEW_DEVICES
static void show_string(struct usb_device *udev, char *id, char *string)
{
	if (!string)
		return;
	dev_info(&udev->dev, "%s: %s\n", id, string);
}

static void announce_device(struct usb_device *udev)
{
	dev_info(&udev->dev, "New USB device found, idVendor=%04x, idProduct=%04x\n",
		le16_to_cpu(udev->descriptor.idVendor),
		le16_to_cpu(udev->descriptor.idProduct));
	dev_info(&udev->dev,
		"New USB device strings: Mfr=%d, Product=%d, SerialNumber=%d\n",
		udev->descriptor.iManufacturer,
		udev->descriptor.iProduct,
		udev->descriptor.iSerialNumber);
	show_string(udev, "Product", udev->product);
	show_string(udev, "Manufacturer", udev->manufacturer);
	show_string(udev, "SerialNumber", udev->serial);
}
#else
static inline void announce_device(struct usb_device *udev) { }
#endif

#ifdef	CONFIG_USB_OTG
#include "otg_whitelist.h"
#endif

static int usb_enumerate_device_otg(struct usb_device *udev)
{
	int err = 0;

#ifdef	CONFIG_USB_OTG
	bool old_otg = false;
	if (!udev->bus->is_b_host
			&& udev->config
			&& udev->parent == udev->bus->root_hub) {
		struct usb_otg_descriptor	*desc = NULL;
		struct usb_bus			*bus = udev->bus;

		
		if (__usb_get_extra_descriptor (udev->rawdescriptors[0],
					le16_to_cpu(udev->config[0].desc.wTotalLength),
					USB_DT_OTG, (void **) &desc) == 0) {
			if (desc->bmAttributes & USB_OTG_HNP) {
				unsigned		port1 = udev->portnum;

				dev_info(&udev->dev,
					"Dual-Role OTG device on %sHNP port\n",
					(port1 == bus->otg_port)
						? "" : "non-");

				
				if (port1 != bus->otg_port)
					goto out;

				bus->hnp_support = 1;


				if ((le16_to_cpu(desc->bLength) ==
						USB_DT_OTG_SIZE) &&
					le16_to_cpu(desc->bcdOTG) >= 0x0200)
					goto out;

				old_otg = true;

				
				err = usb_control_msg(udev,
					usb_sndctrlpipe(udev, 0),
					USB_REQ_SET_FEATURE, 0,
					USB_DEVICE_A_HNP_SUPPORT,
					0, NULL, 0, USB_CTRL_SET_TIMEOUT);
				if (err < 0) {
					dev_info(&udev->dev,
						"can't set HNP mode: %d\n",
						err);
					bus->hnp_support = 0;
				}
			}
		}
	}
out:
	if ((udev->quirks & USB_QUIRK_OTG_PET)) {
		if (le16_to_cpu(udev->descriptor.bcdDevice) &
			OTG_TTST_VBUS_OFF)
			udev->bus->otg_vbus_off = 1;
		if (udev->bus->is_b_host || old_otg)
			udev->bus->quick_hnp = 1;
	}

	if (!is_targeted(udev)) {

		otg_send_event(OTG_EVENT_DEV_NOT_SUPPORTED);

		if (udev->bus->hnp_support) {
			err = usb_port_suspend(udev, PMSG_SUSPEND);
			if (err < 0)
				dev_dbg(&udev->dev, "HNP fail, %d\n", err);
		}
		err = -ENOTSUPP;
	} else if (udev->bus->hnp_support &&
		udev->portnum == udev->bus->otg_port) {
		if (udev->bus->quick_hnp)
			schedule_delayed_work(&udev->bus->hnp_polling,
				msecs_to_jiffies(OTG_TTST_SUSP));
		else
			schedule_delayed_work(&udev->bus->hnp_polling,
				msecs_to_jiffies(THOST_REQ_POLL));
	}
#endif
	return err;
}

/**
 * usb_enumerate_device - Read device configs/intfs/otg (usbcore-internal)
 * @udev: newly addressed device (in ADDRESS state)
 *
 * This is only called by usb_new_device() and usb_authorize_device()
 * and FIXME -- all comments that apply to them apply here wrt to
 * environment.
 *
 * If the device is WUSB and not authorized, we don't attempt to read
 * the string descriptors, as they will be errored out by the device
 * until it has been authorized.
 */
static int usb_enumerate_device(struct usb_device *udev)
{
	int err;

	if (udev->config == NULL) {
		err = usb_get_configuration(udev);
		if (err < 0) {
			if (err != -ENODEV)
				dev_err(&udev->dev, "can't read configurations, error %d\n",
						err);
			return err;
		}
	}

	/* read the standard strings and cache them if present */
	udev->product = usb_cache_string(udev, udev->descriptor.iProduct);
	udev->manufacturer = usb_cache_string(udev,
					      udev->descriptor.iManufacturer);
	udev->serial = usb_cache_string(udev, udev->descriptor.iSerialNumber);

	err = usb_enumerate_device_otg(udev);
	if (err < 0)
		return err;

	usb_detect_interface_quirks(udev);

	return 0;
}

static void set_usb_port_removable(struct usb_device *udev)
{
	struct usb_device *hdev = udev->parent;
	struct usb_hub *hub;
	u8 port = udev->portnum;
	u16 wHubCharacteristics;
	bool removable = true;

	if (!hdev)
		return;

	hub = usb_hub_to_struct_hub(udev->parent);
	if (!hub)
		return;

	wHubCharacteristics = le16_to_cpu(hub->descriptor->wHubCharacteristics);

	if (!(wHubCharacteristics & HUB_CHAR_COMPOUND))
		return;

	if (hub_is_superspeed(hdev)) {
		if (le16_to_cpu(hub->descriptor->u.ss.DeviceRemovable)
				& (1 << port))
			removable = false;
	} else {
		if (hub->descriptor->u.hs.DeviceRemovable[port / 8] & (1 << (port % 8)))
			removable = false;
	}

	if (removable)
		udev->removable = USB_DEVICE_REMOVABLE;
	else
		udev->removable = USB_DEVICE_FIXED;
}

int usb_new_device(struct usb_device *udev)
{
	int err;

	if (udev->parent) {
		device_init_wakeup(&udev->dev, 0);
	}

	
	pm_runtime_set_active(&udev->dev);
	pm_runtime_get_noresume(&udev->dev);
	pm_runtime_use_autosuspend(&udev->dev);
	pm_runtime_enable(&udev->dev);

	usb_disable_autosuspend(udev);

	err = usb_enumerate_device(udev);	
	if (err < 0)
		goto fail;
	dev_dbg(&udev->dev, "udev %d, busnum %d, minor = %d\n",
			udev->devnum, udev->bus->busnum,
			(((udev->bus->busnum-1) * 128) + (udev->devnum-1)));
	
	udev->dev.devt = MKDEV(USB_DEVICE_MAJOR,
			(((udev->bus->busnum-1) * 128) + (udev->devnum-1)));

	
	announce_device(udev);

	if (udev->serial)
		add_device_randomness(udev->serial, strlen(udev->serial));
	if (udev->product)
		add_device_randomness(udev->product, strlen(udev->product));
	if (udev->manufacturer)
		add_device_randomness(udev->manufacturer,
				      strlen(udev->manufacturer));

	device_enable_async_suspend(&udev->dev);

	if (udev->parent)
		set_usb_port_removable(udev);

	err = device_add(&udev->dev);
	if (err) {
		dev_err(&udev->dev, "can't device_add, error %d\n", err);
		goto fail;
	}

	
	if (udev->parent) {
		struct usb_hub *hub = usb_hub_to_struct_hub(udev->parent);
		struct usb_port	*port_dev;

		if (!hub)
			goto fail;

		port_dev = hub->ports[udev->portnum - 1];
		if (!port_dev)
			goto fail;

		err = sysfs_create_link(&udev->dev.kobj,
				&port_dev->dev.kobj, "port");
		if (err)
			goto fail;

		err = sysfs_create_link(&port_dev->dev.kobj,
				&udev->dev.kobj, "device");
		if (err) {
			sysfs_remove_link(&udev->dev.kobj, "port");
			goto fail;
		}

		pm_runtime_get_sync(&port_dev->dev);
	}

	(void) usb_create_ep_devs(&udev->dev, &udev->ep0, udev);
	usb_mark_last_busy(udev);
	pm_runtime_put_sync_autosuspend(&udev->dev);
	return err;

fail:
	usb_set_device_state(udev, USB_STATE_NOTATTACHED);
	pm_runtime_disable(&udev->dev);
	pm_runtime_set_suspended(&udev->dev);
	return err;
}

/**
 * usb_deauthorize_device - deauthorize a device (usbcore-internal)
 * @usb_dev: USB device
 *
 * Move the USB device to a very basic state where interfaces are disabled
 * and the device is in fact unconfigured and unusable.
 *
 * We share a lock (that we have) with device_del(), so we need to
 * defer its call.
 */
int usb_deauthorize_device(struct usb_device *usb_dev)
{
	usb_lock_device(usb_dev);
	if (usb_dev->authorized == 0)
		goto out_unauthorized;

	usb_dev->authorized = 0;
	usb_set_configuration(usb_dev, -1);

out_unauthorized:
	usb_unlock_device(usb_dev);
	return 0;
}


int usb_authorize_device(struct usb_device *usb_dev)
{
	int result = 0, c;

	usb_lock_device(usb_dev);
	if (usb_dev->authorized == 1)
		goto out_authorized;

	result = usb_autoresume_device(usb_dev);
	if (result < 0) {
		dev_err(&usb_dev->dev,
			"can't autoresume for authorization: %d\n", result);
		goto error_autoresume;
	}
	result = usb_get_device_descriptor(usb_dev, sizeof(usb_dev->descriptor));
	if (result < 0) {
		dev_err(&usb_dev->dev, "can't re-read device descriptor for "
			"authorization: %d\n", result);
		goto error_device_descriptor;
	}

	usb_dev->authorized = 1;
	c = usb_choose_configuration(usb_dev);
	if (c >= 0) {
		result = usb_set_configuration(usb_dev, c);
		if (result) {
			dev_err(&usb_dev->dev,
				"can't set config #%d, error %d\n", c, result);
		}
	}
	dev_info(&usb_dev->dev, "authorized to connect\n");

error_device_descriptor:
	usb_autosuspend_device(usb_dev);
error_autoresume:
out_authorized:
	usb_unlock_device(usb_dev);	
	return result;
}


static unsigned hub_is_wusb(struct usb_hub *hub)
{
	struct usb_hcd *hcd;
	if (hub->hdev->parent != NULL)  
		return 0;
	hcd = container_of(hub->hdev->bus, struct usb_hcd, self);
	return hcd->wireless;
}


#define PORT_RESET_TRIES	5
#define SET_ADDRESS_TRIES	2
#define GET_DESCRIPTOR_TRIES	2
#define SET_CONFIG_TRIES	(2 * (use_both_schemes + 1))
#define USE_NEW_SCHEME(i)	((i) / 2 == (int)old_scheme_first)

#define HUB_ROOT_RESET_TIME	50	/* times are in msec */
#define HUB_SHORT_RESET_TIME	10
#define HUB_BH_RESET_TIME	50
#define HUB_LONG_RESET_TIME	200
#define HUB_RESET_TIMEOUT	800

static int hub_port_reset(struct usb_hub *hub, int port1,
			struct usb_device *udev, unsigned int delay, bool warm);

static bool hub_port_warm_reset_required(struct usb_hub *hub, u16 portstatus)
{
	return hub_is_superspeed(hub->hdev) &&
		(((portstatus & USB_PORT_STAT_LINK_STATE) ==
		  USB_SS_PORT_LS_SS_INACTIVE) ||
		 ((portstatus & USB_PORT_STAT_LINK_STATE) ==
		  USB_SS_PORT_LS_COMP_MOD)) ;
}

static int hub_port_wait_reset(struct usb_hub *hub, int port1,
			struct usb_device *udev, unsigned int delay, bool warm)
{
	int delay_time, ret;
	u16 portstatus;
	u16 portchange;

	for (delay_time = 0;
			delay_time < HUB_RESET_TIMEOUT;
			delay_time += delay) {
		/* wait to give the device a chance to reset */
		msleep(delay);

		/* read and decode port status */
		ret = hub_port_status(hub, port1, &portstatus, &portchange);
		if (ret < 0)
			return ret;

		/* The port state is unknown until the reset completes. */
		if (!(portstatus & USB_PORT_STAT_RESET))
			break;

		/* switch to the long delay after two short delay failures */
		if (delay_time >= 2 * HUB_SHORT_RESET_TIME)
			delay = HUB_LONG_RESET_TIME;

		dev_dbg (hub->intfdev,
			"port %d not %sreset yet, waiting %dms\n",
			port1, warm ? "warm " : "", delay);
	}

	if ((portstatus & USB_PORT_STAT_RESET))
		return -EBUSY;

	if (hub_port_warm_reset_required(hub, portstatus))
		return -ENOTCONN;

	
	if (!(portstatus & USB_PORT_STAT_CONNECTION))
		return -ENOTCONN;

	if (!hub_is_superspeed(hub->hdev) &&
			(portchange & USB_PORT_STAT_C_CONNECTION))
		return -ENOTCONN;

	if (!(portstatus & USB_PORT_STAT_ENABLE))
		return -EBUSY;

	if (!udev)
		return 0;

	if (hub_is_wusb(hub))
		udev->speed = USB_SPEED_WIRELESS;
	else if (hub_is_superspeed(hub->hdev))
		udev->speed = USB_SPEED_SUPER;
	else if (portstatus & USB_PORT_STAT_HIGH_SPEED)
		udev->speed = USB_SPEED_HIGH;
	else if (portstatus & USB_PORT_STAT_LOW_SPEED)
		udev->speed = USB_SPEED_LOW;
	else
		udev->speed = USB_SPEED_FULL;
	return 0;
}

static void hub_port_finish_reset(struct usb_hub *hub, int port1,
			struct usb_device *udev, int *status)
{
	switch (*status) {
	case 0:
		
		msleep(10 + 40);
		if (udev) {
			struct usb_hcd *hcd = bus_to_hcd(udev->bus);

			update_devnum(udev, 0);
			if (hcd->driver->reset_device)
				hcd->driver->reset_device(hcd, udev);
		}
		
	case -ENOTCONN:
	case -ENODEV:
		usb_clear_port_feature(hub->hdev,
				port1, USB_PORT_FEAT_C_RESET);
		if (hub_is_superspeed(hub->hdev)) {
			usb_clear_port_feature(hub->hdev, port1,
					USB_PORT_FEAT_C_BH_PORT_RESET);
			usb_clear_port_feature(hub->hdev, port1,
					USB_PORT_FEAT_C_PORT_LINK_STATE);
			usb_clear_port_feature(hub->hdev, port1,
					USB_PORT_FEAT_C_CONNECTION);
		}
		if (udev)
			usb_set_device_state(udev, *status
					? USB_STATE_NOTATTACHED
					: USB_STATE_DEFAULT);
		break;
	}
}

static int hub_port_reset(struct usb_hub *hub, int port1,
			struct usb_device *udev, unsigned int delay, bool warm)
{
	int i, status;
	u16 portchange, portstatus;

	if (!hub_is_superspeed(hub->hdev)) {
		if (warm) {
			dev_err(hub->intfdev, "only USB3 hub support "
						"warm reset\n");
			return -EINVAL;
		}
		down_read(&ehci_cf_port_reset_rwsem);
	} else if (!warm) {
		status = hub_port_status(hub, port1,
					&portstatus, &portchange);
		if (status < 0)
			goto done;

		if (hub_port_warm_reset_required(hub, portstatus))
			warm = true;
	}

	/* Reset the port */
	for (i = 0; i < PORT_RESET_TRIES; i++) {
		status = set_port_feature(hub->hdev, port1, (warm ?
					USB_PORT_FEAT_BH_PORT_RESET :
					USB_PORT_FEAT_RESET));
		if (status == -ENODEV) {
			;	/* The hub is gone */
		} else if (status) {
			dev_err(hub->intfdev,
					"cannot %sreset port %d (err = %d)\n",
					warm ? "warm " : "", port1, status);
		} else {
			status = hub_port_wait_reset(hub, port1, udev, delay,
								warm);
			if (status && status != -ENOTCONN && status != -ENODEV)
				dev_dbg(hub->intfdev,
						"port_wait_reset: err = %d\n",
						status);
		}

		
		if (status == 0 || status == -ENOTCONN || status == -ENODEV) {
			hub_port_finish_reset(hub, port1, udev, &status);

			if (!hub_is_superspeed(hub->hdev))
				goto done;

			if (hub_port_status(hub, port1,
					&portstatus, &portchange) < 0)
				goto done;

			if (!hub_port_warm_reset_required(hub, portstatus))
				goto done;

			if (!warm) {
				dev_dbg(hub->intfdev, "hot reset failed, warm reset port %d\n",
						port1);
				warm = true;
			}
		}

		dev_dbg (hub->intfdev,
			"port %d not enabled, trying %sreset again...\n",
			port1, warm ? "warm " : "");
		delay = HUB_LONG_RESET_TIME;
	}

	dev_err (hub->intfdev,
		"Cannot enable port %i.  Maybe the USB cable is bad?\n",
		port1);

done:
	if (!hub_is_superspeed(hub->hdev))
		up_read(&ehci_cf_port_reset_rwsem);

	return status;
}

static int port_is_power_on(struct usb_hub *hub, unsigned portstatus)
{
	int ret = 0;

	if (hub_is_superspeed(hub->hdev)) {
		if (portstatus & USB_SS_PORT_STAT_POWER)
			ret = 1;
	} else {
		if (portstatus & USB_PORT_STAT_POWER)
			ret = 1;
	}

	return ret;
}

#ifdef	CONFIG_PM

static int port_is_suspended(struct usb_hub *hub, unsigned portstatus)
{
	int ret = 0;

	if (hub_is_superspeed(hub->hdev)) {
		if ((portstatus & USB_PORT_STAT_LINK_STATE)
				== USB_SS_PORT_LS_U3)
			ret = 1;
	} else {
		if (portstatus & USB_PORT_STAT_SUSPEND)
			ret = 1;
	}

	return ret;
}

static int check_port_resume_type(struct usb_device *udev,
		struct usb_hub *hub, int port1,
		int status, unsigned portchange, unsigned portstatus)
{
	
	if (status || port_is_suspended(hub, portstatus) ||
			!port_is_power_on(hub, portstatus) ||
			!(portstatus & USB_PORT_STAT_CONNECTION)) {
		if (status >= 0)
			status = -ENODEV;
	}

	else if (!(portstatus & USB_PORT_STAT_ENABLE) && !udev->reset_resume) {
		if (udev->persist_enabled)
			udev->reset_resume = 1;
		else
			status = -ENODEV;
	}

	if (status) {
		dev_dbg(hub->intfdev,
				"port %d status %04x.%04x after resume, %d\n",
				port1, portchange, portstatus, status);
	} else if (udev->reset_resume) {

		/* Late port handoff can set status-change bits */
		if (portchange & USB_PORT_STAT_C_CONNECTION)
			usb_clear_port_feature(hub->hdev, port1,
					USB_PORT_FEAT_C_CONNECTION);
		if (portchange & USB_PORT_STAT_C_ENABLE)
			usb_clear_port_feature(hub->hdev, port1,
					USB_PORT_FEAT_C_ENABLE);
	}

	return status;
}

int usb_disable_ltm(struct usb_device *udev)
{
	struct usb_hcd *hcd = bus_to_hcd(udev->bus);

	
	if (!usb_device_supports_ltm(hcd->self.root_hub) ||
			!usb_device_supports_ltm(udev))
		return 0;

	if (!udev->actconfig)
		return 0;

	return usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			USB_REQ_CLEAR_FEATURE, USB_RECIP_DEVICE,
			USB_DEVICE_LTM_ENABLE, 0, NULL, 0,
			USB_CTRL_SET_TIMEOUT);
}
EXPORT_SYMBOL_GPL(usb_disable_ltm);

void usb_enable_ltm(struct usb_device *udev)
{
	struct usb_hcd *hcd = bus_to_hcd(udev->bus);

	
	if (!usb_device_supports_ltm(hcd->self.root_hub) ||
			!usb_device_supports_ltm(udev))
		return;

	if (!udev->actconfig)
		return;

	usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			USB_REQ_SET_FEATURE, USB_RECIP_DEVICE,
			USB_DEVICE_LTM_ENABLE, 0, NULL, 0,
			USB_CTRL_SET_TIMEOUT);
}
EXPORT_SYMBOL_GPL(usb_enable_ltm);

#ifdef	CONFIG_PM
static int usb_disable_function_remotewakeup(struct usb_device *udev)
{
	return usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
				USB_REQ_CLEAR_FEATURE, USB_RECIP_INTERFACE,
				USB_INTRF_FUNC_SUSPEND,	0, NULL, 0,
				USB_CTRL_SET_TIMEOUT);
}

static unsigned wakeup_enabled_descendants(struct usb_device *udev)
{
	struct usb_hub *hub = usb_hub_to_struct_hub(udev);

	return udev->do_remote_wakeup +
			(hub ? hub->wakeup_enabled_descendants : 0);
}

int usb_port_suspend(struct usb_device *udev, pm_message_t msg)
{
	struct usb_hub	*hub = usb_hub_to_struct_hub(udev->parent);
	struct usb_port *port_dev;
	int		port1 = udev->portnum;
	int		status;
	bool		really_suspend = true;

	if (!hub)
		return -ENODEV;

	port_dev = hub->ports[udev->portnum - 1];
	if (udev->do_remote_wakeup) {
		if (!hub_is_superspeed(hub->hdev)) {
			status = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
					USB_REQ_SET_FEATURE, USB_RECIP_DEVICE,
					USB_DEVICE_REMOTE_WAKEUP, 0,
					NULL, 0,
					USB_CTRL_SET_TIMEOUT);
		} else {
			status = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
					USB_REQ_SET_FEATURE,
					USB_RECIP_INTERFACE,
					USB_INTRF_FUNC_SUSPEND,
					USB_INTRF_FUNC_SUSPEND_RW |
					USB_INTRF_FUNC_SUSPEND_LP,
					NULL, 0,
					USB_CTRL_SET_TIMEOUT);
		}
		if (status) {
			dev_dbg(&udev->dev, "won't remote wakeup, status %d\n",
					status);
			/* bail if autosuspend is requested */
			if (PMSG_IS_AUTO(msg))
				goto err_wakeup;
		}
	}
#ifdef CONFIG_USB_OTG
	if (!udev->bus->is_b_host && udev->bus->hnp_support &&
		udev->portnum == udev->bus->otg_port) {
		status = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
				USB_REQ_SET_FEATURE, 0,
				USB_DEVICE_B_HNP_ENABLE,
				0, NULL, 0, USB_CTRL_SET_TIMEOUT);
		if (status < 0) {
			otg_send_event(OTG_EVENT_NO_RESP_FOR_HNP_ENABLE);
			dev_dbg(&udev->dev, "can't enable HNP on port %d, "
					"status %d\n", port1, status);
		} else {
			udev->bus->b_hnp_enable = 1;
		}
	}
#endif

	/* disable USB2 hardware LPM */
	if (udev->usb2_hw_lpm_enabled == 1)
		usb_set_usb2_hardware_lpm(udev, 0);

	if (usb_disable_ltm(udev)) {
		dev_err(&udev->dev, "Failed to disable LTM before suspend\n.");
		status = -ENOMEM;
		if (PMSG_IS_AUTO(msg))
			goto err_ltm;
	}
	if (usb_unlocked_disable_lpm(udev)) {
		dev_err(&udev->dev, "Failed to disable LPM before suspend\n.");
		status = -ENOMEM;
		if (PMSG_IS_AUTO(msg))
			goto err_lpm3;
	}

	
	if (hub_is_superspeed(hub->hdev))
		status = hub_set_port_link_state(hub, port1, USB_SS_PORT_LS_U3);

	else if (PMSG_IS_AUTO(msg) || wakeup_enabled_descendants(udev) > 0)
		status = set_port_feature(hub->hdev, port1,
				USB_PORT_FEAT_SUSPEND);
	else {
		really_suspend = false;
		status = 0;
	}
	if (status) {
		dev_dbg(hub->intfdev, "can't suspend port %d, status %d\n",
				port1, status);

		/* Try to enable USB3 LPM and LTM again */
		usb_unlocked_enable_lpm(udev);
 err_lpm3:
		usb_enable_ltm(udev);
 err_ltm:
		/* Try to enable USB2 hardware LPM again */
		if (udev->usb2_hw_lpm_capable == 1)
			usb_set_usb2_hardware_lpm(udev, 1);

		if (udev->do_remote_wakeup) {
			if (udev->speed < USB_SPEED_SUPER)
				usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
						USB_REQ_CLEAR_FEATURE,
						USB_RECIP_DEVICE,
						USB_DEVICE_REMOTE_WAKEUP, 0,
						NULL, 0, USB_CTRL_SET_TIMEOUT);
			else
				usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
						USB_REQ_CLEAR_FEATURE,
						USB_RECIP_INTERFACE,
						USB_INTRF_FUNC_SUSPEND, 0,
						NULL, 0, USB_CTRL_SET_TIMEOUT);
		}
 err_wakeup:

		/* System sleep transitions should never fail */
		if (!PMSG_IS_AUTO(msg))
			status = 0;
	} else {
		dev_dbg(&udev->dev, "usb %ssuspend, wakeup %d\n",
				(PMSG_IS_AUTO(msg) ? "auto-" : ""),
				udev->do_remote_wakeup);
		if (really_suspend) {
			udev->port_is_suspended = 1;

			/* device has up to 10 msec to fully suspend */
			msleep(10);
		}
		usb_set_device_state(udev, USB_STATE_SUSPENDED);
	}

	if (status == 0 && !udev->do_remote_wakeup && udev->persist_enabled) {
		pm_runtime_put_sync(&port_dev->dev);
		port_dev->did_runtime_put = true;
	}

	usb_mark_last_busy(hub->hdev);
	return status;
}

static int finish_port_resume(struct usb_device *udev)
{
	int	status = 0;
	u16	devstatus = 0;

	
	dev_dbg(&udev->dev, "%s\n",
		udev->reset_resume ? "finish reset-resume" : "finish resume");

	usb_set_device_state(udev, udev->actconfig
			? USB_STATE_CONFIGURED
			: USB_STATE_ADDRESS);

	if (udev->reset_resume)
 retry_reset_resume:
		status = usb_reset_and_verify_device(udev);

	if (status == 0) {
		devstatus = 0;
		status = usb_get_status(udev, USB_RECIP_DEVICE, 0, &devstatus);
		if (status >= 0)
			status = (status > 0 ? 0 : -ENODEV);

		
		if (status && !udev->reset_resume && udev->persist_enabled) {
			dev_dbg(&udev->dev, "retry with reset-resume\n");
			udev->reset_resume = 1;
			goto retry_reset_resume;
		}
	}

	if (status) {
		dev_dbg(&udev->dev, "gone after usb resume? status %d\n",
				status);
	} else if (udev->actconfig && !udev->reset_resume) {
		if (!hub_is_superspeed(udev->parent)) {
			le16_to_cpus(&devstatus);
			if (devstatus & (1 << USB_DEVICE_REMOTE_WAKEUP))
				status = usb_control_msg(udev,
						usb_sndctrlpipe(udev, 0),
						USB_REQ_CLEAR_FEATURE,
						USB_RECIP_DEVICE,
						USB_DEVICE_REMOTE_WAKEUP, 0,
						NULL, 0,
						USB_CTRL_SET_TIMEOUT);
		} else {
			status = usb_get_status(udev, USB_RECIP_INTERFACE, 0,
					&devstatus);
			le16_to_cpus(&devstatus);
			if (!status && devstatus & (USB_INTRF_STAT_FUNC_RW_CAP
					| USB_INTRF_STAT_FUNC_RW))
				status =
					usb_disable_function_remotewakeup(udev);
		}

		if (status)
			dev_dbg(&udev->dev,
				"disable remote wakeup, status %d\n",
				status);
		status = 0;
	}
	return status;
}

int usb_port_resume(struct usb_device *udev, pm_message_t msg)
{
	struct usb_hub	*hub = usb_hub_to_struct_hub(udev->parent);
	struct usb_port *port_dev;
	int		port1 = udev->portnum;
	int		status;
	u16		portchange, portstatus;

	if (!hub)
		return -ENODEV;

	port_dev = hub->ports[udev->portnum - 1];
	if (port_dev->did_runtime_put) {
		status = pm_runtime_get_sync(&port_dev->dev);
		port_dev->did_runtime_put = false;
		if (status < 0) {
			dev_dbg(&udev->dev, "can't resume usb port, status %d\n",
					status);
			return status;
		}
	}

	/* Skip the initial Clear-Suspend step for a remote wakeup */
	status = hub_port_status(hub, port1, &portstatus, &portchange);
	if (status == 0 && !port_is_suspended(hub, portstatus))
		goto SuspendCleared;

	// dev_dbg(hub->intfdev, "resume port %d\n", port1);

	set_bit(port1, hub->busy_bits);

	/* see 7.1.7.7; affects power usage, but not budgeting */
	if (hub_is_superspeed(hub->hdev))
		status = hub_set_port_link_state(hub, port1, USB_SS_PORT_LS_U0);
	else
		status = usb_clear_port_feature(hub->hdev,
				port1, USB_PORT_FEAT_SUSPEND);
	if (status) {
		dev_dbg(hub->intfdev, "can't resume port %d, status %d\n",
				port1, status);
	} else {
		
		dev_dbg(&udev->dev, "usb %sresume\n",
				(PMSG_IS_AUTO(msg) ? "auto-" : ""));
		msleep(25);

		status = hub_port_status(hub, port1, &portstatus, &portchange);

		
		msleep(10);
	}

 SuspendCleared:
	if (status == 0) {
		udev->port_is_suspended = 0;
		if (hub_is_superspeed(hub->hdev)) {
			if (portchange & USB_PORT_STAT_C_LINK_STATE)
				usb_clear_port_feature(hub->hdev, port1,
					USB_PORT_FEAT_C_PORT_LINK_STATE);
		} else {
			if (portchange & USB_PORT_STAT_C_SUSPEND)
				usb_clear_port_feature(hub->hdev, port1,
						USB_PORT_FEAT_C_SUSPEND);
		}
	}

	clear_bit(port1, hub->busy_bits);

	status = check_port_resume_type(udev,
			hub, port1, status, portchange, portstatus);
	if (status == 0)
		status = finish_port_resume(udev);
	if (status < 0) {
		dev_dbg(&udev->dev, "can't resume, status %d\n", status);
		hub_port_logical_disconnect(hub, port1);
	} else  {
		/* Try to enable USB2 hardware LPM */
		if (udev->usb2_hw_lpm_capable == 1)
			usb_set_usb2_hardware_lpm(udev, 1);

		/* Try to enable USB3 LTM and LPM */
		usb_enable_ltm(udev);
		usb_unlocked_enable_lpm(udev);
	}

	return status;
}

#endif	

#ifdef	CONFIG_PM_RUNTIME

int usb_remote_wakeup(struct usb_device *udev)
{
	int	status = 0;
	struct usb_hcd *hcd = bus_to_hcd(udev->bus);

	if (udev->state == USB_STATE_SUSPENDED) {
		dev_dbg(&udev->dev, "usb %sresume\n", "wakeup-");
		status = usb_autoresume_device(udev);
		if (status == 0) {
			/* Let the drivers do their thing, then... */
			usb_autosuspend_device(udev);
		}
	} else {
		dev_dbg(&udev->dev, "usb not suspended\n");
		clear_bit(HCD_FLAG_WAKEUP_PENDING, &hcd->flags);
	}

	return status;
}

#endif

static int check_ports_changed(struct usb_hub *hub)
{
	int port1;

	for (port1 = 1; port1 <= hub->hdev->maxchild; ++port1) {
		u16 portstatus, portchange;
		int status;

		status = hub_port_status(hub, port1, &portstatus, &portchange);
		if (!status && portchange)
			return 1;
	}
	return 0;
}

static int hub_suspend(struct usb_interface *intf, pm_message_t msg)
{
	struct usb_hub		*hub = usb_get_intfdata (intf);
	struct usb_device	*hdev = hub->hdev;
	unsigned		port1;
	int			status;

	hub->wakeup_enabled_descendants = 0;
	for (port1 = 1; port1 <= hdev->maxchild; port1++) {
		struct usb_device	*udev;

		udev = hub->ports[port1 - 1]->child;
		if (udev && udev->can_submit) {
			dev_warn(&intf->dev, "port %d nyet suspended\n", port1);
			if (PMSG_IS_AUTO(msg))
				return -EBUSY;
		}
		if (udev)
			hub->wakeup_enabled_descendants +=
					wakeup_enabled_descendants(udev);
	}

	if (hdev->do_remote_wakeup && hub->quirk_check_port_auto_suspend) {
		/* check if there are changes pending on hub ports */
		if (check_ports_changed(hub)) {
			if (PMSG_IS_AUTO(msg))
				return -EBUSY;
			pm_wakeup_event(&hdev->dev, 2000);
		}
	}

	if (hub_is_superspeed(hdev) && hdev->do_remote_wakeup) {
		/* Enable hub to send remote wakeup for all ports. */
		for (port1 = 1; port1 <= hdev->maxchild; port1++) {
			status = set_port_feature(hdev,
					port1 |
					USB_PORT_FEAT_REMOTE_WAKE_CONNECT |
					USB_PORT_FEAT_REMOTE_WAKE_DISCONNECT |
					USB_PORT_FEAT_REMOTE_WAKE_OVER_CURRENT,
					USB_PORT_FEAT_REMOTE_WAKE_MASK);
		}
	}

	dev_dbg(&intf->dev, "%s\n", __func__);

	/* stop khubd and related activity */
	hub_quiesce(hub, HUB_SUSPEND);
	return 0;
}

static int hub_resume(struct usb_interface *intf)
{
	struct usb_hub *hub = usb_get_intfdata(intf);

	dev_dbg(&intf->dev, "%s\n", __func__);
	hub_activate(hub, HUB_RESUME);
	return 0;
}

static int hub_reset_resume(struct usb_interface *intf)
{
	struct usb_hub *hub = usb_get_intfdata(intf);

	dev_dbg(&intf->dev, "%s\n", __func__);
	hub_activate(hub, HUB_RESET_RESUME);
	return 0;
}

void usb_root_hub_lost_power(struct usb_device *rhdev)
{
	dev_warn(&rhdev->dev, "root hub lost power or was reset\n");
	rhdev->reset_resume = 1;
}
EXPORT_SYMBOL_GPL(usb_root_hub_lost_power);

static const char * const usb3_lpm_names[]  = {
	"U0",
	"U1",
	"U2",
	"U3",
};

static int usb_req_set_sel(struct usb_device *udev, enum usb3_link_state state)
{
	struct usb_set_sel_req *sel_values;
	unsigned long long u1_sel;
	unsigned long long u1_pel;
	unsigned long long u2_sel;
	unsigned long long u2_pel;
	int ret;

	
	u1_sel = DIV_ROUND_UP(udev->u1_params.sel, 1000);
	u1_pel = DIV_ROUND_UP(udev->u1_params.pel, 1000);
	u2_sel = DIV_ROUND_UP(udev->u2_params.sel, 1000);
	u2_pel = DIV_ROUND_UP(udev->u2_params.pel, 1000);

	if ((state == USB3_LPM_U1 &&
				(u1_sel > USB3_LPM_MAX_U1_SEL_PEL ||
				 u1_pel > USB3_LPM_MAX_U1_SEL_PEL)) ||
			(state == USB3_LPM_U2 &&
			 (u2_sel > USB3_LPM_MAX_U2_SEL_PEL ||
			  u2_pel > USB3_LPM_MAX_U2_SEL_PEL))) {
		dev_dbg(&udev->dev, "Device-initiated %s disabled due to long SEL %llu us or PEL %llu us\n",
				usb3_lpm_names[state], u1_sel, u1_pel);
		return -EINVAL;
	}

	if (u1_sel > USB3_LPM_MAX_U1_SEL_PEL)
		u1_sel = USB3_LPM_MAX_U1_SEL_PEL;

	if (u1_pel > USB3_LPM_MAX_U1_SEL_PEL)
		u1_pel = USB3_LPM_MAX_U1_SEL_PEL;

	if (u2_sel > USB3_LPM_MAX_U2_SEL_PEL)
		u2_sel = USB3_LPM_MAX_U2_SEL_PEL;

	if (u2_pel > USB3_LPM_MAX_U2_SEL_PEL)
		u2_pel = USB3_LPM_MAX_U2_SEL_PEL;

	sel_values = kmalloc(sizeof *(sel_values), GFP_NOIO);
	if (!sel_values)
		return -ENOMEM;

	sel_values->u1_sel = u1_sel;
	sel_values->u1_pel = u1_pel;
	sel_values->u2_sel = cpu_to_le16(u2_sel);
	sel_values->u2_pel = cpu_to_le16(u2_pel);

	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			USB_REQ_SET_SEL,
			USB_RECIP_DEVICE,
			0, 0,
			sel_values, sizeof *(sel_values),
			USB_CTRL_SET_TIMEOUT);
	kfree(sel_values);
	return ret;
}

static int usb_set_device_initiated_lpm(struct usb_device *udev,
		enum usb3_link_state state, bool enable)
{
	int ret;
	int feature;

	switch (state) {
	case USB3_LPM_U1:
		feature = USB_DEVICE_U1_ENABLE;
		break;
	case USB3_LPM_U2:
		feature = USB_DEVICE_U2_ENABLE;
		break;
	default:
		dev_warn(&udev->dev, "%s: Can't %s non-U1 or U2 state.\n",
				__func__, enable ? "enable" : "disable");
		return -EINVAL;
	}

	if (udev->state != USB_STATE_CONFIGURED) {
		dev_dbg(&udev->dev, "%s: Can't %s %s state "
				"for unconfigured device.\n",
				__func__, enable ? "enable" : "disable",
				usb3_lpm_names[state]);
		return 0;
	}

	if (enable) {
		ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
				USB_REQ_SET_FEATURE,
				USB_RECIP_DEVICE,
				feature,
				0, NULL, 0,
				USB_CTRL_SET_TIMEOUT);
	} else {
		ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
				USB_REQ_CLEAR_FEATURE,
				USB_RECIP_DEVICE,
				feature,
				0, NULL, 0,
				USB_CTRL_SET_TIMEOUT);
	}
	if (ret < 0) {
		dev_warn(&udev->dev, "%s of device-initiated %s failed.\n",
				enable ? "Enable" : "Disable",
				usb3_lpm_names[state]);
		return -EBUSY;
	}
	return 0;
}

static int usb_set_lpm_timeout(struct usb_device *udev,
		enum usb3_link_state state, int timeout)
{
	int ret;
	int feature;

	switch (state) {
	case USB3_LPM_U1:
		feature = USB_PORT_FEAT_U1_TIMEOUT;
		break;
	case USB3_LPM_U2:
		feature = USB_PORT_FEAT_U2_TIMEOUT;
		break;
	default:
		dev_warn(&udev->dev, "%s: Can't set timeout for non-U1 or U2 state.\n",
				__func__);
		return -EINVAL;
	}

	if (state == USB3_LPM_U1 && timeout > USB3_LPM_U1_MAX_TIMEOUT &&
			timeout != USB3_LPM_DEVICE_INITIATED) {
		dev_warn(&udev->dev, "Failed to set %s timeout to 0x%x, "
				"which is a reserved value.\n",
				usb3_lpm_names[state], timeout);
		return -EINVAL;
	}

	ret = set_port_feature(udev->parent,
			USB_PORT_LPM_TIMEOUT(timeout) | udev->portnum,
			feature);
	if (ret < 0) {
		dev_warn(&udev->dev, "Failed to set %s timeout to 0x%x,"
				"error code %i\n", usb3_lpm_names[state],
				timeout, ret);
		return -EBUSY;
	}
	if (state == USB3_LPM_U1)
		udev->u1_params.timeout = timeout;
	else
		udev->u2_params.timeout = timeout;
	return 0;
}

static void usb_enable_link_state(struct usb_hcd *hcd, struct usb_device *udev,
		enum usb3_link_state state)
{
	int timeout, ret;
	__u8 u1_mel = udev->bos->ss_cap->bU1devExitLat;
	__le16 u2_mel = udev->bos->ss_cap->bU2DevExitLat;

	if ((state == USB3_LPM_U1 && u1_mel == 0) ||
			(state == USB3_LPM_U2 && u2_mel == 0))
		return;

	ret = usb_req_set_sel(udev, state);
	if (ret < 0) {
		dev_warn(&udev->dev, "Set SEL for device-initiated %s failed.\n",
				usb3_lpm_names[state]);
		return;
	}

	timeout = hcd->driver->enable_usb3_lpm_timeout(hcd, udev, state);

	
	if (timeout == 0)
		return;

	if (timeout < 0) {
		dev_warn(&udev->dev, "Could not enable %s link state, "
				"xHCI error %i.\n", usb3_lpm_names[state],
				timeout);
		return;
	}

	if (usb_set_lpm_timeout(udev, state, timeout))
		hcd->driver->disable_usb3_lpm_timeout(hcd, udev, state);

	
	else if (udev->actconfig)
		usb_set_device_initiated_lpm(udev, state, true);

}

static int usb_disable_link_state(struct usb_hcd *hcd, struct usb_device *udev,
		enum usb3_link_state state)
{
	int feature;

	switch (state) {
	case USB3_LPM_U1:
		feature = USB_PORT_FEAT_U1_TIMEOUT;
		break;
	case USB3_LPM_U2:
		feature = USB_PORT_FEAT_U2_TIMEOUT;
		break;
	default:
		dev_warn(&udev->dev, "%s: Can't disable non-U1 or U2 state.\n",
				__func__);
		return -EINVAL;
	}

	if (usb_set_lpm_timeout(udev, state, 0))
		return -EBUSY;

	usb_set_device_initiated_lpm(udev, state, false);

	if (hcd->driver->disable_usb3_lpm_timeout(hcd, udev, state))
		dev_warn(&udev->dev, "Could not disable xHCI %s timeout, "
				"bus schedule bandwidth may be impacted.\n",
				usb3_lpm_names[state]);
	return 0;
}

int usb_disable_lpm(struct usb_device *udev)
{
	struct usb_hcd *hcd;

	if (!udev || !udev->parent ||
			udev->speed != USB_SPEED_SUPER ||
			!udev->lpm_capable)
		return 0;

	hcd = bus_to_hcd(udev->bus);
	if (!hcd || !hcd->driver->disable_usb3_lpm_timeout)
		return 0;

	udev->lpm_disable_count++;
	if ((udev->u1_params.timeout == 0 && udev->u2_params.timeout == 0))
		return 0;

	/* If LPM is enabled, attempt to disable it. */
	if (usb_disable_link_state(hcd, udev, USB3_LPM_U1))
		goto enable_lpm;
	if (usb_disable_link_state(hcd, udev, USB3_LPM_U2))
		goto enable_lpm;

	return 0;

enable_lpm:
	usb_enable_lpm(udev);
	return -EBUSY;
}
EXPORT_SYMBOL_GPL(usb_disable_lpm);

int usb_unlocked_disable_lpm(struct usb_device *udev)
{
	struct usb_hcd *hcd = bus_to_hcd(udev->bus);
	int ret;

	if (!hcd)
		return -EINVAL;

	mutex_lock(hcd->bandwidth_mutex);
	ret = usb_disable_lpm(udev);
	mutex_unlock(hcd->bandwidth_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(usb_unlocked_disable_lpm);

void usb_enable_lpm(struct usb_device *udev)
{
	struct usb_hcd *hcd;

	if (!udev || !udev->parent ||
			udev->speed != USB_SPEED_SUPER ||
			!udev->lpm_capable)
		return;

	udev->lpm_disable_count--;
	hcd = bus_to_hcd(udev->bus);
	if (!hcd || !hcd->driver->enable_usb3_lpm_timeout ||
			!hcd->driver->disable_usb3_lpm_timeout)
		return;

	if (udev->lpm_disable_count > 0)
		return;

	usb_enable_link_state(hcd, udev, USB3_LPM_U1);
	usb_enable_link_state(hcd, udev, USB3_LPM_U2);
}
EXPORT_SYMBOL_GPL(usb_enable_lpm);

void usb_unlocked_enable_lpm(struct usb_device *udev)
{
	struct usb_hcd *hcd = bus_to_hcd(udev->bus);

	if (!hcd)
		return;

	mutex_lock(hcd->bandwidth_mutex);
	usb_enable_lpm(udev);
	mutex_unlock(hcd->bandwidth_mutex);
}
EXPORT_SYMBOL_GPL(usb_unlocked_enable_lpm);


#else	/* CONFIG_PM */

#define hub_suspend		NULL
#define hub_resume		NULL
#define hub_reset_resume	NULL

int usb_disable_lpm(struct usb_device *udev)
{
	return 0;
}
EXPORT_SYMBOL_GPL(usb_disable_lpm);

void usb_enable_lpm(struct usb_device *udev) { }
EXPORT_SYMBOL_GPL(usb_enable_lpm);

int usb_unlocked_disable_lpm(struct usb_device *udev)
{
	return 0;
}
EXPORT_SYMBOL_GPL(usb_unlocked_disable_lpm);

void usb_unlocked_enable_lpm(struct usb_device *udev) { }
EXPORT_SYMBOL_GPL(usb_unlocked_enable_lpm);

int usb_disable_ltm(struct usb_device *udev)
{
	return 0;
}
EXPORT_SYMBOL_GPL(usb_disable_ltm);

void usb_enable_ltm(struct usb_device *udev) { }
EXPORT_SYMBOL_GPL(usb_enable_ltm);
#endif

/* USB 2.0 spec, 7.1.7.3 / fig 7-29:
 *
 * Between connect detection and reset signaling there must be a delay
 * of 100ms at least for debounce and power-settling.  The corresponding
 * timer shall restart whenever the downstream port detects a disconnect.
 * 
 * Apparently there are some bluetooth and irda-dongles and a number of
 * low-speed devices for which this debounce period may last over a second.
 * Not covered by the spec - but easy to deal with.
 *
 * This implementation uses a 1500ms total debounce timeout; if the
 * connection isn't stable by then it returns -ETIMEDOUT.  It checks
 * every 25ms for transient disconnects.  When the port status has been
 * unchanged for 100ms it returns the port status.
 */
int hub_port_debounce(struct usb_hub *hub, int port1, bool must_be_connected)
{
	int ret;
	int total_time, stable_time = 0;
	u16 portchange, portstatus;
	unsigned connection = 0xffff;

	for (total_time = 0; ; total_time += HUB_DEBOUNCE_STEP) {
		ret = hub_port_status(hub, port1, &portstatus, &portchange);
		if (ret < 0)
			return ret;

		if (!(portchange & USB_PORT_STAT_C_CONNECTION) &&
		     (portstatus & USB_PORT_STAT_CONNECTION) == connection) {
			if (!must_be_connected ||
			     (connection == USB_PORT_STAT_CONNECTION))
				stable_time += HUB_DEBOUNCE_STEP;
			if (stable_time >= HUB_DEBOUNCE_STABLE)
				break;
		} else {
			stable_time = 0;
			connection = portstatus & USB_PORT_STAT_CONNECTION;
		}

		if (portchange & USB_PORT_STAT_C_CONNECTION) {
			usb_clear_port_feature(hub->hdev, port1,
					USB_PORT_FEAT_C_CONNECTION);
		}

		if (total_time >= HUB_DEBOUNCE_TIMEOUT)
			break;
		msleep(HUB_DEBOUNCE_STEP);
	}

	dev_dbg (hub->intfdev,
		"debounce: port %d: total %dms stable %dms status 0x%x\n",
		port1, total_time, stable_time, portstatus);

	if (stable_time < HUB_DEBOUNCE_STABLE)
		return -ETIMEDOUT;
	return portstatus;
}

void usb_ep0_reinit(struct usb_device *udev)
{
	usb_disable_endpoint(udev, 0 + USB_DIR_IN, true);
	usb_disable_endpoint(udev, 0 + USB_DIR_OUT, true);
	usb_enable_endpoint(udev, &udev->ep0, true);
}
EXPORT_SYMBOL_GPL(usb_ep0_reinit);

#define usb_sndaddr0pipe()	(PIPE_CONTROL << 30)
#define usb_rcvaddr0pipe()	((PIPE_CONTROL << 30) | USB_DIR_IN)

static int hub_set_address(struct usb_device *udev, int devnum)
{
	int retval;
	struct usb_hcd *hcd = bus_to_hcd(udev->bus);

	if (!hcd->driver->address_device && devnum <= 1)
		return -EINVAL;
	if (udev->state == USB_STATE_ADDRESS)
		return 0;
	if (udev->state != USB_STATE_DEFAULT)
		return -EINVAL;
	if (hcd->driver->address_device)
		retval = hcd->driver->address_device(hcd, udev);
	else
		retval = usb_control_msg(udev, usb_sndaddr0pipe(),
				USB_REQ_SET_ADDRESS, 0, devnum, 0,
				NULL, 0, USB_CTRL_SET_TIMEOUT);
	if (retval == 0) {
		update_devnum(udev, devnum);
		
		usb_set_device_state(udev, USB_STATE_ADDRESS);
		usb_ep0_reinit(udev);
	}
	return retval;
}

static int
hub_port_init (struct usb_hub *hub, struct usb_device *udev, int port1,
		int retry_counter)
{
	static DEFINE_MUTEX(usb_address0_mutex);

	struct usb_device	*hdev = hub->hdev;
	struct usb_hcd		*hcd = bus_to_hcd(hdev->bus);
	int			i, j, retval;
	unsigned		delay = HUB_SHORT_RESET_TIME;
	enum usb_device_speed	oldspeed = udev->speed;
	const char		*speed;
	int			devnum = udev->devnum;

	if (!hdev->parent) {
		delay = HUB_ROOT_RESET_TIME;
		if (port1 == hdev->bus->otg_port)
			hdev->bus->b_hnp_enable = 0;
	}

	
	
	if (oldspeed == USB_SPEED_LOW)
		delay = HUB_LONG_RESET_TIME;

	mutex_lock(&usb_address0_mutex);

	
	
	retval = hub_port_reset(hub, port1, udev, delay, false);
	if (retval < 0)		
		goto fail;
	

	retval = -ENODEV;

	if (oldspeed != USB_SPEED_UNKNOWN && oldspeed != udev->speed) {
		dev_dbg(&udev->dev, "device reset changed speed!\n");
		goto fail;
	}
	oldspeed = udev->speed;

	switch (udev->speed) {
	case USB_SPEED_SUPER:
	case USB_SPEED_WIRELESS:	
		udev->ep0.desc.wMaxPacketSize = cpu_to_le16(512);
		break;
	case USB_SPEED_HIGH:		
		udev->ep0.desc.wMaxPacketSize = cpu_to_le16(64);
		break;
	case USB_SPEED_FULL:		
		udev->ep0.desc.wMaxPacketSize = cpu_to_le16(64);
		break;
	case USB_SPEED_LOW:		
		udev->ep0.desc.wMaxPacketSize = cpu_to_le16(8);
		break;
	default:
		goto fail;
	}

	if (udev->speed == USB_SPEED_WIRELESS)
		speed = "variable speed Wireless";
	else
		speed = usb_speed_string(udev->speed);

	if (udev->speed != USB_SPEED_SUPER)
		dev_info(&udev->dev,
				"%s %s USB device number %d using %s\n",
				(udev->config) ? "reset" : "new", speed,
				devnum, udev->bus->controller->driver->name);

	/* Set up TT records, if needed  */
	if (hdev->tt) {
		udev->tt = hdev->tt;
		udev->ttport = hdev->ttport;
	} else if (udev->speed != USB_SPEED_HIGH
			&& hdev->speed == USB_SPEED_HIGH) {
		if (!hub->tt.hub) {
			dev_err(&udev->dev, "parent hub has no TT\n");
			retval = -EINVAL;
			goto fail;
		}
		udev->tt = &hub->tt;
		udev->ttport = port1;
	}
 
	for (i = 0; i < GET_DESCRIPTOR_TRIES; (++i, msleep(100))) {
		if (USE_NEW_SCHEME(retry_counter) &&
			!(hcd->driver->flags & HCD_USB3) &&
			!((hcd->driver->flags & HCD_RT_OLD_ENUM) &&
				!hdev->parent)) {
			struct usb_device_descriptor *buf;
			ushort idvendor;
			int r = 0;

#define GET_DESCRIPTOR_BUFSIZE	64
			buf = kmalloc(GET_DESCRIPTOR_BUFSIZE, GFP_NOIO);
			if (!buf) {
				retval = -ENOMEM;
				continue;
			}

			for (j = 0; j < 3; ++j) {
				buf->bMaxPacketSize0 = 0;
				r = usb_control_msg(udev, usb_rcvaddr0pipe(),
					USB_REQ_GET_DESCRIPTOR, USB_DIR_IN,
					USB_DT_DEVICE << 8, 0,
					buf, GET_DESCRIPTOR_BUFSIZE,
					initial_descriptor_timeout);
				switch (buf->bMaxPacketSize0) {
				case 8: case 16: case 32: case 64: case 255:
					if (buf->bDescriptorType ==
							USB_DT_DEVICE) {
						r = 0;
						break;
					}
					/* FALL THROUGH */
				default:
					if (r == 0)
						r = -EPROTO;
					break;
				}
				if (r == 0)
					break;
			}
			udev->descriptor.bMaxPacketSize0 =
					buf->bMaxPacketSize0;
			idvendor = le16_to_cpu(buf->idVendor);
			kfree(buf);

			if (idvendor != 0x1a0a) {
				retval = hub_port_reset(hub, port1, udev,
							 delay, false);
				if (retval < 0)	
					goto fail;
				if (oldspeed != udev->speed) {
					dev_dbg(&udev->dev,
					       "device reset changed speed!\n");
					retval = -ENODEV;
					goto fail;
				}
			}
			if (r) {
				if (r != -ENODEV)
					dev_err(&udev->dev, "device descriptor read/64, error %d\n",
							r);
				retval = -EMSGSIZE;
				continue;
			}
#undef GET_DESCRIPTOR_BUFSIZE
		}

		if (udev->wusb == 0) {
			for (j = 0; j < SET_ADDRESS_TRIES; ++j) {
				retval = hub_set_address(udev, devnum);
				if (retval >= 0)
					break;
				msleep(200);
			}
			if (retval < 0) {
				if (retval != -ENODEV)
					dev_err(&udev->dev, "device not accepting address %d, error %d\n",
							devnum, retval);
				goto fail;
			}
			if (udev->speed == USB_SPEED_SUPER) {
				devnum = udev->devnum;
				dev_info(&udev->dev,
						"%s SuperSpeed USB device number %d using %s\n",
						(udev->config) ? "reset" : "new",
						devnum, udev->bus->controller->driver->name);
			}

			msleep(10);
			if (USE_NEW_SCHEME(retry_counter) &&
				!(hcd->driver->flags & HCD_USB3) &&
				!((hcd->driver->flags & HCD_RT_OLD_ENUM) &&
					!hdev->parent))
				break;
  		}

		retval = usb_get_device_descriptor(udev, 8);
		if (retval < 8) {
			if (retval != -ENODEV)
				dev_err(&udev->dev,
					"device descriptor read/8, error %d\n",
					retval);
			if (retval >= 0)
				retval = -EMSGSIZE;
		} else {
			retval = 0;
			break;
		}
	}
	if (retval)
		goto fail;

	if (hcd->phy && !hdev->parent)
		usb_phy_notify_connect(hcd->phy, udev->speed);

	if ((udev->speed == USB_SPEED_SUPER) &&
			(le16_to_cpu(udev->descriptor.bcdUSB) < 0x0300)) {
		dev_err(&udev->dev, "got a wrong device descriptor, "
				"warm reset device\n");
		hub_port_reset(hub, port1, udev,
				HUB_BH_RESET_TIME, true);
		retval = -EINVAL;
		goto fail;
	}

	if (udev->descriptor.bMaxPacketSize0 == 0xff ||
			udev->speed == USB_SPEED_SUPER)
		i = 512;
	else
		i = udev->descriptor.bMaxPacketSize0;
	if (usb_endpoint_maxp(&udev->ep0.desc) != i) {
		if (udev->speed == USB_SPEED_LOW ||
				!(i == 8 || i == 16 || i == 32 || i == 64)) {
			dev_err(&udev->dev, "Invalid ep0 maxpacket: %d\n", i);
			retval = -EMSGSIZE;
			goto fail;
		}
		if (udev->speed == USB_SPEED_FULL)
			dev_dbg(&udev->dev, "ep0 maxpacket = %d\n", i);
		else
			dev_warn(&udev->dev, "Using ep0 maxpacket: %d\n", i);
		udev->ep0.desc.wMaxPacketSize = cpu_to_le16(i);
		usb_ep0_reinit(udev);
	}
  
	retval = usb_get_device_descriptor(udev, USB_DT_DEVICE_SIZE);
	if (retval < (signed)sizeof(udev->descriptor)) {
		if (retval != -ENODEV)
			dev_err(&udev->dev, "device descriptor read/all, error %d\n",
					retval);
		if (retval >= 0)
			retval = -ENOMSG;
		goto fail;
	}

	if (udev->wusb == 0 && le16_to_cpu(udev->descriptor.bcdUSB) >= 0x0201) {
		retval = usb_get_bos_descriptor(udev);
		if (!retval) {
			udev->lpm_capable = usb_device_supports_lpm(udev);
			usb_set_lpm_parameters(udev);
		}
	}

	retval = 0;
	/* notify HCD that we have a device connected and addressed */
	if (hcd->driver->update_device)
		hcd->driver->update_device(hcd, udev);
fail:
	if (retval) {
		hub_port_disable(hub, port1, 0);
		update_devnum(udev, devnum);	/* for disconnect processing */
	}
	mutex_unlock(&usb_address0_mutex);
	return retval;
}

static void
check_highspeed (struct usb_hub *hub, struct usb_device *udev, int port1)
{
	struct usb_qualifier_descriptor	*qual;
	int				status;

	qual = kmalloc (sizeof *qual, GFP_KERNEL);
	if (qual == NULL)
		return;

	status = usb_get_descriptor (udev, USB_DT_DEVICE_QUALIFIER, 0,
			qual, sizeof *qual);
	if (status == sizeof *qual) {
		dev_info(&udev->dev, "not running at top speed; "
			"connect to a high speed hub\n");
		/* hub LEDs are probably harder to miss than syslog */
		if (hub->has_indicators) {
			hub->indicator[port1-1] = INDICATOR_GREEN_BLINK;
			schedule_delayed_work (&hub->leds, 0);
		}
	}
	kfree(qual);
}

static unsigned
hub_power_remaining (struct usb_hub *hub)
{
	struct usb_device *hdev = hub->hdev;
	int remaining;
	int port1;

	if (!hub->limited_power)
		return 0;

	remaining = hdev->bus_mA - hub->descriptor->bHubContrCurrent;
	for (port1 = 1; port1 <= hdev->maxchild; ++port1) {
		struct usb_device	*udev = hub->ports[port1 - 1]->child;
		int			delta;
		unsigned		unit_load;

		if (!udev)
			continue;
		if (hub_is_superspeed(udev))
			unit_load = 150;
		else
			unit_load = 100;

		if (udev->actconfig)
			delta = usb_get_max_power(udev, udev->actconfig);
		else if (port1 != udev->bus->otg_port || hdev->parent)
			delta = unit_load;
		else
			delta = 8;
		if (delta > hub->mA_per_port)
			dev_warn(&udev->dev,
				 "%dmA is over %umA budget for port %d!\n",
				 delta, hub->mA_per_port, port1);
		remaining -= delta;
	}
	if (remaining < 0) {
		dev_warn(hub->intfdev, "%dmA over power budget!\n",
			- remaining);
		remaining = 0;
	}
	return remaining;
}

static void hub_port_connect_change(struct usb_hub *hub, int port1,
					u16 portstatus, u16 portchange)
{
	struct usb_device *hdev = hub->hdev;
	struct device *hub_dev = hub->intfdev;
	struct usb_hcd *hcd = bus_to_hcd(hdev->bus);
	unsigned wHubCharacteristics =
			le16_to_cpu(hub->descriptor->wHubCharacteristics);
	struct usb_device *udev;
	int status, i;
	unsigned unit_load;

	dev_dbg (hub_dev,
		"port %d, status %04x, change %04x, %s\n",
		port1, portstatus, portchange, portspeed(hub, portstatus));

	if (hub->has_indicators) {
		set_port_led(hub, port1, HUB_LED_AUTO);
		hub->indicator[port1-1] = INDICATOR_AUTO;
	}

#ifdef	CONFIG_USB_OTG
	
	if (hdev->bus->is_b_host)
		portchange &= ~(USB_PORT_STAT_C_CONNECTION |
				USB_PORT_STAT_C_ENABLE);
#endif

	
	udev = hub->ports[port1 - 1]->child;
	if ((portstatus & USB_PORT_STAT_CONNECTION) && udev &&
			udev->state != USB_STATE_NOTATTACHED) {
		usb_lock_device(udev);
		if (portstatus & USB_PORT_STAT_ENABLE) {
			status = 0;		

#ifdef CONFIG_PM_RUNTIME
		} else if (udev->state == USB_STATE_SUSPENDED &&
				udev->persist_enabled) {
			status = usb_remote_wakeup(udev);
#endif

		} else {
			status = -ENODEV;	
		}
		usb_unlock_device(udev);

		if (status == 0) {
			clear_bit(port1, hub->change_bits);
			return;
		}
	}

	/* Disconnect any existing devices under this port */
	if (udev) {
		if (hcd->phy && !hdev->parent &&
				!(portstatus & USB_PORT_STAT_CONNECTION))
			usb_phy_notify_disconnect(hcd->phy, udev->speed);
		usb_disconnect(&hub->ports[port1 - 1]->child);
	}
	clear_bit(port1, hub->change_bits);

	if (!(portstatus & USB_PORT_STAT_CONNECTION) ||
			(portchange & USB_PORT_STAT_C_CONNECTION))
		clear_bit(port1, hub->removed_bits);

	if (portchange & (USB_PORT_STAT_C_CONNECTION |
				USB_PORT_STAT_C_ENABLE)) {
		status = hub_port_debounce_be_stable(hub, port1);
		if (status < 0) {
			if (status != -ENODEV && printk_ratelimit())
				dev_err(hub_dev, "connect-debounce failed, "
						"port %d disabled\n", port1);
			portstatus &= ~USB_PORT_STAT_CONNECTION;
		} else {
			portstatus = status;
		}
	}

	if (!(portstatus & USB_PORT_STAT_CONNECTION) ||
			test_bit(port1, hub->removed_bits)) {

		
		if ((wHubCharacteristics & HUB_CHAR_LPSM) < 2
				&& !port_is_power_on(hub, portstatus))
			set_port_feature(hdev, port1, USB_PORT_FEAT_POWER);

		if (portstatus & USB_PORT_STAT_ENABLE)
  			goto done;
		return;
	}
	if (hub_is_superspeed(hub->hdev))
		unit_load = 150;
	else
		unit_load = 100;

	status = 0;
	for (i = 0; i < SET_CONFIG_TRIES; i++) {

		udev = usb_alloc_dev(hdev, hdev->bus, port1);
		if (!udev) {
			dev_err (hub_dev,
				"couldn't allocate port %d usb_device\n",
				port1);
			goto done;
		}

		usb_set_device_state(udev, USB_STATE_POWERED);
 		udev->bus_mA = hub->mA_per_port;
		udev->level = hdev->level + 1;
		udev->wusb = hub_is_wusb(hub);

		/* Only USB 3.0 devices are connected to SuperSpeed hubs. */
		if (hub_is_superspeed(hub->hdev))
			udev->speed = USB_SPEED_SUPER;
		else
			udev->speed = USB_SPEED_UNKNOWN;

		choose_devnum(udev);
		if (udev->devnum <= 0) {
			status = -ENOTCONN;	/* Don't retry */
			goto loop;
		}

		/* reset (non-USB 3.0 devices) and get descriptor */
		status = hub_port_init(hub, udev, port1, i);
		if (status < 0)
			goto loop;

		usb_detect_quirks(udev);
		if (udev->quirks & USB_QUIRK_DELAY_INIT)
			msleep(1000);

		if (udev->descriptor.bDeviceClass == USB_CLASS_HUB
				&& udev->bus_mA <= unit_load) {
			u16	devstat;

			status = usb_get_status(udev, USB_RECIP_DEVICE, 0,
					&devstat);
			if (status < 2) {
				dev_dbg(&udev->dev, "get status %d ?\n", status);
				goto loop_disable;
			}
			le16_to_cpus(&devstat);
			if ((devstat & (1 << USB_DEVICE_SELF_POWERED)) == 0) {
				dev_err(&udev->dev,
					"can't connect bus-powered hub "
					"to this port\n");
				if (hub->has_indicators) {
					hub->indicator[port1-1] =
						INDICATOR_AMBER_BLINK;
					schedule_delayed_work (&hub->leds, 0);
				}
				status = -ENOTCONN;	
				goto loop_disable;
			}
		}
 
		
		if (le16_to_cpu(udev->descriptor.bcdUSB) >= 0x0200
				&& udev->speed == USB_SPEED_FULL
				&& highspeed_hubs != 0)
			check_highspeed (hub, udev, port1);

		status = 0;

		spin_lock_irq(&device_state_lock);
		if (hdev->state == USB_STATE_NOTATTACHED)
			status = -ENOTCONN;
		else
			hub->ports[port1 - 1]->child = udev;
		spin_unlock_irq(&device_state_lock);

		/* Run it through the hoops (find a driver, etc) */
		if (!status) {
			status = usb_new_device(udev);
			if (status) {
				spin_lock_irq(&device_state_lock);
				hub->ports[port1 - 1]->child = NULL;
				spin_unlock_irq(&device_state_lock);
			}
		}

		if (status)
			goto loop_disable;

		status = hub_power_remaining(hub);
		if (status)
			dev_dbg(hub_dev, "%dmA power budget left\n", status);

		return;

loop_disable:
		hub_port_disable(hub, port1, 1);
loop:
		usb_ep0_reinit(udev);
		release_devnum(udev);
		hub_free_dev(udev);
		usb_put_dev(udev);
		if ((status == -ENOTCONN) || (status == -ENOTSUPP))
			break;
	}
	if (hub->hdev->parent ||
			!hcd->driver->port_handed_over ||
			!(hcd->driver->port_handed_over)(hcd, port1)) {
		if (status != -ENOTCONN && status != -ENODEV)
			dev_err(hub_dev, "unable to enumerate USB device on port %d\n",
					port1);
	}
 
done:
	hub_port_disable(hub, port1, 1);
	if (hcd->driver->relinquish_port && !hub->hdev->parent)
		hcd->driver->relinquish_port(hcd, port1);
}

static int hub_handle_remote_wakeup(struct usb_hub *hub, unsigned int port,
		u16 portstatus, u16 portchange)
{
	struct usb_device *hdev;
	struct usb_device *udev;
	int connect_change = 0;
	int ret;

	hdev = hub->hdev;
	udev = hub->ports[port - 1]->child;
	if (!hub_is_superspeed(hdev)) {
		if (!(portchange & USB_PORT_STAT_C_SUSPEND))
			return 0;
		usb_clear_port_feature(hdev, port, USB_PORT_FEAT_C_SUSPEND);
	} else {
		if (!udev || udev->state != USB_STATE_SUSPENDED ||
				 (portstatus & USB_PORT_STAT_LINK_STATE) !=
				 USB_SS_PORT_LS_U0)
			return 0;
	}

	if (udev) {
		/* TRSMRCY = 10 msec */
		msleep(10);

		usb_lock_device(udev);
		ret = usb_remote_wakeup(udev);
		usb_unlock_device(udev);
		if (ret < 0)
			connect_change = 1;
	} else {
		ret = -ENODEV;
		hub_port_disable(hub, port, 1);
	}
	dev_dbg(hub->intfdev, "resume on port %d, status %d\n",
			port, ret);
	return connect_change;
}

static void hub_events(void)
{
	struct list_head *tmp;
	struct usb_device *hdev;
	struct usb_interface *intf;
	struct usb_hub *hub;
	struct device *hub_dev;
	u16 hubstatus;
	u16 hubchange;
	u16 portstatus;
	u16 portchange;
	int i, ret;
	int connect_change, wakeup_change;

	while (1) {

		
		spin_lock_irq(&hub_event_lock);
		if (list_empty(&hub_event_list)) {
			spin_unlock_irq(&hub_event_lock);
			break;
		}

		tmp = hub_event_list.next;
		list_del_init(tmp);

		hub = list_entry(tmp, struct usb_hub, event_list);
		kref_get(&hub->kref);

		/* make sure hdev is not freed before accessing it */
		if (hub->disconnected) {
			spin_unlock_irq(&hub_event_lock);
			goto hub_disconnected;
		} else {
			usb_get_dev(hub->hdev);
		}
		spin_unlock_irq(&hub_event_lock);

		hdev = hub->hdev;
		hub_dev = hub->intfdev;
		intf = to_usb_interface(hub_dev);
		dev_dbg(hub_dev, "state %d ports %d chg %04x evt %04x\n",
				hdev->state, hub->descriptor
					? hub->descriptor->bNbrPorts
					: 0,
				
				(u16) hub->change_bits[0],
				(u16) hub->event_bits[0]);

		usb_lock_device(hdev);
		if (unlikely(hub->disconnected))
			goto loop_disconnected;

		
		if (hdev->state == USB_STATE_NOTATTACHED) {
			hub->error = -ENODEV;
			hub_quiesce(hub, HUB_DISCONNECT);
			goto loop;
		}

		
		ret = usb_autopm_get_interface(intf);
		if (ret) {
			dev_dbg(hub_dev, "Can't autoresume: %d\n", ret);
			goto loop;
		}

		
		if (hub->quiescing)
			goto loop_autopm;

		if (hub->error) {
			dev_dbg (hub_dev, "resetting for error %d\n",
				hub->error);

			ret = usb_reset_device(hdev);
			if (ret) {
				dev_dbg (hub_dev,
					"error resetting hub: %d\n", ret);
				goto loop_autopm;
			}

			hub->nerrors = 0;
			hub->error = 0;
		}

		/* deal with port status changes */
		for (i = 1; i <= hub->descriptor->bNbrPorts; i++) {
			if (test_bit(i, hub->busy_bits))
				continue;
			connect_change = test_bit(i, hub->change_bits);
			wakeup_change = test_and_clear_bit(i, hub->wakeup_bits);
			if (!test_and_clear_bit(i, hub->event_bits) &&
					!connect_change && !wakeup_change)
				continue;

			ret = hub_port_status(hub, i,
					&portstatus, &portchange);
			if (ret < 0)
				continue;

			if (portchange & USB_PORT_STAT_C_CONNECTION) {
				usb_clear_port_feature(hdev, i,
					USB_PORT_FEAT_C_CONNECTION);
				connect_change = 1;
			}

			if (portchange & USB_PORT_STAT_C_ENABLE) {
				if (!connect_change)
					dev_dbg (hub_dev,
						"port %d enable change, "
						"status %08x\n",
						i, portstatus);
				usb_clear_port_feature(hdev, i,
					USB_PORT_FEAT_C_ENABLE);

				if (!(portstatus & USB_PORT_STAT_ENABLE)
				    && !connect_change
				    && hub->ports[i - 1]->child) {
					dev_err (hub_dev,
					    "port %i "
					    "disabled by hub (EMI?), "
					    "re-enabling...\n",
						i);
					connect_change = 1;
				}
			}

			if (hub_handle_remote_wakeup(hub, i,
						portstatus, portchange))
				connect_change = 1;

			if (portchange & USB_PORT_STAT_C_OVERCURRENT) {
				u16 status = 0;
				u16 unused;

				dev_dbg(hub_dev, "over-current change on port "
					"%d\n", i);
				usb_clear_port_feature(hdev, i,
					USB_PORT_FEAT_C_OVER_CURRENT);
				msleep(100);	/* Cool down */
				hub_power_on(hub, true);
				hub_port_status(hub, i, &status, &unused);
				if (status & USB_PORT_STAT_OVERCURRENT)
					dev_err(hub_dev, "over-current "
						"condition on port %d\n", i);
			}

			if (portchange & USB_PORT_STAT_C_RESET) {
				dev_dbg (hub_dev,
					"reset change on port %d\n",
					i);
				usb_clear_port_feature(hdev, i,
					USB_PORT_FEAT_C_RESET);
			}
			if ((portchange & USB_PORT_STAT_C_BH_RESET) &&
					hub_is_superspeed(hub->hdev)) {
				dev_dbg(hub_dev,
					"warm reset change on port %d\n",
					i);
				usb_clear_port_feature(hdev, i,
					USB_PORT_FEAT_C_BH_PORT_RESET);
			}
			if (portchange & USB_PORT_STAT_C_LINK_STATE) {
				usb_clear_port_feature(hub->hdev, i,
						USB_PORT_FEAT_C_PORT_LINK_STATE);
			}
			if (portchange & USB_PORT_STAT_C_CONFIG_ERROR) {
				dev_warn(hub_dev,
					"config error on port %d\n",
					i);
				usb_clear_port_feature(hub->hdev, i,
						USB_PORT_FEAT_C_PORT_CONFIG_ERROR);
			}

			if (hub_port_warm_reset_required(hub, portstatus)) {
				int status;
				struct usb_device *udev =
					hub->ports[i - 1]->child;

				dev_dbg(hub_dev, "warm reset port %d\n", i);
				if (!udev ||
				    !(portstatus & USB_PORT_STAT_CONNECTION) ||
				    udev->state == USB_STATE_NOTATTACHED) {
					status = hub_port_reset(hub, i,
							NULL, HUB_BH_RESET_TIME,
							true);
					if (status < 0)
						hub_port_disable(hub, i, 1);
				} else {
					usb_lock_device(udev);
					status = usb_reset_device(udev);
					usb_unlock_device(udev);
					connect_change = 0;
				}
			}

			if (connect_change)
				hub_port_connect_change(hub, i,
						portstatus, portchange);
		} /* end for i */

		/* deal with hub status changes */
		if (test_and_clear_bit(0, hub->event_bits) == 0)
			;	/* do nothing */
		else if (hub_hub_status(hub, &hubstatus, &hubchange) < 0)
			dev_err (hub_dev, "get_hub_status failed\n");
		else {
			if (hubchange & HUB_CHANGE_LOCAL_POWER) {
				dev_dbg (hub_dev, "power change\n");
				clear_hub_feature(hdev, C_HUB_LOCAL_POWER);
				if (hubstatus & HUB_STATUS_LOCAL_POWER)
					/* FIXME: Is this always true? */
					hub->limited_power = 1;
				else
					hub->limited_power = 0;
			}
			if (hubchange & HUB_CHANGE_OVERCURRENT) {
				u16 status = 0;
				u16 unused;

				dev_dbg(hub_dev, "over-current change\n");
				clear_hub_feature(hdev, C_HUB_OVER_CURRENT);
				msleep(500);	/* Cool down */
                        	hub_power_on(hub, true);
				hub_hub_status(hub, &status, &unused);
				if (status & HUB_STATUS_OVERCURRENT)
					dev_err(hub_dev, "over-current "
						"condition\n");
			}
		}

 loop_autopm:
		
		usb_autopm_put_interface_no_suspend(intf);
 loop:
		usb_autopm_put_interface(intf);
 loop_disconnected:
		usb_unlock_device(hdev);
		usb_put_dev(hdev);
 hub_disconnected:
		kref_put(&hub->kref, hub_release);

        } 
}

static int hub_thread(void *__unused)
{
	set_freezable();

	do {
		hub_events();
		wait_event_freezable(khubd_wait,
				!list_empty(&hub_event_list) ||
				kthread_should_stop());
	} while (!kthread_should_stop() || !list_empty(&hub_event_list));

	pr_debug("%s: khubd exiting\n", usbcore_name);
	return 0;
}

static const struct usb_device_id hub_id_table[] = {
    { .match_flags = USB_DEVICE_ID_MATCH_VENDOR
	           | USB_DEVICE_ID_MATCH_INT_CLASS,
      .idVendor = USB_VENDOR_GENESYS_LOGIC,
      .bInterfaceClass = USB_CLASS_HUB,
      .driver_info = HUB_QUIRK_CHECK_PORT_AUTOSUSPEND},
    { .match_flags = USB_DEVICE_ID_MATCH_DEV_CLASS,
      .bDeviceClass = USB_CLASS_HUB},
    { .match_flags = USB_DEVICE_ID_MATCH_INT_CLASS,
      .bInterfaceClass = USB_CLASS_HUB},
    { }						/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, hub_id_table);

static struct usb_driver hub_driver = {
	.name =		"hub",
	.probe =	hub_probe,
	.disconnect =	hub_disconnect,
	.suspend =	hub_suspend,
	.resume =	hub_resume,
	.reset_resume =	hub_reset_resume,
	.pre_reset =	hub_pre_reset,
	.post_reset =	hub_post_reset,
	.unlocked_ioctl = hub_ioctl,
	.id_table =	hub_id_table,
	.supports_autosuspend =	1,
};

int usb_hub_init(void)
{
	if (usb_register(&hub_driver) < 0) {
		printk(KERN_ERR "%s: can't register hub driver\n",
			usbcore_name);
		return -1;
	}

	khubd_task = kthread_run(hub_thread, NULL, "khubd");
	if (!IS_ERR(khubd_task))
		return 0;

	/* Fall through if kernel_thread failed */
	usb_deregister(&hub_driver);
	printk(KERN_ERR "%s: can't start khubd\n", usbcore_name);

	return -1;
}

void usb_hub_cleanup(void)
{
	kthread_stop(khubd_task);

	usb_deregister(&hub_driver);
} 

static int descriptors_changed(struct usb_device *udev,
		struct usb_device_descriptor *old_device_descriptor)
{
	int		changed = 0;
	unsigned	index;
	unsigned	serial_len = 0;
	unsigned	len;
	unsigned	old_length;
	int		length;
	char		*buf;

	if (memcmp(&udev->descriptor, old_device_descriptor,
			sizeof(*old_device_descriptor)) != 0)
		return 1;

	if (udev->serial)
		serial_len = strlen(udev->serial) + 1;

	len = serial_len;
	for (index = 0; index < udev->descriptor.bNumConfigurations; index++) {
		old_length = le16_to_cpu(udev->config[index].desc.wTotalLength);
		len = max(len, old_length);
	}

	buf = kmalloc(len, GFP_NOIO);
	if (buf == NULL) {
		dev_err(&udev->dev, "no mem to re-read configs after reset\n");
		/* assume the worst */
		return 1;
	}
	for (index = 0; index < udev->descriptor.bNumConfigurations; index++) {
		old_length = le16_to_cpu(udev->config[index].desc.wTotalLength);
		length = usb_get_descriptor(udev, USB_DT_CONFIG, index, buf,
				old_length);
		if (length != old_length) {
			dev_dbg(&udev->dev, "config index %d, error %d\n",
					index, length);
			changed = 1;
			break;
		}
		if (memcmp (buf, udev->rawdescriptors[index], old_length)
				!= 0) {
			dev_dbg(&udev->dev, "config index %d changed (#%d)\n",
				index,
				((struct usb_config_descriptor *) buf)->
					bConfigurationValue);
			changed = 1;
			break;
		}
	}

	if (!changed && serial_len) {
		length = usb_string(udev, udev->descriptor.iSerialNumber,
				buf, serial_len);
		if (length + 1 != serial_len) {
			dev_dbg(&udev->dev, "serial string error %d\n",
					length);
			changed = 1;
		} else if (memcmp(buf, udev->serial, length) != 0) {
			dev_dbg(&udev->dev, "serial string changed\n");
			changed = 1;
		}
	}

	kfree(buf);
	return changed;
}

static int usb_reset_and_verify_device(struct usb_device *udev)
{
	struct usb_device		*parent_hdev = udev->parent;
	struct usb_hub			*parent_hub;
	struct usb_hcd			*hcd = bus_to_hcd(udev->bus);
	struct usb_device_descriptor	descriptor = udev->descriptor;
	int 				i, ret = 0;
	int				port1 = udev->portnum;

	if (udev->state == USB_STATE_NOTATTACHED ||
			udev->state == USB_STATE_SUSPENDED) {
		dev_dbg(&udev->dev, "device reset not allowed in state %d\n",
				udev->state);
		return -EINVAL;
	}

	if (!parent_hdev) {
		
		dev_dbg(&udev->dev, "%s for root hub!\n", __func__);
		return -EISDIR;
	}
	parent_hub = usb_hub_to_struct_hub(parent_hdev);

	if (!parent_hub) {
		dev_dbg(&udev->dev, "%s: parent_hub is NULL\n", __func__);
		return -ENODEV;
	}

	ret = usb_unlocked_disable_lpm(udev);
	if (ret) {
		dev_err(&udev->dev, "%s Failed to disable LPM\n.", __func__);
		goto re_enumerate;
	}
	ret = usb_disable_ltm(udev);
	if (ret) {
		dev_err(&udev->dev, "%s Failed to disable LTM\n.",
				__func__);
		goto re_enumerate;
	}

	set_bit(port1, parent_hub->busy_bits);
	for (i = 0; i < SET_CONFIG_TRIES; ++i) {

		usb_ep0_reinit(udev);
		ret = hub_port_init(parent_hub, udev, port1, i);
		if (ret >= 0 || ret == -ENOTCONN || ret == -ENODEV)
			break;
	}
	clear_bit(port1, parent_hub->busy_bits);

	if (ret < 0)
		goto re_enumerate;
 
	/* Device might have changed firmware (DFU or similar) */
	if (descriptors_changed(udev, &descriptor)) {
		dev_info(&udev->dev, "device firmware changed\n");
		udev->descriptor = descriptor;	/* for disconnect() calls */
		goto re_enumerate;
  	}

	/* Restore the device's previous configuration */
	if (!udev->actconfig)
		goto done;

	mutex_lock(hcd->bandwidth_mutex);
	ret = usb_hcd_alloc_bandwidth(udev, udev->actconfig, NULL, NULL);
	if (ret < 0) {
		dev_warn(&udev->dev,
				"Busted HC?  Not enough HCD resources for "
				"old configuration.\n");
		mutex_unlock(hcd->bandwidth_mutex);
		goto re_enumerate;
	}
	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			USB_REQ_SET_CONFIGURATION, 0,
			udev->actconfig->desc.bConfigurationValue, 0,
			NULL, 0, USB_CTRL_SET_TIMEOUT);
	if (ret < 0) {
		dev_err(&udev->dev,
			"can't restore configuration #%d (error=%d)\n",
			udev->actconfig->desc.bConfigurationValue, ret);
		mutex_unlock(hcd->bandwidth_mutex);
		goto re_enumerate;
  	}
	mutex_unlock(hcd->bandwidth_mutex);
	usb_set_device_state(udev, USB_STATE_CONFIGURED);

	for (i = 0; i < udev->actconfig->desc.bNumInterfaces; i++) {
		struct usb_host_config *config = udev->actconfig;
		struct usb_interface *intf = config->interface[i];
		struct usb_interface_descriptor *desc;

		desc = &intf->cur_altsetting->desc;
		if (desc->bAlternateSetting == 0) {
			usb_disable_interface(udev, intf, true);
			usb_enable_interface(udev, intf, true);
			ret = 0;
		} else {
			intf->resetting_device = 1;
			ret = usb_set_interface(udev, desc->bInterfaceNumber,
					desc->bAlternateSetting);
			intf->resetting_device = 0;
		}
		if (ret < 0) {
			dev_err(&udev->dev, "failed to restore interface %d "
				"altsetting %d (error=%d)\n",
				desc->bInterfaceNumber,
				desc->bAlternateSetting,
				ret);
			goto re_enumerate;
		}
	}

done:
	
	usb_unlocked_enable_lpm(udev);
	usb_enable_ltm(udev);
	return 0;
 
re_enumerate:
	
	hub_port_logical_disconnect(parent_hub, port1);
	return -ENODEV;
}

int usb_reset_device(struct usb_device *udev)
{
	int ret;
	int i;
	unsigned int noio_flag;
	struct usb_host_config *config = udev->actconfig;

	if (udev->state == USB_STATE_NOTATTACHED ||
			udev->state == USB_STATE_SUSPENDED) {
		dev_dbg(&udev->dev, "device reset not allowed in state %d\n",
				udev->state);
		return -EINVAL;
	}

	noio_flag = memalloc_noio_save();

	
	usb_autoresume_device(udev);

	if (config) {
		for (i = 0; i < config->desc.bNumInterfaces; ++i) {
			struct usb_interface *cintf = config->interface[i];
			struct usb_driver *drv;
			int unbind = 0;

			if (cintf->dev.driver) {
				drv = to_usb_driver(cintf->dev.driver);
				if (drv->pre_reset && drv->post_reset)
					unbind = (drv->pre_reset)(cintf);
				else if (cintf->condition ==
						USB_INTERFACE_BOUND)
					unbind = 1;
				if (unbind)
					usb_forced_unbind_intf(cintf);
			}
		}
	}

	ret = usb_reset_and_verify_device(udev);

	if (config) {
		for (i = config->desc.bNumInterfaces - 1; i >= 0; --i) {
			struct usb_interface *cintf = config->interface[i];
			struct usb_driver *drv;
			int rebind = cintf->needs_binding;

			if (!rebind && cintf->dev.driver) {
				drv = to_usb_driver(cintf->dev.driver);
				if (drv->post_reset)
					rebind = (drv->post_reset)(cintf);
				else if (cintf->condition ==
						USB_INTERFACE_BOUND)
					rebind = 1;
				if (rebind)
					cintf->needs_binding = 1;
			}
		}
		usb_unbind_and_rebind_marked_interfaces(udev);
	}

	usb_autosuspend_device(udev);
	memalloc_noio_restore(noio_flag);
	return ret;
}
EXPORT_SYMBOL_GPL(usb_reset_device);


void usb_queue_reset_device(struct usb_interface *iface)
{
	schedule_work(&iface->reset_ws);
}
EXPORT_SYMBOL_GPL(usb_queue_reset_device);

struct usb_device *usb_hub_find_child(struct usb_device *hdev,
		int port1)
{
	struct usb_hub *hub = usb_hub_to_struct_hub(hdev);

	if (!hub || port1 < 1 || port1 > hdev->maxchild)
		return NULL;
	return hub->ports[port1 - 1]->child;
}
EXPORT_SYMBOL_GPL(usb_hub_find_child);

void usb_set_hub_port_connect_type(struct usb_device *hdev, int port1,
	enum usb_port_connect_type type)
{
	struct usb_hub *hub = usb_hub_to_struct_hub(hdev);

	if (hub)
		hub->ports[port1 - 1]->connect_type = type;
}

enum usb_port_connect_type
usb_get_hub_port_connect_type(struct usb_device *hdev, int port1)
{
	struct usb_hub *hub = usb_hub_to_struct_hub(hdev);

	if (!hub)
		return USB_PORT_CONNECT_TYPE_UNKNOWN;

	return hub->ports[port1 - 1]->connect_type;
}

void usb_hub_adjust_deviceremovable(struct usb_device *hdev,
		struct usb_hub_descriptor *desc)
{
	enum usb_port_connect_type connect_type;
	int i;

	if (!hub_is_superspeed(hdev)) {
		for (i = 1; i <= hdev->maxchild; i++) {
			connect_type = usb_get_hub_port_connect_type(hdev, i);

			if (connect_type == USB_PORT_CONNECT_TYPE_HARD_WIRED) {
				u8 mask = 1 << (i%8);

				if (!(desc->u.hs.DeviceRemovable[i/8] & mask)) {
					dev_dbg(&hdev->dev, "usb port%d's DeviceRemovable is changed to 1 according to platform information.\n",
						i);
					desc->u.hs.DeviceRemovable[i/8]	|= mask;
				}
			}
		}
	} else {
		u16 port_removable = le16_to_cpu(desc->u.ss.DeviceRemovable);

		for (i = 1; i <= hdev->maxchild; i++) {
			connect_type = usb_get_hub_port_connect_type(hdev, i);

			if (connect_type == USB_PORT_CONNECT_TYPE_HARD_WIRED) {
				u16 mask = 1 << i;

				if (!(port_removable & mask)) {
					dev_dbg(&hdev->dev, "usb port%d's DeviceRemovable is changed to 1 according to platform information.\n",
						i);
					port_removable |= mask;
				}
			}
		}

		desc->u.ss.DeviceRemovable = cpu_to_le16(port_removable);
	}
}

#ifdef CONFIG_ACPI
acpi_handle usb_get_hub_port_acpi_handle(struct usb_device *hdev,
	int port1)
{
	struct usb_hub *hub = usb_hub_to_struct_hub(hdev);

	if (!hub)
		return NULL;

	return DEVICE_ACPI_HANDLE(&hub->ports[port1 - 1]->dev);
}
#endif
