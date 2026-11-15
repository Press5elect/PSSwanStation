/*
	PSSwanStation - the Sinden Lightgun's USB, the parts that need no device:
	descriptors decoded, the camera's format and alternate setting chosen,
	its UVC stream put together into pictures, the serial port's frames and
	the buttons' HID reports.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	src/ps5/sinden_ps5.cpp reads the gun with these; ps5/tests/sinden-usb
	checks them on a PC. The USB, UVC, HID and CDC facts are their
	specifications' (USB 2.0 chapter 9; USB Video Class 1.1 and 1.5; HID 1.11;
	CDC 1.2 and its PSTN subclass). The serial protocol is the one MrLightgun's
	MiSTerSindenDriver (Tools/gun_serial.py, an open tool) speaks: fixed 7-byte
	frames AA cmd p1 p2 p3 p4 BB at 115200 baud, answers as plain bytes.
*/
#pragma once

#include "sinden.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fe::sinden::usb
{

// "01 02 03 04 05 06 07 08 090a..." for the boot log: bytes in hex, a space
// every 8, at most `most` of them ("..." after).
std::string hex(const uint8_t *p, size_t n, size_t most = 64);

// ------------------------------------------------------------------ descriptors
struct DeviceDescriptor
{
	bool ok = false;
	uint16_t bcdUSB = 0, vendor = 0, product = 0, bcdDevice = 0;
	uint8_t cls = 0, subclass = 0, protocol = 0, maxPacket0 = 0;
	uint8_t iManufacturer = 0, iProduct = 0, iSerial = 0, configurations = 0;
};
bool parseDevice(const uint8_t *d, size_t n, DeviceDescriptor& out);
std::string describeDevice(const DeviceDescriptor& d);
// "video", "HID", "CDC" ... for a class code.
const char *className(uint8_t cls);

enum EndpointType { Control = 0, Isochronous = 1, Bulk = 2, Interrupt = 3 };

struct Endpoint
{
	uint8_t address = 0, attributes = 0, interval = 0;
	uint16_t maxPacket = 0;
	int type() const { return attributes & 3; }
	bool in() const { return (address & 0x80) != 0; }
	// What one (micro)frame carries: the packet size times its transactions
	// (a high-speed periodic endpoint may have up to 3).
	uint32_t bytesPerInterval() const { return (maxPacket & 0x7FFu) * (1u + ((maxPacket >> 11) & 3u)); }
};

// What a UVC picture is made of.
enum class Pixels { Unknown, Yuy2, Nv12, Grey, Mjpeg };
const char *pixelsName(Pixels p);

struct UvcFrame
{
	uint8_t index = 0;
	uint16_t width = 0, height = 0;
	uint32_t maxFrameBytes = 0, defaultInterval = 0;
	bool continuous = false;
	std::vector<uint32_t> intervals;			// in 100 ns, discrete
	uint32_t minInterval = 0, maxInterval = 0, step = 0;	// continuous
	uint32_t shortestInterval() const;
};

struct UvcFormat
{
	uint8_t index = 0, subtype = 0, bitsPerPixel = 0, defaultFrame = 0;
	uint8_t guid[16] = {};
	Pixels pixels = Pixels::Unknown;
	std::vector<UvcFrame> frames;
};

// One interface descriptor (one alternate setting) and what follows it.
struct AltSetting
{
	uint8_t number = 0, alt = 0, cls = 0, subclass = 0, protocol = 0;
	std::vector<Endpoint> endpoints;
	uint16_t hidReportLength = 0;	// HID: its report descriptor's length
	uint16_t uvcVersion = 0;		// UVC video control: bcdUVC
	uint32_t uvcClock = 0;
	uint8_t vsEndpoint = 0;			// UVC video streaming: the endpoint its input header names
	std::vector<UvcFormat> formats;	// UVC video streaming, alternate setting 0
	int cdcControl = -1, cdcData = -1;	// CDC: the union's interfaces
	const Endpoint *endpoint(int type, bool in) const;
};

struct Configuration
{
	bool ok = false;
	uint8_t value = 0, attributes = 0, maxPower = 0, interfaceCount = 0;
	uint16_t totalLength = 0;
	std::vector<AltSetting> alts;	// every interface descriptor, in order
	const AltSetting *find(uint8_t number, uint8_t alt = 0) const;
	// The interface's index in the configuration (the order the interfaces
	// first appear in), which FreeBSD's ugen ioctls take.
	int indexOf(uint8_t number) const;
	uint16_t uvcVersion() const;
	bool hasVideo() const;			// a video streaming interface
};

// Decodes a configuration descriptor and everything after it. `lines`, when
// given, gets one line for each descriptor, as the boot log shows them.
bool parseConfiguration(const uint8_t *d, size_t n, Configuration& out, std::vector<std::string> *lines = nullptr);

// The gun's parts in a configuration.
struct HidPart
{
	uint8_t iface = 0, subclass = 0, protocol = 0;
	uint16_t reportLength = 0;
	Endpoint in;
};
struct Parts
{
	int serialControl = -1, serialData = -1;	// CDC ACM: the control and the data interface
	Endpoint serialIn, serialOut;
	std::vector<HidPart> hid;
	int videoControl = -1, videoStreaming = -1;
};
Parts findParts(const Configuration& c);

// ------------------------------------------------------------------ the camera's choice
struct StreamChoice
{
	bool ok = false;
	std::string why;		// why not, or how it was chosen
	uint8_t formatIndex = 0, frameIndex = 0;
	Pixels pixels = Pixels::Unknown;
	uint16_t width = 0, height = 0;
	uint32_t interval = 0;	// 100 ns
	uint32_t maxFrameBytes = 0;
};
// The format and the frame size for the tracker: the size closest to the one
// wanted (320 x 240), no larger than the most; uncompressed (YUY2, NV12,
// grey) preferred to MJPEG at the same size, and the shortest frame interval
// offered for it.
StreamChoice chooseStream(const Configuration& c, uint8_t streamingInterface, int wantWidth = 320, int wantHeight = 240,
		int mostWidth = 640, int mostHeight = 480);

struct AltChoice
{
	bool ok = false;
	bool bulk = false;		// the stream is on a bulk endpoint (alternate setting 0)
	std::string why;
	uint8_t alt = 0;
	Endpoint endpoint;
};
// The alternate setting whose endpoint carries `payload` bytes a (micro)frame
// with the least bandwidth (the device's dwMaxPayloadTransferSize); the
// largest when none is big enough or the payload is 0.
AltChoice chooseAlt(const Configuration& c, uint8_t streamingInterface, uint32_t payload);

// UVC's video probe and commit control (UVC 1.1 4.3.1.1).
struct Probe
{
	uint16_t hint = 0;
	uint8_t formatIndex = 0, frameIndex = 0;
	uint32_t interval = 0;
	uint16_t keyFrameRate = 0, pFrameRate = 0, compQuality = 0, compWindowSize = 0, delay = 0;
	uint32_t maxVideoFrameSize = 0, maxPayloadTransferSize = 0;
	uint32_t clockFrequency = 0;
	uint8_t framingInfo = 0, preferredVersion = 0, minVersion = 0, maxVersion = 0;
};
// Its length for a UVC version: 26 bytes (1.0), 34 (1.1), 48 (1.5).
size_t probeLength(uint16_t bcdUVC);
void packProbe(const Probe& p, uint8_t *out, size_t n);
Probe unpackProbe(const uint8_t *d, size_t n);
std::string describeProbe(const Probe& p, size_t n);

// ------------------------------------------------------------------ the stream
// Puts UVC payloads (each isochronous packet, or each bulk transfer: a
// header, then data) together into pictures. A picture ends at the header's
// end-of-frame bit or when the frame bit (FID) changes; one with the error
// bit set, one that overflowed, and an uncompressed one shorter than its size
// are dropped. Its memory is set aside in begin(), none per picture.
class Assembler
{
public:
	struct Counts
	{
		uint64_t payloads = 0, empty = 0, badHeader = 0, errorBit = 0, bytes = 0;
		uint64_t pictures = 0, dropped = 0, droppedShort = 0, droppedError = 0, droppedOverflow = 0, droppedNotJpeg = 0;
		uint64_t fidChanges = 0, endOfFrame = 0;
	};

	void begin(Pixels pixels, uint16_t width, uint16_t height, size_t capacity);
	// One payload. True when a picture is whole: picture() and pictureBytes()
	// hold it until the next true.
	bool payload(const uint8_t *p, size_t n);
	const uint8_t *picture() const { return done_.data(); }
	size_t pictureBytes() const { return doneBytes_; }
	const Counts& counts() const { return counts_; }
	// The last picture dropped, said in words.
	const std::string& lastDrop() const { return lastDrop_; }
	// The bytes an uncompressed picture has (0 for MJPEG).
	size_t expectedBytes() const;
	// FFmpeg reads past a picture's end: this much room after it, zeroed.
	static constexpr size_t Padding = 64;

private:
	bool finish();

	Pixels pixels_ = Pixels::Unknown;
	uint16_t width_ = 0, height_ = 0;
	std::vector<uint8_t> filling_, done_;
	size_t fillBytes_ = 0, doneBytes_ = 0, capacity_ = 0;
	int fid_ = -1;
	bool bad_ = false, overflow_ = false;
	Counts counts_;
	std::string lastDrop_;
};

// The brightness of an uncompressed picture, into `luma` (rows `stride`
// apart). False for MJPEG (src/sinden_mjpeg.cpp) or a size that does not fit.
bool lumaFrom(Pixels pixels, const uint8_t *picture, size_t bytes, int width, int height, uint8_t *luma, int stride);

// MJPEG to brightness with FFmpeg's decoder (src/sinden_mjpeg.cpp). Its
// buffers are set aside by FFmpeg once and reused; the input must have
// Assembler::Padding bytes after it.
class Mjpeg
{
public:
	~Mjpeg();
	bool open(std::string& error);
	void close();
	// False with `error` saying why (the first time: FFmpeg's words).
	bool decode(const uint8_t *jpeg, size_t bytes, uint8_t *luma, int width, int height, int stride, std::string& error);

private:
	void *context_ = nullptr, *frame_ = nullptr, *packet_ = nullptr;
};

// Pictures a second, over a window.
struct Rate
{
	double began = -1, last = 0;
	unsigned count = 0;
	void note(double t)
	{
		if (began < 0)
			began = t;
		last = t;
		count++;
	}
	double perSecond() const { return count > 1 && last > began ? (count - 1) / (last - began) : 0; }
	double seconds() const { return began < 0 ? 0 : last - began; }
};

// ------------------------------------------------------------------ the serial port
namespace serial
{
// The commands read here; 103 (which writes a character of the camera's name
// into the gun) is never sent.
enum : uint8_t { Firmware = 101, CameraName = 102, CalibrationX = 104, CalibrationY = 105 };
std::array<uint8_t, 7> frame(uint8_t command, uint8_t p1 = 0, uint8_t p2 = 0, uint8_t p3 = 0, uint8_t p4 = 0);
// The bytes the gun answers a command with (gun_serial.py's reads).
size_t answerLength(uint8_t command);
const char *commandName(uint8_t command);
// "1.9" from its two bytes.
std::string firmware(const uint8_t *d, size_t n);
// The camera's name: 15 bytes, trailing spaces and zeros cut.
std::string cameraName(const uint8_t *d, size_t n);
// Two bytes, big-endian: raw, and (raw - 10000) / 100.
bool calibration(const uint8_t *d, size_t n, int& raw, double& value);
// CDC SET_LINE_CODING's 7 bytes: the rate, 1 stop bit, no parity, 8 bits.
void lineCoding(uint32_t baud, uint8_t out[7]);
std::string describeLineCoding(const uint8_t *d, size_t n);
}

// ------------------------------------------------------------------ the buttons
struct HidField
{
	enum Kind { Other, MouseButtons, JoystickButtons, KeyModifiers, Keys };
	Kind kind = Other;
	uint8_t reportId = 0;
	uint32_t bit = 0;		// from the report's first byte after its ID
	uint8_t size = 0;		// bits each
	uint16_t count = 0;
	uint16_t page = 0, usageMin = 0, usageMax = 0;
	int32_t logicalMin = 0;
};

struct HidLayout
{
	bool ok = false;
	bool reportIds = false;
	std::vector<HidField> fields;	// buttons and keys only
	std::string summary;			// for the boot log
};
HidLayout parseHid(const uint8_t *d, size_t n);
// The boot protocol's fixed reports (HID 1.11 appendix B): 1 keyboard, 2 mouse.
HidLayout bootLayout(uint8_t protocol);
// One report into `io`: only what this report carries changes (a keyboard
// report the keys, a mouse report the mouse buttons). False when the layout
// has nothing for it.
bool decodeHid(const HidLayout& layout, const uint8_t *r, size_t n, Buttons& io);
std::string describeButtons(const Buttons& b);

// ------------------------------------------------------------------ libSceUsbd's transfers
// libSceUsbd mirrors libusb 1.0, whose transfer structure is not declared for
// the console, and whose layout differs between libusb's own and FreeBSD's.
// It is found on the console instead: a transfer is filled by the library's
// own fill functions with values that cannot be mistaken, and each value is
// looked for in the structure's bytes.
struct TransferValues
{
	uint64_t handle = 0, callback = 0, userData = 0, buffer = 0;
	uint32_t length = 0, timeout = 0, packets = 0, packetLength = 0;
	uint8_t endpoint = 0;
};
struct TransferLayout
{
	bool ok = false;
	std::string why;
	int handle = -1, endpoint = -1, timeout = -1, status = -1, length = -1, actual = -1;
	int callback = -1, userData = -1, buffer = -1, packets = -1, packetDescriptors = -1, packetStride = 0;
};
// `mem`: the transfer's bytes after the fill and after the packets' lengths
// were set to values.packetLength.
TransferLayout findTransferLayout(const uint8_t *mem, size_t n, const TransferValues& values);
std::string describeLayout(const TransferLayout& l);

}
