// PSSwanStation - a Sinden Lightgun's configuration descriptor as the PC tests
// use it (test.cpp, console_test.cpp).
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Written byte by byte after the USB Video Class 1.1, CDC 1.2 and HID 1.11
// specifications, with the values a typical USB 2.0 webcam gives (an MJPEG
// and a YUY2 format, three frame sizes each, six isochronous alternate
// settings from 128 to 3 x 1024 bytes), and a CDC ACM serial port and a boot
// mouse after it, as the Sinden Lightgun is said to have. Not the gun's own
// bytes: those are what the first test build logs.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace
{
struct Bytes
{
	std::vector<uint8_t> b;
	Bytes& add(std::initializer_list<int> v)
	{
		for (int x : v)
			b.push_back((uint8_t)x);
		return *this;
	}
	Bytes& u16(unsigned v) { return add({ (int)(v & 0xFF), (int)(v >> 8) }); }
	Bytes& u32(uint32_t v) { return add({ (int)(v & 0xFF), (int)((v >> 8) & 0xFF), (int)((v >> 16) & 0xFF), (int)(v >> 24) }); }
};

void frameDescriptor(Bytes& c, int subtype, int index, int w, int h, std::initializer_list<uint32_t> intervals)
{
	Bytes f;
	f.add({ 0x24, subtype, index, 0x00 }).u16((unsigned)w).u16((unsigned)h);
	f.u32((uint32_t)w * h * 16 * 5).u32((uint32_t)w * h * 16 * 30).u32((uint32_t)w * h * 2).u32(*intervals.begin());
	f.add({ (int)intervals.size() });
	for (uint32_t i : intervals)
		f.u32(i);
	c.b.push_back((uint8_t)(f.b.size() + 1));
	c.b.insert(c.b.end(), f.b.begin(), f.b.end());
}

std::vector<uint8_t> gunConfiguration(unsigned hidReportLength = 52)
{
	Bytes c;
	c.add({ 9, 0x02, 0, 0, 5, 1, 0, 0x80, 250 });		// configuration: 5 interfaces, bus-powered, 500 mA
	c.add({ 8, 0x0B, 0, 2, 0x0E, 0x03, 0x00, 2 });		// a video function: interfaces 0 and 1
	c.add({ 9, 0x04, 0, 0, 1, 0x0E, 0x01, 0x00, 2 });	// interface 0: video control
	c.add({ 13, 0x24, 0x01 }).u16(0x0110).u16(0x0033).u32(48000000).add({ 1, 1 });	// UVC 1.10 header
	c.add({ 18, 0x24, 0x02, 1 }).u16(0x0201).add({ 0, 0 }).u16(0).u16(0).u16(0).add({ 3, 0x0E, 0x00, 0x00 });	// camera
	c.add({ 11, 0x24, 0x05, 2, 1 }).u16(0x4000).add({ 2, 0x7F, 0x15, 0 });	// processing unit
	c.add({ 9, 0x24, 0x03, 3 }).u16(0x0101).add({ 0, 2, 0 });	// output terminal (streaming)
	c.add({ 7, 0x05, 0x83, 0x03 }).u16(16).add({ 8 });	// interrupt in
	c.add({ 5, 0x25, 0x03 }).u16(16);
	c.add({ 9, 0x04, 1, 0, 0, 0x0E, 0x02, 0x00, 0 });	// interface 1: video streaming, no bandwidth
	c.add({ 15, 0x24, 0x01, 2 }).u16(0).add({ 0x81, 0, 3, 2, 1, 0, 1, 0, 0 });	// input header: 2 formats, endpoint 0x81
	c.add({ 11, 0x24, 0x06, 1, 3, 1, 1, 0, 0, 0, 0 });	// format 1: MJPEG, 3 frames
	frameDescriptor(c, 0x07, 1, 640, 480, { 333333, 666666 });
	frameDescriptor(c, 0x07, 2, 320, 240, { 166666, 333333 });
	frameDescriptor(c, 0x07, 3, 160, 120, { 166666 });
	c.add({ 6, 0x24, 0x0D, 1, 1, 4 });
	c.add({ 27, 0x24, 0x04, 2, 3, 'Y', 'U', 'Y', '2', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
			16, 1, 0, 0, 0, 0 });	// format 2: YUY2, 3 frames
	frameDescriptor(c, 0x05, 1, 640, 480, { 666666 });
	frameDescriptor(c, 0x05, 2, 320, 240, { 333333, 166666 });
	frameDescriptor(c, 0x05, 3, 176, 144, { 333333 });
	c.add({ 6, 0x24, 0x0D, 1, 1, 4 });
	const unsigned sizes[] = { 128, 512, 1024, 0x0B00 /* 2 x 768 */, 0x1380 /* 3 x 896 */, 0x1400 /* 3 x 1024 */ };
	for (unsigned k = 0; k < 6; k++)
	{
		c.add({ 9, 0x04, 1, (int)k + 1, 1, 0x0E, 0x02, 0x00, 0 });
		c.add({ 7, 0x05, 0x81, 0x05 }).u16(sizes[k]).add({ 1 });
	}
	c.add({ 8, 0x0B, 2, 2, 0x02, 0x02, 0x01, 0 });		// a CDC function: interfaces 2 and 3
	c.add({ 9, 0x04, 2, 0, 1, 0x02, 0x02, 0x01, 0 });	// interface 2: CDC ACM
	c.add({ 5, 0x24, 0x00 }).u16(0x0110);
	c.add({ 5, 0x24, 0x01, 0x01, 3 });
	c.add({ 4, 0x24, 0x02, 0x06 });
	c.add({ 5, 0x24, 0x06, 2, 3 });
	c.add({ 7, 0x05, 0x84, 0x03 }).u16(16).add({ 64 });
	c.add({ 9, 0x04, 3, 0, 2, 0x0A, 0x00, 0x00, 0 });	// interface 3: CDC data
	c.add({ 7, 0x05, 0x05, 0x02 }).u16(64).add({ 0 });
	c.add({ 7, 0x05, 0x86, 0x02 }).u16(64).add({ 0 });
	c.add({ 9, 0x04, 4, 0, 1, 0x03, 0x01, 0x02, 0 });	// interface 4: a boot mouse
	c.add({ 9, 0x21 }).u16(0x0111).add({ 0, 1, 0x22 }).u16(hidReportLength);
	c.add({ 7, 0x05, 0x87, 0x03 }).u16(8).add({ 1 });
	const unsigned total = (unsigned)c.b.size();
	c.b[2] = (uint8_t)(total & 0xFF);
	c.b[3] = (uint8_t)(total >> 8);
	return c.b;
}

}
