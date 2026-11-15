/*
 * ps5-native-app-boilerplate - One-request elevation helper.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Changed for PSSwanStation (SPDX-FileCopyrightText: 2026 Press5elect): the title ID, PPSA99248;
 * and the root's reference counts kept right, as Crux2 v1.0.19 has it from OGBox 0.0.2
 * (Press5elect/Crux2 and Press5elect/OGBox, ps5/elevation/payload/main.cpp). Additional term
 * (GPL-3.0 section 7(b)) of that change: keep the attribution "Crux2 by Press5elect".
 */
#include "../protocol.hpp"

#include <array>
#include <cstring>

extern "C"
{
#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <ps5/payload.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
}

namespace
{
using elevation::Capability;
using elevation::Status;
using elevation::wire::Kind;
using elevation::wire::Message;

constexpr char target_title_id[] = "PPSA99248"; // PSSwanStation: the one title this helper serves
constexpr std::uint64_t system_auth_id = UINT64_C(0x4801000000000013);

struct AppInfo
{
    std::uint32_t app_id;
    std::uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c];
};
extern "C" int sceKernelGetAppInfo(pid_t pid, AppInfo *info);

struct Target
{
    std::intptr_t process = 0;
    std::intptr_t ucred = 0;
    std::intptr_t filedesc = 0;
};

struct State
{
    // uid, ruid, svuid, ngroups, rgid, svgid; the SDK's supported ucred layout.
    std::array<std::uint32_t, 6> identity{};
    std::uint64_t authority = 0;
    std::array<std::uint8_t, 16> caps{};
    std::array<std::uint8_t, 32> attrs{};
    std::intptr_t root = 0;
    std::intptr_t jail = 0;
    bool operator==(const State &) const = default;
};

bool kernel_pointer(std::intptr_t pointer) noexcept
{
    return (static_cast<std::uint64_t>(pointer) >> 48) == 0xffff;
}

bool find_target(std::uint32_t pid, Target &target) noexcept
{
    AppInfo info{};
    if (sceKernelGetAppInfo(static_cast<pid_t>(pid), &info) != 0 ||
        std::memcmp(info.title_id, target_title_id, sizeof(target_title_id)) != 0)
        return false;

    // Walk afresh instead of reusing the SDK's PID cache after the handshake.
    std::intptr_t process = 0;
    if (kernel_copyout(KERNEL_ADDRESS_ALLPROC, &process, sizeof(process)) != 0)
        return false;
    for (unsigned guard = 0; process != 0 && guard < 4096; ++guard)
    {
        std::uint32_t candidate = 0;
        if (!kernel_pointer(process) ||
            kernel_copyout(process + KERNEL_OFFSET_PROC_P_PID, &candidate, sizeof(candidate)) != 0)
            return false;
        if (candidate == pid)
        {
            target.process = process;
            return kernel_copyout(process + KERNEL_OFFSET_PROC_P_UCRED, &target.ucred,
                                  sizeof(target.ucred)) == 0 &&
                   kernel_copyout(process + KERNEL_OFFSET_PROC_P_FD, &target.filedesc,
                                  sizeof(target.filedesc)) == 0 &&
                   kernel_pointer(target.ucred) && kernel_pointer(target.filedesc);
        }
        if (kernel_copyout(process, &process, sizeof(process)) != 0)
            return false;
    }
    return false;
}

bool read_state(const Target &target, State &state) noexcept
{
    return kernel_copyout(target.ucred + KERNEL_OFFSET_UCRED_CR_UID, state.identity.data(),
                          sizeof(state.identity)) == 0 &&
           kernel_copyout(target.ucred + KERNEL_OFFSET_UCRED_CR_SCEAUTHID, &state.authority,
                          sizeof(state.authority)) == 0 &&
           kernel_copyout(target.ucred + KERNEL_OFFSET_UCRED_CR_SCECAPS, state.caps.data(),
                          state.caps.size()) == 0 &&
           kernel_copyout(target.ucred + KERNEL_OFFSET_UCRED_CR_SCEATTRS, state.attrs.data(),
                          state.attrs.size()) == 0 &&
           kernel_copyout(target.filedesc + KERNEL_OFFSET_FILEDESC_FD_RDIR, &state.root,
                          sizeof(state.root)) == 0 &&
           kernel_copyout(target.filedesc + KERNEL_OFFSET_FILEDESC_FD_JDIR, &state.jail,
                          sizeof(state.jail)) == 0;
}

