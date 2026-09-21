// SPDX-License-Identifier: GPL-2.0
/*
 * Backlight driver for the Apple Studio Display
 *
 * The Studio Display (2022, USB PID 0x1114) and Studio Display XDR (2026,
 * USB PID 0x1116) expose their brightness on one of their USB HID interfaces:
 * a Monitor Control application collection (usage page 0x80) whose feature
 * report 1 carries the brightness as a 32-bit value in units of 0.01 nit
 * (logical range 400..60000 = 4..600 nits), followed by a 16-bit transition
 * time in milliseconds.  The descriptor also declares an input report with
 * the brightness, but the interrupt IN endpoint behind it errors out on the
 * XDR: usbhid retries for five seconds and then resets the display, which
 * drops off the bus and re-enumerates.  The endpoint is therefore never
 * opened and brightness changes made behind our back are only seen when the
 * value is read.
 *
 * This driver registers a "raw" backlight class device for that interface,
 * parented to the DRM connector the display is attached to.  That is the
 * layout GPU drivers use for laptop panels, and the one desktop environments
 * (GNOME's mutter in particular) look for to attach an external monitor's
 * brightness slider.  The display's other HID interfaces (vendor and sensor
 * hub collections) are handed to the generic HID paths untouched.
 */

#include <linux/backlight.h>
#include <linux/hid.h>
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <drm/drm_connector.h>

#define USB_VENDOR_ID_APPLE			0x05ac
#define USB_DEVICE_ID_APPLE_STUDIO_DISPLAY	0x1114
#define USB_DEVICE_ID_APPLE_STUDIO_DISPLAY_XDR	0x1116

/* VESA Virtual Controls usage page (0x82), Brightness usage (0x10) */
#define ASD_USAGE_BRIGHTNESS	0x00820010
/* Physical Interface Device usage page (0x0f), Duration usage (0x50) */
#define ASD_USAGE_DURATION	0x000f0050

/* How long to keep waiting for the GPU driver to bring up its connectors */
#define ASD_CONNECTOR_RETRY_MS	1000
#define ASD_CONNECTOR_RETRIES	120

static char *connector;
module_param(connector, charp, 0444);
MODULE_PARM_DESC(connector,
		 "DRM connector the display is attached to, e.g. DP-1 (default: the only connected connector)");

static unsigned int fade_ms;
module_param(fade_ms, uint, 0644);
MODULE_PARM_DESC(fade_ms, "Transition time for brightness changes in milliseconds (default: 0)");

static DEFINE_IDA(asd_ida);

struct asd_device {
	struct hid_device *hdev;
	struct hid_report *report;		/* feature report with the controls */
	struct hid_field *brightness;
	struct hid_field *duration;		/* optional */
	struct mutex lock;			/* serialises access to buf */
	u8 *buf;				/* hid_report_len(report) bytes */
	struct backlight_device *bl;
	struct delayed_work register_work;
	unsigned int retries;
	int id;
};

/* --- DRM connector lookup --------------------------------------------- */

struct asd_connector_search {
	const char *name;		/* NULL: pick the only connected one */
	struct device *found;
	unsigned int n_connected;
};

static int asd_match_connector(struct device *dev, void *data)
{
	struct asd_connector_search *s = data;
	struct drm_connector *conn;

	if (!dev->type || !dev->type->name ||
	    strcmp(dev->type->name, "drm_connector"))
		return 0;

	conn = dev_get_drvdata(dev);
	if (!conn || !conn->name)
		return 0;

	if (s->name) {
		if (strcmp(conn->name, s->name))
			return 0;
		s->found = get_device(dev);
		return 1;
	}

	if (conn->status != connector_status_connected)
		return 0;
	if (!s->found)
		s->found = get_device(dev);
	s->n_connected++;
	return 0;
}

static int asd_match_card(struct device *dev, void *data)
{
	if (!dev->class || strcmp(dev->class->name, "drm") ||
	    strncmp(dev_name(dev), "card", 4))
		return 0;

	return device_for_each_child(dev, data, asd_match_connector);
}

/*
 * Find the sysfs device of the DRM connector the display hangs off.  Nothing
 * lets a non-DRM driver enumerate connectors, so walk the device tree instead:
 * PCI display controller -> drm/cardN -> cardN-<connector>.
 */
