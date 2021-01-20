// SPDX-License-Identifier: MIT
/*
 * Copyright 2020 Noralf Trønnes
 */

#include <linux/dma-buf.h>
#include <linux/lz4.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/string_helpers.h>
#include <linux/usb.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/gud.h>

#include "gud_internal.h"

/* Only used internally */
static const struct drm_format_info gud_drm_format_r1 = {
	.format = GUD_DRM_FORMAT_R1,
	.num_planes = 1,
	.char_per_block = { 1, 0, 0 },
	.block_w = { 8, 0, 0 },
	.block_h = { 1, 0, 0 },
	.hsub = 1,
	.vsub = 1,
};

static int gud_usb_control_msg(struct usb_device *usb, u8 ifnum, bool in,
			       u8 request, u16 value, void *buf, size_t len)
{
	u8 requesttype = USB_TYPE_VENDOR | USB_RECIP_INTERFACE;
	unsigned int pipe;
	int ret;

	if (in) {
		pipe = usb_rcvctrlpipe(usb, 0);
		requesttype |= USB_DIR_IN;
	} else {
		pipe = usb_sndctrlpipe(usb, 0);
		requesttype |= USB_DIR_OUT;
	}

	ret = usb_control_msg(usb, pipe, request, requesttype, value,
			      ifnum, buf, len, USB_CTRL_GET_TIMEOUT);
	if (ret < 0)
		return ret;
	if (ret != len)
		return -EIO;

	return 0;
}

static int gud_get_display_descriptor(struct usb_interface *interface,
				      struct gud_display_descriptor_req *desc)
{
	u8 ifnum = interface->cur_altsetting->desc.bInterfaceNumber;
	struct usb_device *usb = interface_to_usbdev(interface);
	void *buf;
	int ret;