// One pointer of a filedesc, written as it is (no reference is taken or released).
bool write_pointer(const Target &target, off_t offset, std::intptr_t value) noexcept
{
    std::intptr_t now = 0;
    return kernel_copyin(&value, target.filedesc + offset, sizeof(value)) == 0 &&
           kernel_copyout(target.filedesc + offset, &now, sizeof(now)) == 0 && now == value;
}

// The credentials (the cloned ucred is the process's own: no reference counts there).
bool write_credentials(const Target &target, const State &state) noexcept
{
    // Attempt every field, including during rollback after a partial write.
    int failures = 0;
    failures += kernel_copyin(state.identity.data(), target.ucred + KERNEL_OFFSET_UCRED_CR_UID,
                              sizeof(state.identity)) != 0;
    failures += kernel_copyin(&state.authority, target.ucred + KERNEL_OFFSET_UCRED_CR_SCEAUTHID,
                              sizeof(state.authority)) != 0;
    failures += kernel_copyin(state.caps.data(), target.ucred + KERNEL_OFFSET_UCRED_CR_SCECAPS,
                              state.caps.size()) != 0;
    failures += kernel_copyin(state.attrs.data(), target.ucred + KERNEL_OFFSET_UCRED_CR_SCEATTRS,
                              state.attrs.size()) != 0;
    return failures == 0;
}

bool same_credentials(const State &a, const State &b) noexcept
{
    return a.identity == b.identity && a.authority == b.authority && a.caps == b.caps && a.attrs == b.attrs;
}

bool send_message(const Message &message) noexcept
{
    return elevation::wire::transfer(reinterpret_cast<const std::uint8_t *>(&message),
                                     sizeof(message), [](const auto *bytes, std::size_t size)
                                     { return write(STDOUT_FILENO, bytes, size); });
}

bool receive_message(Message &message) noexcept
{
    return elevation::wire::transfer(reinterpret_cast<std::uint8_t *>(&message), sizeof(message),
                                     [](auto *bytes, std::size_t size)
                                     { return read(STDIN_FILENO, bytes, size); });
}

// Crux2 v1.0.19 (from OGBox 0.0.2; PSSwanStation from build 27): the console's root as the title's
// root without unbalancing the vnodes' reference counts. Before, the root vnode was written into the
// title's fd_rdir and fd_jdir as it is: the title's exit (fdescfree) then released two references on
// the root that it never took, and the sandbox's two were never released. Each elevated run that
// ended drained the root by two, and the console goes down once the root's count runs out (OGBox, which ends an
// elevated process at every game start, met it within a session). chroot is not there on the PS5
// (ENOSYS), so the kernel can't count them for us. This helper's own fd_rdir and fd_jdir hold
// counted references on the console's root (the payload loader's fork gave them): the helper and
// the title trade theirs. The title's slots then hold the helper's references on the root, the
// helper's the title's on the sandbox, and each exit releases what its slots hold: every count
// stays right whichever process ends first. Only when the helper's own root is not the console's
// is the old way taken, and said (ok_unbalanced).
bool find_process(pid_t pid, Target &target) noexcept
{
    std::intptr_t process = 0;
    if (kernel_copyout(KERNEL_ADDRESS_ALLPROC, &process, sizeof(process)) != 0)
        return false;
    for (unsigned guard = 0; process != 0 && guard < 4096; ++guard)
    {
        std::uint32_t candidate = 0;
        if (!kernel_pointer(process) ||
            kernel_copyout(process + KERNEL_OFFSET_PROC_P_PID, &candidate, sizeof(candidate)) != 0)
            return false;
        if (candidate == static_cast<std::uint32_t>(pid))
        {
            target.process = process;
            return kernel_copyout(process + KERNEL_OFFSET_PROC_P_FD, &target.filedesc,
                                  sizeof(target.filedesc)) == 0 &&
                   kernel_pointer(target.filedesc);
        }
        if (kernel_copyout(process, &process, sizeof(process)) != 0)
            return false;
    }
    return false;
}