static struct device *asd_find_connector(struct hid_device *hdev)
{
	struct asd_connector_search s = {
		.name = connector && *connector ? connector : NULL,
	};
	struct pci_dev *pdev = NULL;

	while ((pdev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, pdev))) {
		if ((pdev->class >> 16) != PCI_BASE_CLASS_DISPLAY)
			continue;
		if (device_for_each_child(&pdev->dev, &s, asd_match_card)) {
			pci_dev_put(pdev);
			break;
		}
	}

	if (!s.name && s.n_connected > 1) {
		hid_warn(hdev, "%u connected DRM connectors, set the connector= module parameter\n",
			 s.n_connected);
		put_device(s.found);
		return NULL;
	}

	return s.found;
}

/* --- HID transport ----------------------------------------------------- */

/*
 * The display autosuspends after a couple of idle seconds and control
 * transfers to a suspended device fail, so wake it for the request, as
 * hidraw does.
 */
static int asd_raw_request(struct asd_device *asd, int reqtype)
{
	struct hid_device *hdev = asd->hdev;
	int ret;

	ret = hid_hw_power(hdev, PM_HINT_FULLON);
	if (ret < 0)
		return ret;
	ret = hid_hw_raw_request(hdev, asd->report->id, asd->buf,
				 hid_report_len(asd->report),
				 HID_FEATURE_REPORT, reqtype);
	hid_hw_power(hdev, PM_HINT_NORMAL);

	return ret;
}

static int asd_set_brightness(struct asd_device *asd, u32 value)
{
	int ret;

	mutex_lock(&asd->lock);
	asd->brightness->value[0] = value;
	if (asd->duration)
		asd->duration->value[0] = min_t(u32, fade_ms,
						asd->duration->logical_maximum);
	hid_output_report(asd->report, asd->buf);
	ret = asd_raw_request(asd, HID_REQ_SET_REPORT);
	mutex_unlock(&asd->lock);

	return ret < 0 ? ret : 0;
}

static int asd_get_brightness(struct asd_device *asd, u32 *value)
{
	struct hid_field *f = asd->brightness;
	int len = hid_report_len(asd->report);
	int need = 1 + DIV_ROUND_UP(f->report_offset + f->report_size, 8);
	int ret;

	mutex_lock(&asd->lock);
	memset(asd->buf, 0, len);
	asd->buf[0] = asd->report->id;
	ret = asd_raw_request(asd, HID_REQ_GET_REPORT);
	if (ret >= 0 && ret < need)
		ret = -EIO;
	if (ret >= 0) {
		*value = hid_field_extract(asd->hdev, asd->buf + 1,
					   f->report_offset, f->report_size);
		ret = 0;
	}
	mutex_unlock(&asd->lock);

	return ret;
}

/* --- backlight class --------------------------------------------------- */

static int asd_bl_update_status(struct backlight_device *bl)
{
	struct asd_device *asd = bl_get_data(bl);
	int brightness = backlight_get_brightness(bl);

	/* The panel cannot be switched off through this control, only dimmed */
	return asd_set_brightness(asd, clamp_val(brightness,
						 asd->brightness->logical_minimum,
						 asd->brightness->logical_maximum));
}

static int asd_bl_get_brightness(struct backlight_device *bl)
{
	struct asd_device *asd = bl_get_data(bl);
	u32 value;
	int ret;

	ret = asd_get_brightness(asd, &value);
	return ret ? ret : value;
}

static const struct backlight_ops asd_bl_ops = {
	.update_status = asd_bl_update_status,
	.get_brightness = asd_bl_get_brightness,
};

