#include "live_skin.hpp"
#include "special_forms.hpp"

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
extern const unsigned char stallion_persist_replay_begin[];

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
    ldr w17, [x16, #0x58]
    tbz w17, #0, Lswitch_done
    // Load the selected character/skin record before rebuilding its appearance.
    add x0, x16, #0x220
    ldr w1, [x16, #4]
    ldr x17, [x16, #48]
    blr x17
    adr x16, Lmailbox_literal
    ldr x16, [x16]
    ldr x0, [x16, #8]
    ldr x0, [x0]
    cbz x0, Lno_actor
    // First let the normal skin refresh copy the selected skin record into
    // the actor stack. This makes the record pointer below refer to the new
    // skin rather than the skin that was active when the hook was installed.
    // The actor field holds the encoded ID that the refresh decodes.
    ldr w9, [x16, #44]
    add x9, x0, x9
    ldr w2, [x16, #0x54]
    str w2, [x9]
    mov x1, #0
    mov x2, #0
    ldr x17, [x16, #56]
    blr x17
    adr x16, Lmailbox_literal
    ldr x16, [x16]
    ldr x0, [x16, #8]
    ldr x0, [x0]
    cbz x0, Lno_actor
    // Match the game's actor-construction order: bind the selected character
    // record to the actor components before refreshing the resource stack.
    //
    // actor_refresh resolves the selected CharacterRecord* at the verified
    // actor field. The constructor passes a pointer to this record pointer to
    // character_prepare. The resource update below uses the separate stack
    // record at stack+0xb8.
    ldr w9, [x16, #80]
    add x1, x0, x9
    ldr x9, [x1]
    cbz x9, Lno_record
    ldr w10, [x9, #20]
    ldr w11, [x16, #4]
    cmp w10, w11
    b.ne Lno_record
    ldr x17, [x16, #72]
    blr x17
    adr x16, Lmailbox_literal
    ldr x16, [x16]
    // Run appearance setup again after binding the newly selected record.
    ldr x0, [x16, #8]
    ldr x0, [x0]
    cbz x0, Lno_actor
    mov x1, #0
    mov x2, #0
    ldr x17, [x16, #56]
    blr x17
    adr x16, Lmailbox_literal
    ldr x16, [x16]
    // Re-run the component resource update after setup has seen the bound
    // animation graph. It installs the selected spell/effect resources.
    ldr x0, [x16, #8]
    ldr x0, [x0]
    cbz x0, Lno_actor
    ldr w9, [x16, #32]
    add x1, x0, x9
    add x1, x1, #0xb8
    ldr x9, [x1]
    cbz x9, Lno_record
    ldr w10, [x9, #20]
    ldr w11, [x16, #4]
    cmp w10, w11
    b.ne Lno_record
    ldr x17, [x16, #64]
    blr x17
Lswitch_done:
    adr x16, Lmailbox_literal
    ldr x16, [x16]
    ldr x0, [x16, #8]
    ldr x0, [x0]
    cbz x0, Lno_actor
    ldr w17, [x16, #0x58]
    tbz w17, #1, Lgear_done
    ldr w17, [x16, #40]
    cmp w17, #0xff
    b.eq Lgear_done
    // The stack refresh hands a selected form to the actor's own gear setter,
    // which shows that form's weapon submeshes and installs its effects.
    ldr w9, [x16, #36]
    add x9, x0, x9
    strb w17, [x9]
    ldr w9, [x16, #32]
    add x0, x0, x9
    mov w1, #1
    ldr x17, [x16, #24]
    blr x17
Lgear_done:
    adr x16, Lmailbox_literal
    ldr x16, [x16]
    ldr w17, [x16, #0x58]
    tbz w17, #2, Lquery_done
    // Report the actor's animation player; its graph holds the clip-name table.
    ldr x0, [x16, #8]
    ldr x0, [x0]
    cbz x0, Lno_actor
    ldr x8, [x0]
    ldr w9, [x16, #0x5c]
    ldr x17, [x8, x9]
    blr x17
    adr x16, Lmailbox_literal
    ldr x16, [x16]
    str x0, [x16, #0x68]
Lquery_done:
    ldr w17, [x16, #0x58]
    tbz w17, #3, Lsteps_done
    // Clip-table nodes are later freed by the game, so they must come from
    // its operator new. Count completed allocations in the mailbox.
Lallocate:
    ldr w9, [x16, #0x74]
    ldr w10, [x16, #0x70]
    cmp w9, w10
    b.hs Lsteps_done
    mov x0, #0x18
    ldr x17, [x16, #0x60]
    blr x17
    adr x16, Lmailbox_literal
    ldr x16, [x16]
    ldr w9, [x16, #0x74]
    add x10, x16, #0x78
    str x0, [x10, w9, uxtw #3]
    add w9, w9, #1
    str w9, [x16, #0x74]
    b Lallocate
Lsteps_done:
    mov w17, #3
    str w17, [x16]
    b Lrestore
Lno_record:
    mov w17, #5
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
.globl _stallion_persist_replay_begin
_stallion_persist_begin:
    sub sp, sp, #0x10
    stp x16, x17, [sp]
    adr x16, Lstallion_mailbox_pointer
    ldr x16, [x16]
    ldr x17, [x16, #8]
    ldr x17, [x17]
    cmp x0, x17
    b.ne Lstallion_restore
    // Store the encoded ID in the actor field, then pass the plain ID to setup.
    ldr w2, [x16, #20]
    ldr w17, [x16, #24]
    str w17, [x0, x2]
    ldr w2, [x16]
    ldr w17, [x16, #4]
    cmp w17, #0xff
    b.eq Lstallion_restore
    ldr w16, [x16, #16]
    add x16, x0, x16
    strb w17, [x16]
Lstallion_restore:
    ldp x16, x17, [sp]
    add sp, sp, #0x10
_stallion_persist_replay_begin:
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
    uint32_t gear;
    uint64_t actor_global;
    uint32_t gear_offset;
    uint32_t skin_id_offset;
    uint32_t encoded_skin_id;
    uint32_t reserved;
};
static_assert(offsetof(PersistentMailbox, encoded_skin_id) == 24);

enum : uint32_t {
    kStepSwitch = 1,        // load the record and rebuild the actor's appearance
    kStepGear = 2,          // select the form byte and refresh the character stack
    kStepAnimation = 4,     // report the actor's animation player
    kStepAllocate = 8,      // allocate clip-table nodes with the game's operator new
};
constexpr uint32_t kMaxClipNodes = 4;

struct Mailbox {
    uint32_t state; // 1 requested, 2 executing, 3 completed, 4 actor missing, 5 record missing/mismatched
    uint32_t skin_id;
    uint64_t actor_global;
    uint64_t setup_function;
    uint64_t refresh_function;
    uint32_t stack_offset;
    uint32_t gear_offset;
    uint32_t gear;
    uint32_t skin_id_offset;
    uint64_t preload_function;
    uint64_t actor_refresh_function;
    uint64_t character_update_function;
    uint64_t character_prepare_function;
    uint32_t character_record_offset;
    uint32_t encoded_skin_id;
    uint32_t steps;
    uint32_t animation_slot;
    uint64_t operator_new;
    uint64_t animation_player;          // written by kStepAnimation
    uint32_t node_count;
    uint32_t nodes_allocated;           // written by kStepAllocate
    uint64_t nodes[kMaxClipNodes];
    unsigned char reserved[0x100 - 0x98];
    unsigned char champion_string[24];
    unsigned char reserved2[0x200 - 0x100 - 24];
    char empty_override;
    unsigned char reserved3[0x220 - 0x201];
    char champion_cstr[64];
};
static_assert(offsetof(Mailbox, champion_string) == 0x100);
static_assert(offsetof(Mailbox, character_record_offset) == 80);
static_assert(offsetof(Mailbox, encoded_skin_id) == 0x54);
static_assert(offsetof(Mailbox, steps) == 0x58);
static_assert(offsetof(Mailbox, animation_slot) == 0x5c);
static_assert(offsetof(Mailbox, operator_new) == 0x60);
static_assert(offsetof(Mailbox, animation_player) == 0x68);
static_assert(offsetof(Mailbox, node_count) == 0x70);
static_assert(offsetof(Mailbox, nodes_allocated) == 0x74);
static_assert(offsetof(Mailbox, nodes) == 0x78);
static_assert(offsetof(Mailbox, empty_override) == 0x200);
static_assert(offsetof(Mailbox, champion_cstr) == 0x220);

static uint32_t ror32(uint32_t value, uint32_t amount) {
    amount &= 31;
    return amount ? (value >> amount) | (value << (32 - amount)) : value;
}
static uint32_t decode_skin_id(uint32_t field, const stallion::Offsets& offsets) {
    const uint32_t adjusted = ror32(field, offsets.skin_pre_ror) + offsets.skin_add;
    return ror32(offsets.skin_key ^ ror32(adjusted, offsets.skin_field_ror), offsets.skin_value_ror);
}
static uint32_t encode_skin_id(uint32_t skin_id, const stallion::Offsets& offsets) {
    const uint32_t adjusted = ror32(offsets.skin_key ^ ror32(skin_id, 32 - offsets.skin_value_ror),
                                    32 - offsets.skin_field_ror);
    return ror32(adjusted - offsets.skin_add, 32 - offsets.skin_pre_ror);
}

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
    skin_id = decode_skin_id(skin_id, offsets);
    uint32_t stack_skin = 0;
    if (!read_exact(task, actor + offsets.stack + 0x38,
                    &stack_skin, sizeof(stack_skin))) return 1;
    int8_t gear = -1;
    if (!read_exact(task, actor + offsets.gear, &gear, sizeof(gear))) return 1;
    uint64_t character_record = 0, stack_record = 0;
    uint32_t character_record_skin = 0, stack_record_skin = 0;
    if (!read_exact(task, actor + offsets.character_record,
                    &character_record, sizeof(character_record)) ||
        !read_exact(task, actor + offsets.stack + 0xb8,
                    &stack_record, sizeof(stack_record))) return 1;
    if (character_record &&
        !read_exact(task, character_record + 20,
                    &character_record_skin, sizeof(character_record_skin))) return 1;
    if (stack_record &&
        !read_exact(task, stack_record + 20,
                    &stack_record_skin, sizeof(stack_record_skin))) return 1;
    state.actor = actor;
    state.skin_id = skin_id;
    state.stack_skin = stack_skin;
    state.gear = gear;
    state.character_record = character_record;
    state.character_record_skin = character_record_skin;
    state.stack_record = stack_record;
    state.stack_record_skin = stack_record_skin;
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
    unsigned char signature[32] = {};
    if (!read_exact(task, hook_address, signature, sizeof(signature)) ||
        memcmp(signature, stallion_persist_begin, 8) != 0 ||
        memcmp(signature + 12, stallion_persist_begin + 12, 16) != 0) return false;
    uint32_t branch_insn = 0, expected_branch = 0;
    memcpy(&branch_insn, signature + 28, sizeof(branch_insn));
    memcpy(&expected_branch, stallion_persist_begin + 28, sizeof(expected_branch));
    if ((branch_insn & 0xff00001f) != (expected_branch & 0xff00001f)) return false;
    uint32_t adr = 0;
    memcpy(&adr, signature + 8, sizeof(adr));
    if ((adr & 0x9f00001f) != 0x10000010) return false;
    int32_t displacement = static_cast<int32_t>(
        (((adr >> 5) & 0x7ffff) << 2) | ((adr >> 29) & 3));
    if (displacement & (1 << 20)) displacement -= 1 << 21;
    if (displacement < 32 || displacement > 0x1000 ||
        !read_exact(task, hook_address + 8 + displacement,
                    &mailbox_address, sizeof(mailbox_address))) return false;
    PersistentMailbox mailbox = {};
    return read_exact(task, mailbox_address, &mailbox, sizeof(mailbox)) &&
           mailbox.actor_global == actor_global;
}

static bool install_persistent_hook(mach_port_t task, uint64_t setup,
                                    const PersistentMailbox& mailbox) {
    const unsigned char original[16] = {
        0xff,0x03,0x02,0xd1, 0xf8,0x5f,0x04,0xa9,
        0xf6,0x57,0x05,0xa9, 0xf4,0x4f,0x06,0xa9
    };
    const unsigned char extended[16] = {
        0xff,0x43,0x02,0xd1, 0xfa,0x67,0x04,0xa9,
        0xf8,0x5f,0x05,0xa9, 0xf6,0x57,0x06,0xa9
    };
    unsigned char current[16] = {};
    if (!read_exact(task, setup, current, sizeof(current)) ||
        (memcmp(current, original, sizeof(original)) != 0 &&
         memcmp(current, extended, sizeof(extended)) != 0)) {
        fprintf(stderr, "skin setup prologue mismatch\n"); return false;
    }
    const size_t size = (size_t)(stallion_persist_end - stallion_persist_begin);
    const size_t mailbox_slot = (size_t)(stallion_persist_mailbox_literal - stallion_persist_begin);
    const size_t return_slot = (size_t)(stallion_persist_return_literal - stallion_persist_begin);
    const size_t replay_slot = (size_t)(stallion_persist_replay_begin - stallion_persist_begin);
    if (size > 0x4000 || mailbox_slot + 8 > size || return_slot + 8 > size ||
        replay_slot + sizeof(current) > size) return false;
    mach_vm_address_t mailbox_address = 0, hook_address = 0;
    if (mach_vm_allocate(task, &mailbox_address, 0x4000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS ||
        mach_vm_allocate(task, &hook_address, 0x4000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return false;
    if (!write_exact(task, mailbox_address, &mailbox, sizeof(mailbox))) return false;
    unsigned char *hook = (unsigned char *)calloc(1, size);
    if (!hook) return false;
    memcpy(hook, stallion_persist_begin, size);
    // Replay only a recognized, position-independent prologue. The game's
    // larger setup stack frame saves different registers on newer builds.
    memcpy(hook + replay_slot, current, sizeof(current));
    const uint64_t resume = setup + sizeof(current);
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
        write_exact(task, setup, current, sizeof(current));
        mach_vm_protect(task, setup, 16, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    }
    resume_tracked();
    return executable;
}

// A verified game process with the local actor and the mailbox fields that
// every game-thread request shares.
struct GameSession {
    mach_port_t task = MACH_PORT_NULL;
    uint64_t slide = 0;
    uint64_t actor = 0;
    uint64_t frame = 0;
    Mailbox mailbox = {};
};

static bool prefix_matches(mach_port_t task, uint64_t at, const unsigned char (&prefix)[16]) {
    unsigned char current[16] = {};
    return read_exact(task, at, current, sizeof(current)) &&
           memcmp(current, prefix, sizeof(current)) == 0;
}

static int open_game_session(uint32_t pid, const stallion::Offsets& offsets,
                             std::string_view expected_champion, int selected_gear,
                             GameSession& game) {
    kern_return_t kr = task_for_pid(mach_task_self(), pid, &game.task);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "task_for_pid=%d\n", kr); return 1; }
    game.slide = image_slide(game.task);
    if (!game.slide) { fprintf(stderr, "cannot locate image\n"); return 1; }
    if (!known_game_uuid(game.task, IMAGE_BASE + game.slide, offsets)) {
        fprintf(stderr, "unsupported game build UUID\n"); return 1;
    }
    mach_port_t task = game.task;
    const uint64_t slide = game.slide;
    game.frame = offsets.frame + slide;
    const uint64_t actor_global = offsets.actor_global + slide;
    if (!read_exact(task, actor_global, &game.actor, 8) || !game.actor) {
        fprintf(stderr, "actor unavailable\n"); return 1;
    }
    const unsigned char preload_prefix[16] = {
        0xff,0xc3,0x00,0xd1, 0xf4,0x4f,0x01,0xa9,
        0xfd,0x7b,0x02,0xa9, 0xfd,0x83,0x00,0x91
    };
    if (!prefix_matches(task, offsets.preload + slide, preload_prefix)) {
        fprintf(stderr, "character preload signature mismatch\n"); return 1;
    }
    const unsigned char actor_refresh_prefix[16] = {
        0xff,0x43,0x01,0xd1, 0xf8,0x5f,0x01,0xa9,
        0xf6,0x57,0x02,0xa9, 0xf4,0x4f,0x03,0xa9
    };
    if (!prefix_matches(task, offsets.actor_refresh + slide, actor_refresh_prefix)) {
        fprintf(stderr, "actor appearance refresh signature mismatch\n"); return 1;
    }
    const unsigned char character_update_prefix[16] = {
        0xff,0x83,0x01,0xd1, 0xf6,0x57,0x03,0xa9,
        0xf4,0x4f,0x04,0xa9, 0xfd,0x7b,0x05,0xa9
    };
    if (!prefix_matches(task, offsets.character_update + slide, character_update_prefix)) {
        fprintf(stderr, "character resource update signature mismatch\n"); return 1;
    }
    const unsigned char character_prepare_prefix[16] = {
        0xf4,0x4f,0xbe,0xa9, 0xfd,0x7b,0x01,0xa9,
        0xfd,0x43,0x00,0x91, 0xf3,0x03,0x01,0xaa
    };
    if (!prefix_matches(task, offsets.character_prepare + slide, character_prepare_prefix)) {
        fprintf(stderr, "character animation prepare signature mismatch\n"); return 1;
    }
    if (selected_gear != -1) {
        const unsigned char prefix[4] = {0xff, 0x03, 0x02, 0xd1};
        unsigned char current[4] = {};
        if (offsets.gear != offsets.stack + 0x9c ||
            !read_exact(task, offsets.refresh + slide, current, sizeof(current)) ||
            memcmp(current, prefix, sizeof(current)) != 0) {
            fprintf(stderr, "form refresh signature mismatch\n"); return 1;
        }
    }
    Mailbox& mailbox = game.mailbox;
    mailbox.actor_global = actor_global;
    mailbox.setup_function = offsets.setup + slide;
    mailbox.preload_function = offsets.preload + slide;
    mailbox.actor_refresh_function = offsets.actor_refresh + slide;
    mailbox.character_update_function = offsets.character_update + slide;
    mailbox.character_prepare_function = offsets.character_prepare + slide;
    mailbox.character_record_offset = offsets.character_record;
    mailbox.refresh_function = offsets.refresh + slide;
    mailbox.stack_offset = offsets.stack;
    mailbox.gear_offset = offsets.gear;
    mailbox.gear = static_cast<uint32_t>(selected_gear < 0 ? 255 : selected_gear);
    mailbox.skin_id_offset = offsets.skin_id;
    mailbox.animation_slot = offsets.animation_slot;
    mailbox.operator_new = offsets.operator_new + slide;
    if (!read_exact(task, game.actor + offsets.champion_name,
                    mailbox.champion_string, sizeof(mailbox.champion_string))) {
        fprintf(stderr, "cannot read champion name\n"); return 1;
    }
    const size_t champion_length = mailbox.champion_string[23];
    if (champion_length > 22) {
        fprintf(stderr, "unexpected champion name layout\n"); return 1;
    }
    memcpy(mailbox.champion_cstr, mailbox.champion_string, champion_length);
    if (!expected_champion.empty() &&
        !actor_matches_champion(task, game.actor, expected_champion, offsets.champion_name)) {
        fprintf(stderr, "selected skin belongs to a different champion\n"); return 1;
    }
    return 0;
}

// Runs the requested mailbox steps once on the game thread from the frame
// callback and reads the mailbox back. Returns the final state, or 0 when the
// hook could not be installed or removed.
static uint32_t run_on_game_thread(GameSession& game, uint32_t steps) {
    mach_port_t task = game.task;
    const uint64_t frame = game.frame;
    const size_t hook_size = (size_t)(stallion_frame_hook_end - stallion_frame_hook_begin);
    const size_t replay_offset = (size_t)(stallion_frame_replay_begin - stallion_frame_hook_begin);
    unsigned char original[16];
    if (!read_exact(task, frame, original, 16) ||
        memcmp(original, stallion_frame_hook_begin + replay_offset, 16) != 0) {
        fprintf(stderr, "frame prologue mismatch; refusing this game build\n"); return 0;
    }
    if (hook_size > 0x4000) { fprintf(stderr, "hook too large\n"); return 0; }
    Mailbox& mailbox = game.mailbox;
    mailbox.state = 1;
    mailbox.steps = steps;
    mailbox.animation_player = 0;
    mailbox.nodes_allocated = 0;
    memset(mailbox.nodes, 0, sizeof(mailbox.nodes));
    mach_vm_address_t mailbox_address = 0, hook_address = 0;
    if (mach_vm_allocate(task, &mailbox_address, 0x4000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS ||
        mach_vm_allocate(task, &hook_address, 0x4000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        fprintf(stderr, "allocation failed\n"); return 0;
    }
    if (!write_exact(task, mailbox_address, &mailbox, sizeof(mailbox))) return 0;

    unsigned char *hook = (unsigned char *)calloc(1, hook_size);
    if (!hook) return 0;
    memcpy(hook, stallion_frame_hook_begin, hook_size);
    const size_t mailbox_slot = (size_t)(stallion_frame_mailbox_literal - stallion_frame_hook_begin);
    const size_t return_slot = (size_t)(stallion_frame_return_literal - stallion_frame_hook_begin);
    const uint64_t resume = frame + 16;
    memcpy(hook + mailbox_slot, &mailbox_address, 8);
    memcpy(hook + return_slot, &resume, 8);
    const bool prepared = write_exact(task, hook_address, hook, hook_size) &&
        mach_vm_protect(task, hook_address, 0x4000, FALSE, VM_PROT_READ | VM_PROT_EXECUTE) == KERN_SUCCESS;
    free(hook);
    if (!prepared) { fprintf(stderr, "hook setup failed\n"); return 0; }

    // ldr x16, [pc + 8]; br x16; .quad hook_address
    unsigned char branch[16] = {0x50,0x00,0x00,0x58, 0x00,0x02,0x1f,0xd6};
    memcpy(branch + 8, &hook_address, 8);
    if (!suspend_tracked(task)) { fprintf(stderr, "suspend failed\n"); return 0; }
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
    if (!executable) { fprintf(stderr, "frame patch failed\n"); return 0; }

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
    if (!restored) { fprintf(stderr, "frame prologue restore failed\n"); return 0; }
    if (state >= 3 && !read_exact(task, mailbox_address, &mailbox, sizeof(mailbox))) return 0;
    return state;
}

// The graph's clip names live in a libc++ unordered_map<uint32_t, int32_t>:
// bucket array, bucket count, list head, size. Nodes are {next, hash, key, clip}.
struct ClipTable {
    uint64_t buckets;
    uint64_t bucket_count;
    uint64_t first;
    uint64_t size;
};
struct ClipNode {
    uint64_t next;
    uint64_t hash;
    uint32_t key;
    int32_t clip;
};
static_assert(sizeof(ClipNode) == 0x18);

// The game hashes clip names with FNV-1a over ASCII-lowercased bytes.
static uint32_t clip_hash(std::string_view name) {
    uint32_t hash = 0x811c9dc5;
    for (unsigned char c : name) {
        hash ^= (c >= 'A' && c <= 'Z') ? c + 32 : c;
        hash *= 0x01000193;
    }
    return hash;
}
static uint64_t clip_bucket(uint64_t hash, uint64_t count) {
    return (count & (count - 1)) == 0 ? hash & (count - 1) : (hash < count ? hash : hash % count);
}
// Mirrors unordered_map::find; the chain of a bucket ends at the first node
// that hashes elsewhere.
static bool find_clip(mach_port_t task, const ClipTable& table, uint32_t hash, int32_t& clip) {
    if (!table.buckets || !table.bucket_count) return false;
    const uint64_t bucket = clip_bucket(hash, table.bucket_count);
    uint64_t before = 0, at = 0;
    if (!read_exact(task, table.buckets + bucket * 8, &before, 8) || !before ||
        !read_exact(task, before, &at, 8)) return false;
    for (int guard = 0; at && guard < 4096; ++guard) {
        ClipNode node = {};
        if (!read_exact(task, at, &node, sizeof(node))) return false;
        if (node.hash == hash) {
            if (node.key == hash) { clip = node.clip; return true; }
        } else if (clip_bucket(node.hash, table.bucket_count) != bucket) {
            return false;
        }
        at = node.next;
    }
    return false;
}
// Inserts a node the game allocated, as unordered_map inserts a new key
// without rehashing. The task must be suspended.
static bool link_clip(mach_port_t task, uint64_t table_address, ClipTable& table,
                      uint64_t node_address, uint32_t hash, int32_t clip) {
    const uint64_t slot = table.buckets + clip_bucket(hash, table.bucket_count) * 8;
    uint64_t before = 0;
    if (!read_exact(task, slot, &before, 8)) return false;
    ClipNode node = {0, hash, hash, clip};
    if (before) {
        if (!read_exact(task, before, &node.next, 8) ||
            !write_exact(task, node_address, &node, sizeof(node)) ||
            !write_exact(task, before, &node_address, 8)) return false;
    } else {
        // An empty bucket starts after the list head. The previous first node
        // is now preceded by the new node, so its bucket must point there.
        const uint64_t head = table_address + offsetof(ClipTable, first);
        node.next = table.first;
        uint64_t first_hash = 0;
        if (table.first && !read_exact(task, table.first + offsetof(ClipNode, hash),
                                       &first_hash, sizeof(first_hash))) return false;
        if (!write_exact(task, node_address, &node, sizeof(node)) ||
            !write_exact(task, head, &node_address, 8) ||
            !write_exact(task, slot, &head, 8)) return false;
        if (table.first &&
            !write_exact(task, table.buckets + clip_bucket(first_hash, table.bucket_count) * 8,
                         &node_address, 8)) return false;
        table.first = node_address;
    }
    ++table.size;
    return write_exact(task, table_address + offsetof(ClipTable, size), &table.size, 8);
}

// The server keeps sending the default skin's animation names after a live
// switch. Give the new skin's graph the missing names, pointing at the clips
// the server would have requested for that skin. Returns 0 when every alias is
// present, 3 while the new graph is not loaded yet, and 1 on failure.
static int install_clip_aliases(GameSession& game, const stallion::Offsets& offsets,
                                std::string_view champion, uint32_t skin_id) {
    const auto aliases = stallion::clip_aliases(champion, skin_id);
    if (!aliases.count) return 0;
    if (aliases.count > static_cast<int>(kMaxClipNodes)) return 1;
    mach_port_t task = game.task;
    const uint64_t player = game.mailbox.animation_player;
    uint64_t graph = 0;
    if (!player || !read_exact(task, player + offsets.animation_graph, &graph, 8) || !graph) {
        fprintf(stderr, "animation player unavailable\n"); return 1;
    }
    const uint64_t table_address = graph + offsets.clip_map;
    ClipTable table = {};
    if (!read_exact(task, table_address, &table, sizeof(table)) || !table.bucket_count) {
        fprintf(stderr, "animation clip table unavailable\n"); return 1;
    }
    struct Needed { const stallion::ClipAlias* alias; uint32_t hash; int32_t clip; };
    Needed needed[kMaxClipNodes];
    uint32_t count = 0;
    bool waiting = false;
    for (int i = 0; i < aliases.count; ++i) {
        const auto& alias = aliases.items[i];
        int32_t clip = -1;
        if (find_clip(task, table, clip_hash(alias.missing), clip)) {
            printf("animation %s -> %s already active\n", alias.missing, alias.replacement);
        } else if (find_clip(task, table, clip_hash(alias.replacement), clip) && clip >= 0) {
            needed[count++] = {&alias, clip_hash(alias.missing), clip};
        } else {
            waiting = true;
        }
    }
    if (!count) {
        if (waiting) printf("animation fix waits for the skin's animations to load\n");
        return waiting ? 3 : 0;
    }
    game.mailbox.node_count = count;
    if (run_on_game_thread(game, kStepAllocate) != 3 || game.mailbox.nodes_allocated != count) {
        fprintf(stderr, "could not allocate animation aliases\n"); return 1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!game.mailbox.nodes[i]) { fprintf(stderr, "game allocator returned null\n"); return 1; }
    }
    // Link with the game stopped and the table unchanged since it was read. If
    // anything moved, the nodes stay unused rather than risk the table.
    bool linked = true;
    if (!suspend_tracked(task)) return 1;
    uint64_t graph_now = 0;
    ClipTable table_now = {};
    if (!read_exact(task, player + offsets.animation_graph, &graph_now, 8) || graph_now != graph ||
        !read_exact(task, table_address, &table_now, sizeof(table_now)) ||
        table_now.buckets != table.buckets || table_now.bucket_count != table.bucket_count) {
        linked = false;
    }
    for (uint32_t i = 0; linked && i < count; ++i) {
        int32_t clip = -1;
        linked = !find_clip(task, table_now, needed[i].hash, clip) &&
                 link_clip(task, table_address, table_now, game.mailbox.nodes[i],
                           needed[i].hash, needed[i].clip);
    }
    resume_tracked();
    if (!linked) { fprintf(stderr, "animation clip table changed; try again\n"); return 1; }
    for (uint32_t i = 0; i < count; ++i)
        printf("animation %s -> %s added\n", needed[i].alias->missing, needed[i].alias->replacement);
    return waiting ? 3 : 0;
}

// How many forms (mGearSkinUpgrades) the skin the actor wears has, read
// along the same path as the game's gear component. 0 when it has none.
static uint32_t read_form_count(mach_port_t task, uint64_t actor, const stallion::Offsets& offsets) {
    uint64_t resource = 0, properties = 0;
    uint32_t count = 0;
    if (!read_exact(task, actor + offsets.skin_resource, &resource, 8) || !resource ||
        !read_exact(task, resource + offsets.skin_properties, &properties, 8) || !properties ||
        !read_exact(task, properties + offsets.gear_list + 8, &count, 4)) return 0;
    return count <= static_cast<uint32_t>(stallion::kMaxForms) ? count : 0;
}

auto stallion::apply_live_skin(std::uint32_t pid, std::uint32_t skin_id,
                                   const Offsets& offsets,
                                   std::string_view expected_champion,
                                   int selected_gear) -> int {
    if (skin_id > 255) { fprintf(stderr, "skin ID out of range\n"); return 2; }
    if (!valid_gear(expected_champion, skin_id, selected_gear)) {
        fprintf(stderr, "unsupported skin form\n"); return 2;
    }
    GameSession game;
    if (open_game_session(pid, offsets, expected_champion, selected_gear, game) != 0) return 1;
    uint32_t current = 0;
    if (!read_exact(game.task, game.actor + offsets.skin_id, &current, sizeof(current)) ||
        decode_skin_id(current, offsets) > 0xffff) {
        fprintf(stderr, "skin field does not decode with this profile\n"); return 1;
    }
    const uint64_t setup_function = offsets.setup + game.slide;
    const uint32_t gear = game.mailbox.gear;
    const PersistentMailbox persistent = {skin_id, gear, game.mailbox.actor_global, offsets.gear,
                                          offsets.skin_id, encode_skin_id(skin_id, offsets), 0};
    uint64_t existing_mailbox = 0;
    const bool has_persistent_hook = persistent_mailbox(game.task, setup_function,
                                                         game.mailbox.actor_global, existing_mailbox);
    if (has_persistent_hook &&
        !write_exact(game.task, existing_mailbox, &persistent, sizeof(persistent))) {
        fprintf(stderr, "cannot update persistent skin\n"); return 1;
    }
    game.mailbox.skin_id = skin_id;
    game.mailbox.encoded_skin_id = persistent.encoded_skin_id;
    const bool aliases = clip_aliases(expected_champion, skin_id).count > 0;
    const uint32_t state = run_on_game_thread(game, kStepSwitch |
        (selected_gear != -1 ? kStepGear : 0) | (aliases ? kStepAnimation : 0));
    printf("state=%u actor=0x%llx skin=%u\n", state, game.actor, skin_id);
    if (state == 5) fprintf(stderr, "selected character record did not load\n");
    if (state != 3) return 1;
    if (!has_persistent_hook && !install_persistent_hook(game.task, setup_function, persistent)) {
        fprintf(stderr, "skin changed but persistent respawn hook failed\n"); return 1;
    }
    // The new graph may still be loading. The skin itself has changed, and the
    // overlay repeats the animation fix through apply_live_form.
    if (install_clip_aliases(game, offsets, expected_champion, skin_id) == 1)
        fprintf(stderr, "skin changed; animation fix will be retried\n");
    printf("forms=%u\n", read_form_count(game.task, game.actor, offsets));
    return 0;
}

auto stallion::apply_live_form(std::uint32_t pid, std::uint32_t skin_id,
                                   const Offsets& offsets,
                                   std::string_view expected_champion,
                                   int selected_gear) -> int {
    if (selected_gear < -1 || selected_gear >= kMaxForms) {
        fprintf(stderr, "unsupported skin form\n"); return 2;
    }
    const bool aliases = clip_aliases(expected_champion, skin_id).count > 0;
    GameSession game;
    if (open_game_session(pid, offsets, expected_champion, selected_gear, game) != 0) return 1;
    uint32_t current = 0;
    if (!read_exact(game.task, game.actor + offsets.skin_id, &current, sizeof(current))) return 1;
    if (decode_skin_id(current, offsets) != skin_id) {
        fprintf(stderr, "the actor wears skin %u; switch to skin %u first\n",
                decode_skin_id(current, offsets), skin_id);
        return 1;
    }
    // Any skin with a form list can use it; the list in the game is the limit.
    const uint32_t forms = read_form_count(game.task, game.actor, offsets);
    if (selected_gear >= static_cast<int>(forms)) {
        fprintf(stderr, "this skin has %u forms\n", forms); return 2;
    }
    uint64_t existing_mailbox = 0;
    if (selected_gear != -1 &&
        persistent_mailbox(game.task, offsets.setup + game.slide, game.mailbox.actor_global,
                           existing_mailbox)) {
        // Keep the chosen form when the game re-runs skin setup on respawn.
        const PersistentMailbox persistent = {skin_id, game.mailbox.gear, game.mailbox.actor_global,
                                              offsets.gear, offsets.skin_id,
                                              encode_skin_id(skin_id, offsets), 0};
        if (!write_exact(game.task, existing_mailbox, &persistent, sizeof(persistent))) {
            fprintf(stderr, "cannot update persistent form\n"); return 1;
        }
    }
    const uint32_t steps = (selected_gear != -1 ? kStepGear : 0) | (aliases ? kStepAnimation : 0);
    if (steps) {
        const uint32_t state = run_on_game_thread(game, steps);
        if (state != 3) { fprintf(stderr, "form request did not run (state %u)\n", state); return 1; }
    }
    if (selected_gear != -1) {
        const auto names = form_names(expected_champion, skin_id);
        if (selected_gear < names.count) printf("form %s applied\n", names.names[selected_gear]);
        else printf("form %d applied\n", selected_gear + 1);
    }
    const int aliased = install_clip_aliases(game, offsets, expected_champion, skin_id);
    printf("forms=%u\n", forms);
    return aliased;
}

#else

auto stallion::apply_live_skin(std::uint32_t, std::uint32_t,
                                   const Offsets&, std::string_view, int) -> int { return 1; }
auto stallion::apply_live_form(std::uint32_t, std::uint32_t,
                                   const Offsets&, std::string_view, int) -> int { return 1; }
auto stallion::read_live_skin_state(std::uint32_t, const Offsets&, std::string_view,
                                       LiveSkinState&) -> int { return 1; }
void stallion::emergency_resume_live_skin() noexcept {}

#endif
