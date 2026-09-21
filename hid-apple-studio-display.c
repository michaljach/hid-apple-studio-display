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
 * brightness slider.  The vendor interface sharing that HID group is handed
 * to the generic HID paths untouched.
 *
 * A second interface is a HID sensor hub holding only a Device Orientation
 * collection (usage 0x20008A): input report 1 with three 9-bit "tilt" angles
 * (0..360, usages 0x47F..0x481) and no feature report at all.  The in-tree
 * sensor drivers cannot use it (no report interval / power state, and the
 * sensor hub core walks report fields byte-wise, so 9-bit fields come out
 * garbled), so this driver claims that interface too and exposes the angles
 * as an IIO inclinometer (in_incli_{x,y,z}_raw, degrees), fetched with
 * GET_REPORT on read.  An upright display in landscape reads 0/0/0; which
 * axis moves when the panel is turned to portrait, and by how much, is not
 * confirmed yet.  The display's ambient light sensors live on a third
 * interface that stays with hid-sensor-hub.
 */

#include <linux/backlight.h>
#include <linux/hid.h>
#include <linux/idr.h>
#include <linux/iio/iio.h>
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
/* Sensors usage page (0x20), Orientation: Tilt X/Y/Z data fields */
#define ASD_USAGE_TILT_X	0x0020047f
#define ASD_USAGE_TILT_Y	0x00200480
#define ASD_USAGE_TILT_Z	0x00200481

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

/* One usage inside a report: the field holding it and its index in there */
struct asd_usage {
	struct hid_field *field;
	unsigned int index;
};

struct asd_device {
	struct hid_device *hdev;
	struct hid_report *report;		/* feature report with the controls */
	struct asd_usage brightness;
	struct asd_usage duration;		/* optional */
	struct mutex lock;			/* serialises access to buf */
	u8 *buf;				/* hid_report_len(report) bytes */
	struct backlight_device *bl;
	struct delayed_work register_work;
	unsigned int retries;
	int id;
};

struct asd_orient {
	struct hid_device *hdev;
	struct hid_report *report;		/* input report with the angles */
	struct asd_usage tilt[3];
	struct mutex lock;			/* serialises access to buf */
	u8 *buf;
};

/* --- HID transport ----------------------------------------------------- */

/*
 * The display autosuspends after a couple of idle seconds and control
 * transfers to a suspended device fail, so wake it for the request, as
 * hidraw does.
 */
static int asd_hid_request(struct hid_device *hdev, struct hid_report *report,
			   u8 *buf, int reqtype)
{
	int ret;

	ret = hid_hw_power(hdev, PM_HINT_FULLON);
	if (ret < 0)
		return ret;
	ret = hid_hw_raw_request(hdev, report->id, buf, hid_report_len(report),
				 report->type, reqtype);
	hid_hw_power(hdev, PM_HINT_NORMAL);

	return ret;
}

/* Fetch a report and pull one usage's value out of it */
static int asd_hid_get(struct hid_device *hdev, struct hid_report *report,
		       u8 *buf, const struct asd_usage *u, u32 *value)
{
	unsigned int offset = u->field->report_offset +
			      u->index * u->field->report_size;
	int need = 1 + DIV_ROUND_UP(offset + u->field->report_size, 8);
	int ret;

	memset(buf, 0, hid_report_len(report));
	buf[0] = report->id;
	ret = asd_hid_request(hdev, report, buf, HID_REQ_GET_REPORT);
	if (ret < 0)
		return ret;
	if (ret < need)
		return -EIO;

	*value = hid_field_extract(hdev, buf + 1, offset, u->field->report_size);
	return 0;
}

static bool asd_find_usage(struct hid_report *report, unsigned int usage,
			   struct asd_usage *u)
{
	int i, j;

	for (i = 0; i < report->maxfield; i++) {
		struct hid_field *f = report->field[i];

		for (j = 0; j < f->maxusage; j++) {
			if (f->usage[j].hid != usage)
				continue;
			if (u) {
				u->field = f;
				u->index = j;
			}
			return true;
		}
	}

	return false;
}

static struct hid_report *asd_find_report(struct hid_device *hdev,
					  unsigned int type, unsigned int usage)
{
	struct hid_report *report;

	list_for_each_entry(report, &hdev->report_enum[type].report_list, list) {
		if (asd_find_usage(report, usage, NULL))
			return report;
	}

	return NULL;
}

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

/* --- brightness -------------------------------------------------------- */