	buf = kmalloc(sizeof(*desc), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = gud_usb_control_msg(usb, ifnum, true, GUD_REQ_GET_DESCRIPTOR, 0, buf, sizeof(*desc));
	memcpy(desc, buf, sizeof(*desc));
	kfree(buf);
	if (ret)
		return ret;

	if (desc->magic != GUD_DISPLAY_MAGIC)
		return -ENODATA;

	DRM_DEV_DEBUG_DRIVER(&interface->dev,
			     "version=%u flags=0x%x compression=0x%x num_formats=%u num_connectors=%u max_buffer_size=%u\n",
			     desc->version, le32_to_cpu(desc->flags), desc->compression,
			     desc->num_formats, desc->num_connectors,
			     le32_to_cpu(desc->max_buffer_size));

	if (!desc->version || !desc->num_formats || !desc->num_connectors ||
	    !desc->max_width || !desc->max_height ||
	    le32_to_cpu(desc->min_width) > le32_to_cpu(desc->max_width) ||
	    le32_to_cpu(desc->min_height) > le32_to_cpu(desc->max_height))
		return -EINVAL;

	return 0;
}

static int gud_usb_get_status(struct usb_device *usb, u8 ifnum, u8 *status)
{
	u8 *buf;
	int ret;

	buf = kmalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = gud_usb_control_msg(usb, ifnum, true, GUD_REQ_GET_STATUS, 0, buf, sizeof(*buf));
	*status = *buf;
	kfree(buf);

	return ret;
}

static int gud_status_to_errno(u8 status)
{
	switch (status) {
	case GUD_STATUS_OK:
		return 0;
	case GUD_STATUS_BUSY:
		return -EBUSY;
	case GUD_STATUS_REQUEST_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	case GUD_STATUS_PROTOCOL_ERROR:
		return -EPROTO;
	case GUD_STATUS_INVALID_PARAMETER:
		return -EINVAL;
	case GUD_STATUS_ERROR:
		return -EREMOTEIO;
	default:
		return -EREMOTEIO;
	}
}

static int gud_usb_transfer(struct gud_device *gdrm, bool in, u8 request, u16 index,
			    void *buf, size_t len)
{
	struct usb_device *usb = gud_to_usb_device(gdrm);
	void *trbuf = NULL;
	int idx, ret;

	drm_dbg(&gdrm->drm, "%s: request=0x%x index=%u len=%zu\n",
		in ? "get" : "set", request, index, len);

	if (!drm_dev_enter(&gdrm->drm, &idx))
		return -ENODEV;

	mutex_lock(&gdrm->ctrl_lock);

	if (buf) {
		if (in)
			trbuf = kmalloc(len, GFP_KERNEL);
		else
			trbuf = kmemdup(buf, len, GFP_KERNEL);
		if (!trbuf) {
			ret = -ENOMEM;
			goto unlock;
		}
	}

	ret = gud_usb_control_msg(usb, gdrm->ifnum, in, request, index, trbuf, len);
	if (ret == -EPIPE || (!ret && !in && (gdrm->flags & GUD_DISPLAY_FLAG_STATUS_ON_SET))) {
		bool error = ret;
		u8 status;

		ret = gud_usb_get_status(usb, gdrm->ifnum, &status);
		if (!ret) {
			if (error && status == GUD_STATUS_OK) {
				dev_err_once(gdrm->drm.dev,
					     "Unexpected status OK for failed transfer\n");
				ret = -EPIPE;
			} else {
				ret = gud_status_to_errno(status);
			}
		}
	}

	if (!ret && in && buf)
		memcpy(buf, trbuf, len);

	if (ret) {
		drm_dbg(&gdrm->drm, "ret=%d\n", ret);
		gdrm->stats_num_errors++;
	}

	kfree(trbuf);
unlock:
	mutex_unlock(&gdrm->ctrl_lock);
	drm_dev_exit(idx);

	return ret;
}

int gud_usb_get(struct gud_device *gdrm, u8 request, u16 index, void *buf, size_t len)
{
	return gud_usb_transfer(gdrm, true, request, index, buf, len);
}

int gud_usb_set(struct gud_device *gdrm, u8 request, u16 index, void *buf, size_t len)
{
	return gud_usb_transfer(gdrm, false, request, index, buf, len);
}

int gud_usb_write8(struct gud_device *gdrm, u8 request, u8 val)
{
	return gud_usb_set(gdrm, request, 0, &val, sizeof(val));
}

static int gud_set_version(struct usb_device *usb, u8 ifnum, u32 flags, u8 version)
{
	u8 *buf;
	int ret;

	buf = kmalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	*buf = version;
	ret = gud_usb_control_msg(usb, ifnum, false, GUD_REQ_SET_VERSION, 0, buf, sizeof(*buf));
	kfree(buf);
	if (ret == -EPIPE)
		return -EPROTONOSUPPORT;
	if (ret)
		return ret;

	if (flags & GUD_DISPLAY_FLAG_STATUS_ON_SET) {
		u8 status;

		ret = gud_usb_get_status(usb, ifnum, &status);
		if (!ret && status != GUD_STATUS_OK)
			ret = -EPROTONOSUPPORT;
	}

	return ret;
}

static int gud_get_properties(struct gud_device *gdrm, unsigned int num_properties)
{
	struct gud_property_req *properties;
	unsigned int i;
	int ret;

	if (!num_properties)
		return 0;

	gdrm->properties = drmm_kcalloc(&gdrm->drm, num_properties, sizeof(*gdrm->properties),
					GFP_KERNEL);
	if (!gdrm->properties)
		return -ENOMEM;

	properties = kcalloc(num_properties, sizeof(*properties), GFP_KERNEL);
	if (!properties)
		return -ENOMEM;

	ret = gud_usb_get(gdrm, GUD_REQ_GET_PROPERTIES, 0,
			  properties, num_properties * sizeof(*properties));
	if (ret)
		goto out;

	for (i = 0; i < num_properties; i++) {
		u16 prop = le16_to_cpu(properties[i].prop);
		u64 val = le64_to_cpu(properties[i].val);

		switch (prop) {
		case GUD_PROPERTY_ROTATION:
			/*
			 * DRM UAPI matches the protocol so use the value directly,
			 * but mask out any additions on future devices.
			 */
			val &= GUD_ROTATION_MASK;
			ret = drm_plane_create_rotation_property(&gdrm->pipe.plane,
								 DRM_MODE_ROTATE_0, val);
			break;
		default:
			/* New ones might show up in future devices, skip those we don't know. */
			drm_dbg(&gdrm->drm, "Unknown property: %u\n", prop);
			continue;
		}

		if (ret)
			goto out;

		gdrm->properties[gdrm->num_properties++] = prop;
	}
out:
	kfree(properties);

	return ret;
}

static int gud_stats_debugfs(struct seq_file *m, void *data)
{
	struct drm_info_node *node = m->private;
	struct gud_device *gdrm = to_gud_device(node->minor->dev);
	char buf[10];

	string_get_size(gdrm->bulk_len, 1, STRING_UNITS_2, buf, sizeof(buf));
	seq_printf(m, "Max buffer size: %s\n", buf);
	seq_printf(m, "Number of errors:  %u\n", gdrm->stats_num_errors);

	seq_puts(m, "Compression:      ");
	if (gdrm->compression & GUD_COMPRESSION_LZ4)
		seq_puts(m, " lz4");
	seq_puts(m, "\n");

	if (gdrm->compression) {
		u64 remainder;
		u64 ratio = div64_u64_rem(gdrm->stats_length, gdrm->stats_actual_length,
					  &remainder);
		u64 ratio_frac = div64_u64(remainder * 10, gdrm->stats_actual_length);

		seq_printf(m, "Compression ratio: %llu.%llu\n", ratio, ratio_frac);
	}

	return 0;
}

static const struct drm_info_list gud_debugfs_list[] = {
	{ "stats", gud_stats_debugfs, 0, NULL },
};

static void gud_debugfs_init(struct drm_minor *minor)
{
	drm_debugfs_create_files(gud_debugfs_list, ARRAY_SIZE(gud_debugfs_list),
				 minor->debugfs_root, minor);
}

static const struct drm_simple_display_pipe_funcs gud_pipe_funcs = {
	.check      = gud_pipe_check,
	.update	    = gud_pipe_update,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
};

static const struct drm_mode_config_funcs gud_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const u64 gud_pipe_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

DEFINE_DRM_GEM_FOPS(gud_fops);

static const struct drm_driver gud_drm_driver = {
	.driver_features	= DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.fops			= &gud_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	.debugfs_init		= gud_debugfs_init,

	.name			= "gud",
	.desc			= "Generic USB Display",
	.date			= "20200422",
	.major			= 1,
	.minor			= 0,
};

static void gud_free_buffers_and_mutex(struct drm_device *drm, void *unused)
{
	struct gud_device *gdrm = to_gud_device(drm);

	vfree(gdrm->compress_buf);
	kfree(gdrm->bulk_buf);
	mutex_destroy(&gdrm->ctrl_lock);
	mutex_destroy(&gdrm->damage_lock);
}

static int gud_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	u8 ifnum = interface->cur_altsetting->desc.bInterfaceNumber;
	struct usb_device *usb = interface_to_usbdev(interface);
	struct device *dev = &interface->dev;
	const struct drm_format_info *xrgb8888_emulation_format = NULL;
	bool rgb565_supported = false, xrgb8888_supported = false;
	struct usb_endpoint_descriptor *bulk_out;
	struct gud_display_descriptor_req desc;
	unsigned int num_formats = 0;
	struct gud_device *gdrm;
	size_t max_buffer_size = 0;
	struct drm_device *drm;
	u8 *formats_dev;
	u32 *formats;
	int ret, i;

