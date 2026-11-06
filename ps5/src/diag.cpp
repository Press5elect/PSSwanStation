/*
	PSSwanStation - start-up record and crash report.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	PSFlyCast's (shell/ps5/ps5_diag.cpp), which follows PS5 RetroArch's
	src/crash_report.cpp. The console only says that a title stopped, not
	where. This file writes what the next run needs to fix it, in the title's
	own folder:

	- every start-up step, as a line of the file. The lines go through
	  memory to a writer thread of their own (as OGBox's boot log, OGBox by
	  Press5elect): a line written straight to the file waits for the file
	  system, on whatever thread wrote it, and the frame thread writes some.
	  The writer puts the file on the disk every two seconds that had lines;
	  a crash report writes what memory still holds, then itself, straight;
	- on a fatal signal, the faulting instruction as an offset in eboot.bin
	  (ps5/tools/symbolize.sh turns it into a function), the registers, the
	  return addresses on the stack and the last step reached;
	- on std::terminate, the exception that was not caught.

	The machine context is the console's: the payload SDK fork's ucontext_t
	places uc_mcontext where the console does (ps5platform/context.h).
*/
#include "fe.h"

#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <initializer_list>
#include <typeinfo>

#include <cxxabi.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(SWANSTATION_PS5)
#include <pthread_np.h>
#include <ucontext.h>
#include <ps5platform/context.h>

extern "C"
{
int sceKernelSendNotificationRequest(int, void *, size_t, int);
int sceKernelAvailableFlexibleMemorySize(size_t *size);
// ps5/runtime/ps5-title.ld
extern char __ps5_text_start[];
extern char __ps5_text_end[];
}
#endif

namespace fe::options
{
struct Frontend;
Frontend& frontend();
}

