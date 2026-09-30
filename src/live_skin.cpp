#include "live_skin.hpp"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))

// One-shot game-thread hook for a pattern-verified League ARM64 build.
// This uses the game's existing frame callback; it does not create or modify
// a Mach thread. It is intentionally version-locked.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <atomic>
#include <cctype>
#include <string>

#define IMAGE_BASE 0x100000000ULL

extern const unsigned char stallion_frame_hook_begin[], stallion_frame_hook_end[];
extern const unsigned char stallion_frame_mailbox_literal[], stallion_frame_return_literal[];
extern const unsigned char stallion_frame_replay_begin[];
extern const unsigned char stallion_persist_begin[], stallion_persist_end[];
extern const unsigned char stallion_persist_mailbox_literal[], stallion_persist_return_literal[];

__asm__(R"(
.text
.p2align 4
.globl _stallion_frame_hook_begin, _stallion_frame_hook_end
.globl _stallion_frame_mailbox_literal, _stallion_frame_return_literal, _stallion_frame_replay_begin
_stallion_frame_hook_begin:
    sub sp, sp, #0x120
    stp q0, q1, [sp, #0x00]
    stp q2, q3, [sp, #0x20]
    stp q4, q5, [sp, #0x40]
    stp q6, q7, [sp, #0x60]
    stp x0, x1, [sp, #0x80]
    stp x2, x3, [sp, #0x90]
    stp x4, x5, [sp, #0xa0]
    stp x6, x7, [sp, #0xb0]
    stp x8, x9, [sp, #0xc0]
    stp x10, x11, [sp, #0xd0]
    stp x12, x13, [sp, #0xe0]
    stp x14, x15, [sp, #0xf0]
    stp x16, x17, [sp, #0x100]
    str x30, [sp, #0x110]

    adr x16, Lmailbox_literal
    ldr x16, [x16]
    ldr w17, [x16]
    cmp w17, #1
    b.ne Lrestore
    mov w17, #2
    str w17, [x16]
    ldr x0, [x16, #8]
    ldr x0, [x0]
    cbz x0, Lno_actor
    add x1, x16, #0x100
    ldr w2, [x16, #4]
    mov w3, #0
    add x4, x16, #0x200
    ldr x17, [x16, #16]
    blr x17
    adr x16, Lmailbox_literal
    ldr x16, [x16]
    mov w17, #3
    str w17, [x16]
    b Lrestore
Lno_actor:
    mov w17, #4
    str w17, [x16]
Lrestore:
    ldp q0, q1, [sp, #0x00]
    ldp q2, q3, [sp, #0x20]
    ldp q4, q5, [sp, #0x40]
    ldp q6, q7, [sp, #0x60]
    ldp x0, x1, [sp, #0x80]
    ldp x2, x3, [sp, #0x90]
    ldp x4, x5, [sp, #0xa0]
    ldp x6, x7, [sp, #0xb0]
    ldp x8, x9, [sp, #0xc0]
    ldp x10, x11, [sp, #0xd0]
    ldp x12, x13, [sp, #0xe0]
    ldp x14, x15, [sp, #0xf0]
    ldp x16, x17, [sp, #0x100]
    ldr x30, [sp, #0x110]
    add sp, sp, #0x120

_stallion_frame_replay_begin:
    sub sp, sp, #0x170
    stp x28, x27, [sp, #0x120]
    stp x24, x23, [sp, #0x130]
    stp x22, x21, [sp, #0x140]
    adr x16, Lreturn_literal
    ldr x16, [x16]
    br x16
.p2align 3
Lmailbox_literal:
_stallion_frame_mailbox_literal:
    .quad 0
Lreturn_literal:
_stallion_frame_return_literal:
    .quad 0
_stallion_frame_hook_end:
)");

// The setup entry is called again when the local actor is rebuilt. Keep the
// selected skin at this call boundary instead of polling a transient actor field.
__asm__(R"(
.text
.p2align 4
.globl _stallion_persist_begin, _stallion_persist_end
.globl _stallion_persist_mailbox_literal, _stallion_persist_return_literal
_stallion_persist_begin:
    sub sp, sp, #0x10
    stp x16, x17, [sp]
    adr x16, Lstallion_mailbox_pointer
    ldr x16, [x16]
    ldr x17, [x16, #8]
    ldr x17, [x17]
    cmp x0, x17
    b.ne Lstallion_restore
    ldr w2, [x16]
Lstallion_restore:
    ldp x16, x17, [sp]
    add sp, sp, #0x10
    sub sp, sp, #0x80
    stp x24, x23, [sp, #0x40]
    stp x22, x21, [sp, #0x50]
    stp x20, x19, [sp, #0x60]
    adr x16, Lstallion_return_pointer
    ldr x16, [x16]
    br x16
.p2align 3
Lstallion_mailbox_pointer:
_stallion_persist_mailbox_literal:
    .quad 0
Lstallion_return_pointer:
_stallion_persist_return_literal:
    .quad 0
_stallion_persist_end:
)");

struct PersistentMailbox {
    uint32_t skin_id;
    uint32_t reserved;
    uint64_t actor_global;
};

struct Mailbox {
    uint32_t state; // 1 requested, 2 executing, 3 completed, 4 actor missing
    uint32_t skin_id;
    uint64_t actor_global;
    uint64_t setup_function;
    unsigned char reserved[0x100 - 24];
    unsigned char champion_string[24];
    unsigned char reserved2[0x200 - 0x100 - 24];
    char empty_override;
};

static int read_exact(mach_port_t task, uint64_t at, void *out, size_t size) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(task, at, size, (mach_vm_address_t)out, &got) == KERN_SUCCESS && got == size;
}
static int write_exact(mach_port_t task, uint64_t at, const void *data, size_t size) {
    return mach_vm_write(task, at, (vm_offset_t)data, (mach_msg_type_number_t)size) == KERN_SUCCESS;
}
static std::string champion_key(std::string_view name) {
    std::string key;
    for (unsigned char c : name) {
        if (std::isalnum(c)) key.push_back(static_cast<char>(std::tolower(c)));
    }
    // Riot's display names and internal actor names differ for these champions.
    if (key == "wukong") return "monkeyking";
    if (key == "nunuwillump") return "nunu";
    if (key == "renataglasc") return "renata";
    return key;
}
static bool actor_matches_champion(mach_port_t task, uint64_t actor,
                                   std::string_view expected_champion,
                                   uint32_t champion_name_offset) {
    if (expected_champion.empty()) return true;
    unsigned char raw[24] = {};
    if (!read_exact(task, actor + champion_name_offset, raw, sizeof(raw))) return false;
    const unsigned char length = raw[23];
    if (length > 22) return false;
    return champion_key(std::string_view(reinterpret_cast<const char*>(raw), length)) ==
           champion_key(expected_champion);
}
static int known_game_uuid(mach_port_t task, uint64_t load_address,
                           const stallion::Offsets& offsets) {
    struct mach_header_64 header = {0};
    if (!read_exact(task, load_address, &header, sizeof(header)) ||
        header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64) return 0;
    uint64_t offset = sizeof(header);
    for (uint32_t i = 0; i < header.ncmds; ++i) {
        struct load_command command = {0};
        if (offset + sizeof(command) > sizeof(header) + header.sizeofcmds ||
            !read_exact(task, load_address + offset, &command, sizeof(command)) ||
            command.cmdsize < sizeof(command) ||
            offset + command.cmdsize > sizeof(header) + header.sizeofcmds) return 0;
        if (command.cmd == LC_UUID) {
            struct uuid_command uuid = {0};
            return command.cmdsize >= sizeof(uuid) &&
                   read_exact(task, load_address + offset, &uuid, sizeof(uuid)) &&
                   memcmp(uuid.uuid, offsets.uuid.data(), offsets.uuid.size()) == 0;
        }
        offset += command.cmdsize;
    }
    return 0;
}
static uint64_t image_slide(mach_port_t task) {
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;
    uint32_t depth = 0;
    struct vm_region_submap_info_64 info = {0};
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (mach_vm_region_recurse(task, &address, &size, &depth,
                               (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS) return 0;
    return address - IMAGE_BASE;
}

auto stallion::read_live_skin_state(std::uint32_t pid, const Offsets& offsets,
                                       std::string_view expected_champion,
                                       LiveSkinState& state) -> int {
    mach_port_t task = MACH_PORT_NULL;
    if (task_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS) return 1;
    const uint64_t slide = image_slide(task);
    if (!slide || !known_game_uuid(task, IMAGE_BASE + slide, offsets)) return 1;
    uint64_t actor = 0;
    if (!read_exact(task, offsets.actor_global + slide, &actor, sizeof(actor)) || !actor) return 1;
    if (!actor_matches_champion(task, actor, expected_champion, offsets.champion_name)) return 2;
    uint32_t skin_id = 0;
    if (!read_exact(task, actor + offsets.skin_id, &skin_id, sizeof(skin_id))) return 1;
    state.actor = actor;
    state.skin_id = skin_id;
    return 0;
}

static std::atomic<mach_port_t> g_suspended_task{MACH_PORT_NULL};

void stallion::emergency_resume_live_skin() noexcept {
    if (auto task = g_suspended_task.exchange(MACH_PORT_NULL)) task_resume(task);
}

static int suspend_tracked(mach_port_t task) {
    if (task_suspend(task) != KERN_SUCCESS) return 0;
    g_suspended_task.store(task);
    return 1;
}

static void resume_tracked() {
    stallion::emergency_resume_live_skin();
}

static bool persistent_mailbox(mach_port_t task, uint64_t setup, uint64_t actor_global,
                               uint64_t& mailbox_address) {
    unsigned char entry[16] = {};
    if (!read_exact(task, setup, entry, sizeof(entry))) return false;
    const unsigned char branch[8] = {0x50,0x00,0x00,0x58, 0x00,0x02,0x1f,0xd6};
    if (memcmp(entry, branch, sizeof(branch)) != 0) return false;
    uint64_t hook_address = 0;
    memcpy(&hook_address, entry + 8, sizeof(hook_address));
    const size_t literal = (size_t)(stallion_persist_mailbox_literal - stallion_persist_begin);
    if (literal < 32 || literal > 0x1000) return false;
    unsigned char signature[32] = {};
    if (!read_exact(task, hook_address, signature, sizeof(signature)) ||
        memcmp(signature, stallion_persist_begin, sizeof(signature)) != 0 ||
        !read_exact(task, hook_address + literal, &mailbox_address, sizeof(mailbox_address))) return false;
    PersistentMailbox mailbox = {};
    return read_exact(task, mailbox_address, &mailbox, sizeof(mailbox)) &&
           mailbox.actor_global == actor_global;
}

static bool install_persistent_hook(mach_port_t task, uint64_t setup,
                                    uint64_t actor_global, uint32_t skin_id) {
    const unsigned char original[16] = {
        0xff,0x03,0x02,0xd1, 0xf8,0x5f,0x04,0xa9,
        0xf6,0x57,0x05,0xa9, 0xf4,0x4f,0x06,0xa9
    };
    unsigned char current[16] = {};
    if (!read_exact(task, setup, current, sizeof(current)) ||
        memcmp(current, original, sizeof(original)) != 0) {
        fprintf(stderr, "skin setup prologue mismatch\n"); return false;
    }
    const size_t size = (size_t)(stallion_persist_end - stallion_persist_begin);
    const size_t mailbox_slot = (size_t)(stallion_persist_mailbox_literal - stallion_persist_begin);
    const size_t return_slot = (size_t)(stallion_persist_return_literal - stallion_persist_begin);
    if (size > 0x4000 || mailbox_slot + 8 > size || return_slot + 8 > size) return false;
    mach_vm_address_t mailbox_address = 0, hook_address = 0;
    if (mach_vm_allocate(task, &mailbox_address, 0x4000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS ||
        mach_vm_allocate(task, &hook_address, 0x4000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return false;
    PersistentMailbox mailbox = {skin_id, 0, actor_global};
    if (!write_exact(task, mailbox_address, &mailbox, sizeof(mailbox))) return false;
    unsigned char *hook = (unsigned char *)calloc(1, size);
    if (!hook) return false;
    memcpy(hook, stallion_persist_begin, size);
    const uint64_t resume = setup + sizeof(original);
    memcpy(hook + mailbox_slot, &mailbox_address, 8);
    memcpy(hook + return_slot, &resume, 8);
    const bool prepared = write_exact(task, hook_address, hook, size) &&
        mach_vm_protect(task, hook_address, 0x4000, FALSE, VM_PROT_READ | VM_PROT_EXECUTE) == KERN_SUCCESS;
    free(hook);
    if (!prepared) return false;
    unsigned char branch[16] = {0x50,0x00,0x00,0x58, 0x00,0x02,0x1f,0xd6};
    memcpy(branch + 8, &hook_address, 8);
    if (!suspend_tracked(task)) return false;
    const bool writable = mach_vm_protect(task, setup, 16, FALSE,
        VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) == KERN_SUCCESS;
    const bool wrote = writable && write_exact(task, setup, branch, sizeof(branch));
    const bool executable = wrote && mach_vm_protect(task, setup, 16, FALSE,
        VM_PROT_READ | VM_PROT_EXECUTE) == KERN_SUCCESS;
    if (!executable && writable) {
        write_exact(task, setup, original, sizeof(original));
        mach_vm_protect(task, setup, 16, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    }
    resume_tracked();
    return executable;
}

auto stallion::apply_live_skin(std::uint32_t pid, std::uint32_t skin_id,
                                   const Offsets& offsets,
                                   std::string_view expected_champion) -> int {
    if (skin_id > 255) { fprintf(stderr, "skin ID out of range\n"); return 2; }
    mach_port_t task = MACH_PORT_NULL;
    kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "task_for_pid=%d\n", kr); return 1; }
    const uint64_t slide = image_slide(task);
    if (!slide) { fprintf(stderr, "cannot locate image\n"); return 1; }
    if (!known_game_uuid(task, IMAGE_BASE + slide, offsets)) {
        fprintf(stderr, "unsupported game build UUID\n"); return 1;
    }
    const uint64_t frame = offsets.frame + slide;
    const uint64_t setup_function = offsets.setup + slide;
    const uint64_t actor_global = offsets.actor_global + slide;
    uint64_t actor = 0;
    if (!read_exact(task, actor_global, &actor, 8) || !actor) { fprintf(stderr, "actor unavailable\n"); return 1; }
    uint64_t existing_mailbox = 0;
    const bool has_persistent_hook = persistent_mailbox(task, setup_function,
                                                         actor_global, existing_mailbox);

    const size_t hook_size = (size_t)(stallion_frame_hook_end - stallion_frame_hook_begin);
    const size_t replay_offset = (size_t)(stallion_frame_replay_begin - stallion_frame_hook_begin);
    unsigned char original[16];
    if (!read_exact(task, frame, original, 16) ||
        memcmp(original, stallion_frame_hook_begin + replay_offset, 16) != 0) {
        fprintf(stderr, "frame prologue mismatch; refusing this game build\n"); return 1;
    }
    if (hook_size > 0x4000) { fprintf(stderr, "hook too large\n"); return 1; }

    struct Mailbox mailbox = {0};
    mailbox.state = 1;
    mailbox.skin_id = skin_id;
    mailbox.actor_global = actor_global;
    mailbox.setup_function = setup_function;
    if (!read_exact(task, actor + offsets.champion_name,
                    mailbox.champion_string, sizeof(mailbox.champion_string))) {
        fprintf(stderr, "cannot read champion name\n"); return 1;
    }
    if (!expected_champion.empty()) {
        if (!actor_matches_champion(task, actor, expected_champion, offsets.champion_name)) {
            fprintf(stderr, "selected skin belongs to a different champion\n"); return 1;
        }
    }
    if (has_persistent_hook &&
        !write_exact(task, existing_mailbox, &skin_id, sizeof(skin_id))) {
        fprintf(stderr, "cannot update persistent skin\n"); return 1;
    }
    mach_vm_address_t mailbox_address = 0, hook_address = 0;
    if (mach_vm_allocate(task, &mailbox_address, 0x4000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS ||
        mach_vm_allocate(task, &hook_address, 0x4000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        fprintf(stderr, "allocation failed\n"); return 1;
    }
    if (!write_exact(task, mailbox_address, &mailbox, sizeof(mailbox))) return 1;

    unsigned char *hook = (unsigned char *)calloc(1, hook_size);
    if (!hook) return 1;
    memcpy(hook, stallion_frame_hook_begin, hook_size);
    const size_t mailbox_slot = (size_t)(stallion_frame_mailbox_literal - stallion_frame_hook_begin);
    const size_t return_slot = (size_t)(stallion_frame_return_literal - stallion_frame_hook_begin);
    const uint64_t resume = frame + 16;
    memcpy(hook + mailbox_slot, &mailbox_address, 8);
    memcpy(hook + return_slot, &resume, 8);
    if (!write_exact(task, hook_address, hook, hook_size) ||
        mach_vm_protect(task, hook_address, 0x4000, FALSE, VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS) {
        fprintf(stderr, "hook setup failed\n"); return 1;
    }
    free(hook);

    // ldr x16, [pc + 8]; br x16; .quad hook_address
    unsigned char branch[16] = {0x50,0x00,0x00,0x58, 0x00,0x02,0x1f,0xd6};
    memcpy(branch + 8, &hook_address, 8);
    if (!suspend_tracked(task)) { fprintf(stderr, "suspend failed\n"); return 1; }
    const int writable = mach_vm_protect(task, frame, 16, FALSE,
                                          VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) == KERN_SUCCESS;
    const int wrote = writable && write_exact(task, frame, branch, 16);
    const int executable = wrote && mach_vm_protect(task, frame, 16, FALSE,
                                                     VM_PROT_READ | VM_PROT_EXECUTE) == KERN_SUCCESS;
    if (!executable && writable) {
        write_exact(task, frame, original, 16);
        mach_vm_protect(task, frame, 16, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    }
    resume_tracked();
    if (!executable) { fprintf(stderr, "frame patch failed\n"); return 1; }

    uint32_t state = 0;
    for (int i = 0; i < 500; i++) {
        if (read_exact(task, mailbox_address, &state, 4) && state >= 3) break;
        usleep(10000);
    }
    int restored = 0;
    if (suspend_tracked(task)) {
        restored = mach_vm_protect(task, frame, 16, FALSE,
                                    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) == KERN_SUCCESS &&
                   write_exact(task, frame, original, 16) &&
                   mach_vm_protect(task, frame, 16, FALSE,
                                   VM_PROT_READ | VM_PROT_EXECUTE) == KERN_SUCCESS;
        resume_tracked();
    }
    // Hook pages stay mapped: a thread may have entered the trampoline just
    // before the prologue was restored.
    printf("state=%u actor=0x%llx skin=%u\n", state, actor, skin_id);
    if (!restored) fprintf(stderr, "frame prologue restore failed\n");
    if (state != 3 || !restored) return 1;
    if (!has_persistent_hook &&
        !install_persistent_hook(task, setup_function, actor_global, skin_id)) {
        fprintf(stderr, "skin changed but persistent respawn hook failed\n"); return 1;
    }
    return 0;
}

#else

auto stallion::apply_live_skin(std::uint32_t, std::uint32_t,
                                   const Offsets&, std::string_view) -> int { return 1; }
auto stallion::read_live_skin_state(std::uint32_t, const Offsets&, std::string_view,
                                       LiveSkinState&) -> int { return 1; }
void stallion::emergency_resume_live_skin() noexcept {}

#endif
