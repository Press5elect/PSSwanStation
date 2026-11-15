// PSSwanStation - a stand-in for FreeBSD's dev/usb/usb_ioctl.h (see usb.h
// beside it): the structures and requests src/ps5/sinden_ps5.cpp uses, with
// FreeBSD's names. The request numbers are only distinct here.
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <dev/usb/usb.h>

#include <cstdint>

struct usb_ctl_request
{
	void *ucr_data;
	uint16_t ucr_flags;
	uint16_t ucr_actlen;
	uint8_t ucr_addr;
	struct usb_device_request ucr_request;
};

struct usb_alt_interface
{
	uint8_t uai_interface_index;
	uint8_t uai_alt_index;
};

struct usb_gen_descriptor
{
	void *ugd_data;
	uint16_t ugd_lang_id;
	uint16_t ugd_maxlen;
	uint16_t ugd_actlen;
	uint16_t ugd_offset;
	uint8_t ugd_config_index;
	uint8_t ugd_string_index;
	uint8_t ugd_iface_index;
	uint8_t ugd_altif_index;
	uint8_t ugd_endpt_index;
	uint8_t ugd_report_type;
	uint8_t reserved[8];
};

struct usb_device_info
{
	uint16_t udi_productNo;
	uint16_t udi_vendorNo;
	uint16_t udi_releaseNo;
	uint16_t udi_power;
	uint8_t udi_bus;
	uint8_t udi_addr;
	uint8_t udi_index;
	uint8_t udi_class;
	uint8_t udi_subclass;
	uint8_t udi_protocol;
	uint8_t udi_config_no;
	uint8_t udi_config_index;
	uint8_t udi_speed;
	uint8_t udi_mode;
	uint8_t udi_nports;
	uint8_t udi_hubaddr;
	uint8_t udi_hubindex;
	uint8_t udi_hubport;
	uint8_t udi_power_mode;
	uint8_t udi_suspended;
	uint8_t udi_reserved[16];
	char udi_product[128];
	char udi_vendor[128];
	char udi_serial[64];
	char udi_release[8];
};

struct usb_fs_start
{
	uint8_t ep_index;
};
struct usb_fs_stop
{
	uint8_t ep_index;
};
struct usb_fs_complete
{
	uint8_t ep_index;
};
struct usb_fs_close
{
	uint8_t ep_index;
};
struct usb_fs_uninit
{
	uint8_t dummy;
};

struct usb_fs_endpoint
{
	void **ppBuffer;
	uint32_t *pLength;
	uint32_t nFrames;
	uint32_t aFrames;
	uint16_t flags;
#define USB_FS_FLAG_SINGLE_SHORT_OK 0x0001
#define USB_FS_FLAG_MULTI_SHORT_OK 0x0002
	uint16_t timeout;
	uint16_t isoc_time_complete;
	int status;
};

struct usb_fs_init
{
	struct usb_fs_endpoint *pEndpoints;
	uint8_t ep_index_max;
};

struct usb_fs_open
{
	uint32_t max_bufsize;
	uint32_t max_frames;
	uint16_t max_packet_length;
	uint8_t dev_index;
	uint8_t ep_index;
	uint8_t ep_no;
};

enum : unsigned long
{
	USB_GET_DEVICEINFO = 0x55000001,
	USB_GET_DEVICE_DESC,
	USB_GET_FULL_DESC,
	USB_DO_REQUEST,
	USB_GET_IFACE_DRIVER,
	USB_IFACE_DRIVER_ACTIVE,
	USB_IFACE_DRIVER_DETACH,
	USB_CLAIM_INTERFACE,
	USB_RELEASE_INTERFACE,
	USB_SET_ALTINTERFACE,
	USB_FS_INIT,
	USB_FS_UNINIT,
	USB_FS_OPEN,
	USB_FS_CLOSE,
	USB_FS_START,
	USB_FS_COMPLETE,
};