namespace fe::diag
{
namespace
{
int logFd = -1;
// Marks made before the file is open, written when it opens.
char early[8192];
size_t earlyUsed;
// The last mark, for the crash report.
char lastMarkText[256] = "(none)";
timespec startTime;
volatile bool notices = false;

// The lines on their way to the writer thread. Fixed storage: the crash
// report empties it from a signal handler.
constexpr size_t RingBytes = 1u << 20;
char ring[RingBytes];
size_t ringHead, ringLength;		// the oldest byte, and how many
unsigned long long ringDropped;		// bytes of lines that found it full
bool writerBusy;					// a chunk taken out, being written
pthread_mutex_t ringLock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ringFilled = PTHREAD_COND_INITIALIZER;
pthread_cond_t ringEmptied = PTHREAD_COND_INITIALIZER;
volatile bool writerUp;

void writeAll(const char *text, size_t length)
{
	if (logFd < 0)
	{
		const size_t room = sizeof(early) - earlyUsed;
		const size_t n = length < room ? length : room;
		memcpy(early + earlyUsed, text, n);
		earlyUsed += n;
		return;
	}
	while (length > 0)
	{
		const ssize_t n = write(logFd, text, length);
		if (n <= 0)
			return;
		text += n;
		length -= n;
	}
}

// Signal-safe enough: vsnprintf into a stack buffer and write().
void writef(const char *format, ...) __attribute__((format(printf, 1, 2)));
void writef(const char *format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	const int n = vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (n > 0)
		writeAll(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

void ringPut(const char *text, size_t length)
{
	pthread_mutex_lock(&ringLock);
	if (ringLength + length > RingBytes)
		ringDropped += length;
	else
	{
		size_t at = (ringHead + ringLength) % RingBytes;
		for (size_t done = 0; done < length;)
		{
			const size_t n = length - done < RingBytes - at ? length - done : RingBytes - at;
			memcpy(ring + at, text + done, n);
			done += n;
			at = (at + n) % RingBytes;
		}
		ringLength += length;
	}
	pthread_cond_signal(&ringFilled);
	pthread_mutex_unlock(&ringLock);
}

// A line: to the writer when it runs, else straight to the file.
void lineOut(const char *text, size_t length)
{
	if (writerUp && logFd >= 0)
		ringPut(text, length);
	else
		writeAll(text, length);
}

void *writerMain(void *)
{
#if defined(SWANSTATION_PS5)
	pthread_set_name_np(pthread_self(), "psswan-log");
#endif
	static char chunk[64 * 1024];
	timespec lastSync;
	clock_gettime(CLOCK_MONOTONIC, &lastSync);
	bool unsynced = false;
	pthread_mutex_lock(&ringLock);
	for (;;)
	{
		if (ringLength == 0)
		{
			timespec until;
			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_nsec += 100 * 1000000L;
			if (until.tv_nsec >= 1000000000L)
			{
				until.tv_sec++;
				until.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&ringFilled, &ringLock, &until);
		}
		while (ringLength > 0)
		{
			size_t n = ringLength < sizeof(chunk) ? ringLength : sizeof(chunk);
			if (n > RingBytes - ringHead)
				n = RingBytes - ringHead;
			memcpy(chunk, ring + ringHead, n);
			ringHead = (ringHead + n) % RingBytes;
			ringLength -= n;
			writerBusy = true;
			pthread_mutex_unlock(&ringLock);
			writeAll(chunk, n);
			unsynced = true;
			pthread_mutex_lock(&ringLock);
			writerBusy = false;
		}
		if (ringDropped != 0)
		{
			char note[96];
			const int k = snprintf(note, sizeof(note), "[log: %llu bytes of lines left out: the file system was slow]\n",
					ringDropped);
			ringDropped = 0;
			pthread_mutex_unlock(&ringLock);
			if (k > 0)
				writeAll(note, (size_t)k);
			pthread_mutex_lock(&ringLock);
		}
		pthread_cond_broadcast(&ringEmptied);
		// On the disk every two seconds that had lines: a console that stops
		// whole loses what its cache held. Here, in no other thread's way.
		timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (unsynced && now.tv_sec - lastSync.tv_sec >= 2)
		{
			lastSync = now;
			unsynced = false;
			pthread_mutex_unlock(&ringLock);
			fsync(logFd);
			pthread_mutex_lock(&ringLock);
		}
	}
	return nullptr;
}

// From the crash report: what memory holds into the file, before the
// report's own lines - unless another thread holds the lock (a signal
// handler cannot wait for it).
void drainForCrash()
{
	if (logFd < 0 || pthread_mutex_trylock(&ringLock) != 0)
		return;
	while (ringLength > 0)
	{
		const size_t n = ringLength < RingBytes - ringHead ? ringLength : RingBytes - ringHead;
		writeAll(ring + ringHead, n);
		ringHead = (ringHead + n) % RingBytes;
		ringLength -= n;
	}
	writerUp = false;	// what follows goes straight to the file
	pthread_mutex_unlock(&ringLock);
}

void sendNotification(const char *text)
{
#if defined(SWANSTATION_PS5)
	if (!notices)
		return;
	struct
	{
		char useless[45];
		char message[3075];
	} request;
	memset(&request, 0, sizeof(request));
	strncpy(request.message, text, sizeof(request.message) - 1);
	sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
#else
	(void)text;
#endif
}

#if defined(SWANSTATION_PS5)
// eboot.bin's load address: .text starts 16 bytes into the image.
uintptr_t imageBase()
{
	return reinterpret_cast<uintptr_t>(__ps5_text_start) - 0x10;
}

bool inText(uintptr_t address)
{
	return address >= reinterpret_cast<uintptr_t>(__ps5_text_start)
			&& address < reinterpret_cast<uintptr_t>(__ps5_text_end);
}

void report(int signal, siginfo_t *info, void *contextPointer)
{
	const ucontext_t *context = static_cast<const ucontext_t *>(contextPointer);
	const mcontext_t& mc = context->uc_mcontext;
	const uintptr_t rip = mc.mc_rip;
	const uintptr_t rsp = mc.mc_rsp;
	const uintptr_t base = imageBase();
	size_t flexible = 0;
	sceKernelAvailableFlexibleMemorySize(&flexible);
	drainForCrash();

	writef("\n=== CRASH: signal %d (%s) ===\n", signal,
			signal == SIGSEGV ? "SIGSEGV" : signal == SIGBUS ? "SIGBUS" : signal == SIGILL ? "SIGILL"
			: signal == SIGFPE ? "SIGFPE" : signal == SIGABRT ? "SIGABRT" : signal == SIGSYS ? "SIGSYS"
			: signal == SIGTRAP ? "SIGTRAP" : "?");
	writef("build %d; last step: %s\n", BuildNumber, lastMarkText);
	writef("fault address: 0x%llx\n", (unsigned long long)(uintptr_t)info->si_addr);
	if (inText(rip))
		writef("rip: 0x%llx = eboot+0x%llx\n", (unsigned long long)rip, (unsigned long long)(rip - base));
	else
		writef("rip: 0x%llx (outside eboot.bin; eboot at 0x%llx)\n", (unsigned long long)rip,
				(unsigned long long)base);
	writef("rsp: 0x%llx  flexible memory free: %zu KiB\n", (unsigned long long)rsp, flexible >> 10);
	writef("rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx\n",
			(unsigned long long)mc.mc_rax, (unsigned long long)mc.mc_rbx, (unsigned long long)mc.mc_rcx,
			(unsigned long long)mc.mc_rdx, (unsigned long long)mc.mc_rsi, (unsigned long long)mc.mc_rdi,
			(unsigned long long)mc.mc_rbp);
	writef("r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n",
			(unsigned long long)mc.mc_r8, (unsigned long long)mc.mc_r9, (unsigned long long)mc.mc_r10,
			(unsigned long long)mc.mc_r11, (unsigned long long)mc.mc_r12, (unsigned long long)mc.mc_r13,
			(unsigned long long)mc.mc_r14, (unsigned long long)mc.mc_r15);

	// The return addresses: every stack word from rsp up that points into
	// eboot.bin's code.
	if (rsp >= 4096 && (rsp & 7) == 0)
	{
		pthread_attr_t attributes;
		void *stack = nullptr;
		size_t stackSize = 0;
		if (pthread_attr_init(&attributes) == 0)
		{
			if (pthread_attr_get_np(pthread_self(), &attributes) == 0)
				pthread_attr_getstack(&attributes, &stack, &stackSize);
			pthread_attr_destroy(&attributes);
		}
		const uintptr_t top = reinterpret_cast<uintptr_t>(stack) + stackSize;
		if (stack != nullptr && rsp < top)
		{
			const uintptr_t end = top - rsp > 65536 ? rsp + 65536 : top;
			writeAll("stack (eboot offsets):", 22);
			int found = 0;
			for (uintptr_t at = rsp; at + 8 <= end && found < 64; at += 8)
			{
				const uintptr_t word = *reinterpret_cast<const uintptr_t *>(at);
				if (!inText(word))
					continue;
				writef(" %llx", (unsigned long long)(word - base));
				found++;
			}
			writeAll("\n", 1);
		}
	}
	writeAll("=== end of crash report ===\n", 28);
	if (logFd >= 0)
		fsync(logFd);
	for (FILE *stream : { stdout, stderr })
		if (ftrylockfile(stream) == 0)
		{
			fflush(stream);
			funlockfile(stream);
		}
	sendNotification("PSSwanStation stopped. Please send psswanstation-boot.log from its folder.");
	std::signal(signal, SIG_DFL);
	std::raise(signal);
}
#else
void report(int signal, siginfo_t *info, void *)
{
	drainForCrash();
	writef("\n=== CRASH: signal %d ===\nlast step: %s\nfault address: %p\n", signal, lastMarkText, info->si_addr);
	std::signal(signal, SIG_DFL);
	std::raise(signal);
}
#endif

void onTerminate()
{
	const std::type_info *type = abi::__cxa_current_exception_type();
	if (type == nullptr)
	{
		mark("terminate: called with no exception");
	}
	else
	{
		int status = 0;
		char *name = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
		mark("terminate: uncaught exception %s", status == 0 && name != nullptr ? name : type->name());
		free(name);
	}
	std::abort();
}

} // namespace

bool open(const std::string& dir)
{
	if (logFd >= 0)
		return true;
	const std::string a = dir + "psswanstation-boot.log";
	const std::string b = dir + "psswanstation-boot.1.log";
	const std::string c = dir + "psswanstation-boot.2.log";
	rename(b.c_str(), c.c_str());
	rename(a.c_str(), b.c_str());
	const int fd = ::open(a.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
	if (fd < 0)
		return false;
	fchmod(fd, 0666);
	logFd = fd;
	writeAll(early, earlyUsed);
	earlyUsed = 0;
	fsync(logFd);
	pthread_t writer;
	if (pthread_create(&writer, nullptr, writerMain, nullptr) == 0)
	{
		pthread_detach(writer);
		writerUp = true;
	}
	return true;
}

void flush()
{
	if (!writerUp)
		return;
	pthread_mutex_lock(&ringLock);
	for (int i = 0; i < 40 && (ringLength > 0 || writerBusy); i++)
	{
		pthread_cond_signal(&ringFilled);
		timespec until;
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_nsec += 50 * 1000000L;
		if (until.tv_nsec >= 1000000000L)
		{
			until.tv_sec++;
			until.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&ringEmptied, &ringLock, &until);
	}
	pthread_mutex_unlock(&ringLock);
	if (logFd >= 0)
		fsync(logFd);
}

void mark(const char *format, ...)
{
	if (startTime.tv_sec == 0 && startTime.tv_nsec == 0)
		clock_gettime(CLOCK_MONOTONIC, &startTime);
	char text[sizeof(lastMarkText)];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	memcpy(lastMarkText, text, sizeof(lastMarkText));
	timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	const double seconds = (t.tv_sec - startTime.tv_sec) + (t.tv_nsec - startTime.tv_nsec) / 1e9;
	char line[sizeof(text) + 32];
	const int n = snprintf(line, sizeof(line), "[%8.3f] %s\n", seconds, text);
	if (n > 0)
		lineOut(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
#if !defined(SWANSTATION_PS5)
	if (getenv("SWANSTATION_VERBOSE") != nullptr)
		fprintf(stderr, "[boot %8.3f] %s\n", seconds, text);
#endif
}

void notify(const char *format, ...)
{
	char text[512];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	mark("notice: %s", text);
	sendNotification(text);
}

const char *lastMark()
{
	return lastMarkText;
}

void setNotifications(bool show)
{
	notices = show;
}

void installCrashHandler()
{
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = report;
	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigemptyset(&action.sa_mask);
	for (const int signal : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS, SIGTRAP })
		sigaction(signal, &action, nullptr);
	std::set_terminate(onTerminate);
}

}
