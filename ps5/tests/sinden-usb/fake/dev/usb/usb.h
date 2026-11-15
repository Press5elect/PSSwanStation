// PSSwanStation - a stand-in for FreeBSD's dev/usb/usb.h, so that
// src/ps5/sinden_ps5.cpp compiles on a PC for console_test.cpp. Only what that
// file uses, with FreeBSD's names; the console build uses the SDK's own header.
// No ugen node exists on a PC: these structures are never handed to a kernel.
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

typedef uint8_t uByte;
typedef uint8_t uWord[2];

struct usb_device_request
{
	uByte bmRequestType;
	uByte bRequest;
	uWord wValue;
	uWord wIndex;
	uWord wLength;
} __attribute__((packed));

struct usb_device_descriptor
{
	uByte bLength;
	uByte bDescriptorType;
	uWord bcdUSB;
	uByte bDeviceClass;
	uByte bDeviceSubClass;
	uByte bDeviceProtocol;
	uByte bMaxPacketSize;
	uWord idVendor;
	uWord idProduct;
	uWord bcdDevice;
	uByte iManufacturer;
	uByte iProduct;
	uByte iSerialNumber;
	uByte bNumConfigurations;
} __attribute__((packed));