static void asd_register_backlight(struct work_struct *work)
{
	struct asd_device *asd = container_of(work, struct asd_device,
					      register_work.work);
	struct hid_device *hdev = asd->hdev;
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.scale = BACKLIGHT_SCALE_LINEAR,
		.max_brightness = asd->brightness->logical_maximum,
	};
	struct backlight_device *bl;
	struct device *parent;
	char name[32];
	u32 value;

	parent = asd_find_connector(hdev);
	if (!parent) {
		if (++asd->retries < ASD_CONNECTOR_RETRIES) {
			schedule_delayed_work(&asd->register_work,
					      msecs_to_jiffies(ASD_CONNECTOR_RETRY_MS));
			return;
		}
		hid_warn(hdev, "no DRM connector found, registering the backlight without one (desktops will not pick it up)\n");
		parent = get_device(&hdev->dev);
	}

	if (asd_get_brightness(asd, &value))
		value = props.max_brightness;
	props.brightness = value;

	snprintf(name, sizeof(name), "apple_studio_display%d", asd->id);
	bl = backlight_device_register(name, parent, asd, &asd_bl_ops, &props);
	put_device(parent);
	if (IS_ERR(bl)) {
		hid_err(hdev, "failed to register backlight: %pe\n", bl);
		return;
	}
	asd->bl = bl;

	hid_info(hdev, "backlight %s registered on %s (%d..%d, now %u)\n",
		 name, dev_name(parent), asd->brightness->logical_minimum,
		 asd->brightness->logical_maximum, value);
}

/* --- HID driver -------------------------------------------------------- */

static struct hid_field *asd_find_field(struct hid_report *report,
					unsigned int usage)
{
	int i;

	for (i = 0; i < report->maxfield; i++) {
		struct hid_field *f = report->field[i];

		if (f->maxusage >= 1 && f->usage[0].hid == usage)
			return f;
	}

	return NULL;
}

static struct hid_report *asd_find_report(struct hid_device *hdev,
					  enum hid_report_type type,
					  unsigned int usage)
{
	struct hid_report *report;

	list_for_each_entry(report, &hdev->report_enum[type].report_list, list) {
		if (asd_find_field(report, usage))
			return report;
	}

	return NULL;
}

static int asd_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct hid_report *report;
	struct asd_device *asd;
	int ret;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	/* Only one of the display's interfaces carries the monitor controls */
	report = asd_find_report(hdev, HID_FEATURE_REPORT, ASD_USAGE_BRIGHTNESS);
	if (!report)
		return hid_hw_start(hdev, HID_CONNECT_DEFAULT);

	asd = devm_kzalloc(&hdev->dev, sizeof(*asd), GFP_KERNEL);
	if (!asd)
		return -ENOMEM;

	asd->buf = devm_kzalloc(&hdev->dev, hid_report_len(report), GFP_KERNEL);
	if (!asd->buf)
		return -ENOMEM;

	asd->hdev = hdev;
	asd->report = report;
	asd->brightness = asd_find_field(report, ASD_USAGE_BRIGHTNESS);
	asd->duration = asd_find_field(report, ASD_USAGE_DURATION);
	mutex_init(&asd->lock);
	INIT_DELAYED_WORK(&asd->register_work, asd_register_backlight);

	asd->id = ida_alloc(&asd_ida, GFP_KERNEL);
	if (asd->id < 0)
		return asd->id;

	hid_set_drvdata(hdev, asd);

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		ida_free(&asd_ida, asd->id);
		return ret;
	}

	schedule_delayed_work(&asd->register_work, 0);

	return 0;
}

static void asd_remove(struct hid_device *hdev)
{
	struct asd_device *asd = hid_get_drvdata(hdev);

	if (!asd) {
		hid_hw_stop(hdev);
		return;
	}

	cancel_delayed_work_sync(&asd->register_work);
	if (asd->bl)
		backlight_device_unregister(asd->bl);
	hid_hw_stop(hdev);
	ida_free(&asd_ida, asd->id);
}

static const struct hid_device_id asd_devices[] = {
	{ HID_DEVICE(BUS_USB, HID_GROUP_GENERIC, USB_VENDOR_ID_APPLE,
		     USB_DEVICE_ID_APPLE_STUDIO_DISPLAY) },
	{ HID_DEVICE(BUS_USB, HID_GROUP_GENERIC, USB_VENDOR_ID_APPLE,
		     USB_DEVICE_ID_APPLE_STUDIO_DISPLAY_XDR) },
	{ }
};
MODULE_DEVICE_TABLE(hid, asd_devices);

static struct hid_driver asd_driver = {
	.name		= "apple-studio-display",
	.id_table	= asd_devices,
	.probe		= asd_probe,
	.remove		= asd_remove,
};
module_hid_driver(asd_driver);

MODULE_AUTHOR("Michal Jach");
MODULE_DESCRIPTION("Apple Studio Display backlight driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.1");