static int asd_set_brightness(struct asd_device *asd, u32 value)
{
	int ret;

	mutex_lock(&asd->lock);
	asd->brightness.field->value[asd->brightness.index] = value;
	if (asd->duration.field)
		asd->duration.field->value[asd->duration.index] =
			min_t(u32, fade_ms, asd->duration.field->logical_maximum);
	hid_output_report(asd->report, asd->buf);
	ret = asd_hid_request(asd->hdev, asd->report, asd->buf, HID_REQ_SET_REPORT);
	mutex_unlock(&asd->lock);

	return ret < 0 ? ret : 0;
}

static int asd_get_brightness(struct asd_device *asd, u32 *value)
{
	int ret;

	mutex_lock(&asd->lock);
	ret = asd_hid_get(asd->hdev, asd->report, asd->buf, &asd->brightness, value);
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
						 asd->brightness.field->logical_minimum,
						 asd->brightness.field->logical_maximum));
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
		.max_brightness = asd->brightness.field->logical_maximum,
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
		 name, dev_name(parent), asd->brightness.field->logical_minimum,
		 asd->brightness.field->logical_maximum, value);
}

/* --- orientation sensor ------------------------------------------------ */

#define ASD_ORIENT_CHANNEL(_mod, _idx) {				\
	.type = IIO_INCLI,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##_mod,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
	.address = (_idx),						\
}

static const struct iio_chan_spec asd_orient_channels[] = {
	ASD_ORIENT_CHANNEL(X, 0),
	ASD_ORIENT_CHANNEL(Y, 1),
	ASD_ORIENT_CHANNEL(Z, 2),
};

static int asd_orient_read_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan,
			       int *val, int *val2, long mask)
{
	struct asd_orient *o = iio_priv(indio_dev);
	u32 value;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&o->lock);
		ret = asd_hid_get(o->hdev, o->report, o->buf,
				  &o->tilt[chan->address], &value);
		mutex_unlock(&o->lock);
		if (ret)
			return ret;
		*val = value;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 1;	/* degrees */
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static const struct iio_info asd_orient_info = {
	.read_raw = asd_orient_read_raw,
};

static int asd_orient_probe(struct hid_device *hdev)
{
	static const unsigned int usages[3] = {
		ASD_USAGE_TILT_X, ASD_USAGE_TILT_Y, ASD_USAGE_TILT_Z,
	};
	struct hid_report *report;
	struct iio_dev *indio_dev;
	struct asd_orient *o;
	int i, ret;

	/* The other sensor-hub interface (ambient light) is not ours */
	report = asd_find_report(hdev, HID_INPUT_REPORT, ASD_USAGE_TILT_X);
	if (!report)
		return -ENODEV;

	indio_dev = devm_iio_device_alloc(&hdev->dev, sizeof(*o));
	if (!indio_dev)
		return -ENOMEM;

	o = iio_priv(indio_dev);
	o->hdev = hdev;
	o->report = report;
	for (i = 0; i < ARRAY_SIZE(usages); i++) {
		if (!asd_find_usage(report, usages[i], &o->tilt[i]))
			return -ENODEV;
	}
	o->buf = devm_kzalloc(&hdev->dev, hid_report_len(report), GFP_KERNEL);
	if (!o->buf)
		return -ENOMEM;
	mutex_init(&o->lock);

	indio_dev->name = "apple_studio_display_orientation";
	indio_dev->info = &asd_orient_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = asd_orient_channels;
	indio_dev->num_channels = ARRAY_SIZE(asd_orient_channels);

	hid_set_drvdata(hdev, indio_dev);

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	ret = iio_device_register(indio_dev);
	if (ret) {
		hid_hw_stop(hdev);
		return ret;
	}

	hid_info(hdev, "orientation sensor registered as %s\n", indio_dev->name);
	return 0;
}

static void asd_orient_remove(struct hid_device *hdev)
{
	iio_device_unregister(hid_get_drvdata(hdev));
	hid_hw_stop(hdev);
}

/* --- HID driver -------------------------------------------------------- */

static int asd_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct hid_report *report;
	struct asd_device *asd;
	int ret;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	if (hdev->group == HID_GROUP_SENSOR_HUB)
		return asd_orient_probe(hdev);

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
	asd_find_usage(report, ASD_USAGE_BRIGHTNESS, &asd->brightness);
	asd_find_usage(report, ASD_USAGE_DURATION, &asd->duration);
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

	if (hdev->group == HID_GROUP_SENSOR_HUB) {
		asd_orient_remove(hdev);
		return;
	}

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
	{ HID_DEVICE(BUS_USB, HID_GROUP_SENSOR_HUB, USB_VENDOR_ID_APPLE,
		     USB_DEVICE_ID_APPLE_STUDIO_DISPLAY) },
	{ HID_DEVICE(BUS_USB, HID_GROUP_SENSOR_HUB, USB_VENDOR_ID_APPLE,
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
MODULE_DESCRIPTION("Apple Studio Display backlight and orientation sensor driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.1.0");