	ret = usb_find_bulk_out_endpoint(interface->cur_altsetting, &bulk_out);
	if (ret)
		return ret;

	ret = gud_get_display_descriptor(interface, &desc);
	if (ret) {
		DRM_DEV_DEBUG_DRIVER(dev, "Not a display interface: ret=%d\n", ret);
		return -ENODEV;
	}

	if (desc.version > 1) {
		ret = gud_set_version(usb, ifnum, le32_to_cpu(desc.flags), 1);
		if (ret) {
			if (ret == -EPROTONOSUPPORT)
				dev_err(dev, "Protocol version %u is not supported\n",
					desc.version);
			return ret;
		}
	}

	gdrm = devm_drm_dev_alloc(dev, &gud_drm_driver, struct gud_device, drm);
	if (IS_ERR(gdrm))
		return PTR_ERR(gdrm);

	drm = &gdrm->drm;
	drm->mode_config.funcs = &gud_mode_config_funcs;
	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	gdrm->ifnum = ifnum;
	gdrm->flags = le32_to_cpu(desc.flags);
	gdrm->compression = desc.compression & GUD_COMPRESSION_LZ4;

	if (gdrm->flags & GUD_DISPLAY_FLAG_FULL_UPDATE && gdrm->compression)
		return -EINVAL;