Status grant_filesystem(const Target &target, const State &original) noexcept
{
    const std::intptr_t root = kernel_get_root_vnode();
    if (!kernel_pointer(root) || !kernel_pointer(original.root))
        return Status::unavailable;
    State desired = original;
    desired.identity.fill(0);
    desired.authority = system_auth_id;
    desired.caps.fill(0xff);
    desired.attrs[3] |= 0x80;

    State verified{};
    if (!write_credentials(target, desired) || !read_state(target, verified) ||
        !same_credentials(verified, desired))
    {
        if (write_credentials(target, original) && read_state(target, verified) &&
            same_credentials(verified, original))
            return Status::apply_failed;
        return Status::rollback_failed;
    }

    Target self{};
    std::intptr_t own_root = 0, own_jail = 0;
    const bool have_self =
        find_process(getpid(), self) &&
        kernel_copyout(self.filedesc + KERNEL_OFFSET_FILEDESC_FD_RDIR, &own_root, sizeof(own_root)) == 0 &&
        kernel_copyout(self.filedesc + KERNEL_OFFSET_FILEDESC_FD_JDIR, &own_jail, sizeof(own_jail)) == 0;
    if (have_self && own_root == root)
    {
        // the trade: the title's slots get the helper's counted root, the helper's the title's
        // counted sandbox (fd_jdir alike: the helper's may be the root or empty)
        const bool title_set = write_pointer(target, KERNEL_OFFSET_FILEDESC_FD_RDIR, own_root) &&
                               write_pointer(target, KERNEL_OFFSET_FILEDESC_FD_JDIR, own_jail);
        const bool self_set = title_set &&
                              write_pointer(self, KERNEL_OFFSET_FILEDESC_FD_RDIR, original.root) &&
                              write_pointer(self, KERNEL_OFFSET_FILEDESC_FD_JDIR, original.jail);
        if (self_set)
            return Status::ok;
        // undo what was traded, as it was
        write_pointer(self, KERNEL_OFFSET_FILEDESC_FD_RDIR, own_root);
        write_pointer(self, KERNEL_OFFSET_FILEDESC_FD_JDIR, own_jail);
        if (write_pointer(target, KERNEL_OFFSET_FILEDESC_FD_RDIR, original.root) &&
            write_pointer(target, KERNEL_OFFSET_FILEDESC_FD_JDIR, original.jail) &&
            write_credentials(target, original))
            return Status::apply_failed;
        return Status::rollback_failed;
    }

    // The helper's root is not the console's: the boilerplate's way (unbalanced, see above).
    if (write_pointer(target, KERNEL_OFFSET_FILEDESC_FD_RDIR, root) &&
        write_pointer(target, KERNEL_OFFSET_FILEDESC_FD_JDIR, root))
        return Status::ok_unbalanced;
    if (write_pointer(target, KERNEL_OFFSET_FILEDESC_FD_RDIR, original.root) &&
        write_pointer(target, KERNEL_OFFSET_FILEDESC_FD_JDIR, original.jail) &&
        write_credentials(target, original))
        return Status::apply_failed;
    return Status::rollback_failed;
}

Status handle_request(const Message &request) noexcept
{
    if (const auto error = elevation::wire::validate(request); error != Status::ok)
        return error;
    if (request.kind != Kind::request || request.status != Status::ok)
        return Status::invalid_request;
    const auto *args = payload_get_args();
    if (args == nullptr || args->kdata_base_addr == 0)
        return Status::unavailable;

    Target before{};
    State original{};
    if (!find_target(request.pid, before))
        return Status::target_mismatch;
    if (!read_state(before, original))
        return Status::unavailable;
    Message prepare = request;
    prepare.kind = Kind::prepare;
    Message prepared{};
    if (!send_message(prepare) || !receive_message(prepared))
        return Status::transport_error;
    if (!elevation::wire::matches(prepared, request, Kind::prepared))
        return Status::invalid_request;
    if (prepared.status != Status::ok)
        return Status::prepare_failed;

    Target target{};
    State cloned{};
    if (!find_target(request.pid, target) || target.process != before.process ||
        target.filedesc != before.filedesc)
        return Status::target_mismatch;
    // Never write into the preexisting, potentially shared credential. Do not
    // copy cr_prison or swap credential pointers ourselves.
    if (target.ucred == before.ucred || !read_state(target, cloned) || cloned != original)
        return Status::prepare_failed;

    switch (request.capability)
    {
    case Capability::filesystem:
        return grant_filesystem(target, original);
    default:
        return Status::unsupported_capability;
    }
}
} // namespace

int main()
{
    const timeval timeout{elevation::wire::io_timeout_us / 1'000'000,
                          elevation::wire::io_timeout_us % 1'000'000};
    if (setsockopt(STDIN_FILENO, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(STDOUT_FILENO, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
        return 1;
    Message request{};
    if (!receive_message(request))
        return 1;
    const auto result = handle_request(request);
    Message response{};
    response.kind = Kind::response;
    response.capability = request.capability;
    response.pid = request.pid;
    response.status = result;
    klog_printf("[sandbox-elevator] fw=%08x pid=%u capability=%u result=%u\n",
                kernel_get_fw_version(), request.pid, static_cast<unsigned>(request.capability),
                static_cast<unsigned>(result));
    return send_message(response) && result == Status::ok ? 0 : 1;
}