	mutex_init(&gdrm->ctrl_lock);
	mutex_init(&gdrm->damage_lock);
	INIT_WORK(&gdrm->work, gud_flush_work);
	gud_clear_damage(gdrm);

	ret = drmm_add_action_or_reset(drm, gud_free_buffers_and_mutex, NULL);
	if (ret)
		return ret;

	drm->mode_config.min_width = le32_to_cpu(desc.min_width);
	drm->mode_config.max_width = le32_to_cpu(desc.max_width);
	drm->mode_config.min_height = le32_to_cpu(desc.min_height);
	drm->mode_config.max_height = le32_to_cpu(desc.max_height);

	formats_dev = devm_kmalloc(dev, desc.num_formats, GFP_KERNEL);
	/* Add room for emulated XRGB8888 */
	formats = devm_kmalloc_array(dev, desc.num_formats + 1, sizeof(*formats), GFP_KERNEL);
	if (!formats_dev || !formats)
		return -ENOMEM;

	ret = gud_usb_get(gdrm, GUD_REQ_GET_FORMATS, 0, formats_dev, desc.num_formats);
	if (ret)
		return ret;

	for (i = 0; i < desc.num_formats; i++) {
		const struct drm_format_info *info;
		size_t fmt_buf_size;
		u32 format;

		format = gud_to_fourcc(formats_dev[i]);
		if (!format) {
			drm_dbg(drm, "Unsupported format: 0x%02x\n", formats_dev[i]);
			continue;
		}

		if (format == GUD_DRM_FORMAT_R1)
			info = &gud_drm_format_r1;
		else
			info = drm_format_info(format);

		switch (format) {
		case GUD_DRM_FORMAT_R1:
			xrgb8888_emulation_format = info;
			break;
		case DRM_FORMAT_RGB565:
			rgb565_supported = true;
			if (!xrgb8888_emulation_format)
				xrgb8888_emulation_format = info;
			break;
		case DRM_FORMAT_XRGB8888:
			xrgb8888_supported = true;
			break;
		};

		fmt_buf_size = drm_format_info_min_pitch(info, 0, drm->mode_config.max_width) *
			       drm->mode_config.max_height;
		max_buffer_size = max(max_buffer_size, fmt_buf_size);

		if (format == GUD_DRM_FORMAT_R1)
			continue; /* Internal not for userspace */

		formats[num_formats++] = format;
	}

	if (!num_formats && !xrgb8888_emulation_format) {
		dev_err(dev, "No supported pixel formats found\n");
		return -EINVAL;
	}

	/* Prefer speed over color depth */
	if (rgb565_supported)
		drm->mode_config.preferred_depth = 16;

	if (!xrgb8888_supported && xrgb8888_emulation_format) {
		gdrm->xrgb8888_emulation_format = xrgb8888_emulation_format;
		formats[num_formats++] = DRM_FORMAT_XRGB8888;
	}

	if (desc.max_buffer_size)
		max_buffer_size = le32_to_cpu(desc.max_buffer_size);
retry:
	/*
	 * Use plain kmalloc here since devm_kmalloc() places struct devres at the beginning
	 * of the buffer it allocates. This wastes a lot of memory when allocating big buffers.
	 * Asking for 2M would actually allocate 4M. This would also prevent getting the biggest
	 * possible buffer potentially leading to split transfers.
	 */
	gdrm->bulk_buf = kmalloc(max_buffer_size, GFP_KERNEL | __GFP_NOWARN);
	if (!gdrm->bulk_buf) {
		max_buffer_size = roundup_pow_of_two(max_buffer_size) / 2;
		if (max_buffer_size < SZ_512K)
			return -ENOMEM;
		goto retry;
	}

	gdrm->bulk_pipe = usb_sndbulkpipe(usb, usb_endpoint_num(bulk_out));
	gdrm->bulk_len = max_buffer_size;

	if (gdrm->compression & GUD_COMPRESSION_LZ4) {
		gdrm->lz4_comp_mem = devm_kmalloc(dev, LZ4_MEM_COMPRESS, GFP_KERNEL);
		if (!gdrm->lz4_comp_mem)
			return -ENOMEM;

		gdrm->compress_buf = vmalloc(gdrm->bulk_len);
		if (!gdrm->compress_buf)
			return -ENOMEM;
	}

	ret = drm_simple_display_pipe_init(drm, &gdrm->pipe, &gud_pipe_funcs,
					   formats, num_formats,
					   gud_pipe_modifiers, NULL);
	if (ret)
		return ret;

	devm_kfree(dev, formats);
	devm_kfree(dev, formats_dev);

	ret = gud_get_properties(gdrm, desc.num_properties);
	if (ret)
		return ret;

	drm_plane_enable_fb_damage_clips(&gdrm->pipe.plane);

	for (i = 0; i < desc.num_connectors; i++) {
		ret = gud_connector_create(gdrm, i);
		if (ret)
			return ret;
	}

	drm_mode_config_reset(drm);

	usb_set_intfdata(interface, gdrm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_kms_helper_poll_init(drm);

	drm_fbdev_generic_setup(drm, 0);

	return 0;
}

static void gud_disconnect(struct usb_interface *interface)
{
	struct gud_device *gdrm = usb_get_intfdata(interface);
	struct drm_device *drm = &gdrm->drm;

	drm_dbg(drm, "%s:\n", __func__);

	drm_kms_helper_poll_fini(drm);
	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static int gud_suspend(struct usb_interface *interface, pm_message_t message)
{
	struct gud_device *gdrm = usb_get_intfdata(interface);

	return drm_mode_config_helper_suspend(&gdrm->drm);
}

static int gud_resume(struct usb_interface *interface)
{
	struct gud_device *gdrm = usb_get_intfdata(interface);

	drm_mode_config_helper_resume(&gdrm->drm);

	return 0;
}

static const struct usb_device_id gud_id_table[] = {
	{ USB_DEVICE_INTERFACE_CLASS(0x1d50, 0x614d, USB_CLASS_VENDOR_SPEC) },
	{ }
};

MODULE_DEVICE_TABLE(usb, gud_id_table);

static struct usb_driver gud_usb_driver = {
	.name		= "gud",
	.probe		= gud_probe,
	.disconnect	= gud_disconnect,
	.id_table	= gud_id_table,
	.suspend	= gud_suspend,
	.resume		= gud_resume,
	.reset_resume	= gud_resume,
};

module_usb_driver(gud_usb_driver);

MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("Dual MIT/GPL");
