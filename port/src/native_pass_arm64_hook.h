/* GPL-2.0-or-later: FEX-safe observation trampoline for the ISS PC engine hook.
 *
 * PC's 0x623c60 DrawCommandBuffer::drawIndex prologue is x86-64 SysV code,
 * NOT an AArch64 function pointer. ARM64 observes it through guest_cpu_hook
 * on a separate guest trampoline; the original first 14 bytes are preserved.
 *
 * No Vulkan commands are recorded here. No guest data or memory is modified
 * beyond the 14-byte entry jump, only when the exact known signature matches.
 */
#ifndef ISS_NATIVE_PASS_ARM64_H
#define ISS_NATIVE_PASS_ARM64_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum {
    ISS_NATIVE_DRAW_OFFSET = 0x623c60,
    ISS_NATIVE_DRAW_PROLOGUE_SIZE = 14,
    ISS_NATIVE_DRAW_TRAMPOLINE_SIZE = 36,
    ISS_NATIVE_DRAW_TRAP_OFFSET = 3,
    ISS_NATIVE_DRAW_AFTER_TRAP = 5,
};

static const unsigned char iss_native_draw_prologue[ISS_NATIVE_DRAW_PROLOGUE_SIZE] = {
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x89,0xfb
};

static inline int iss_native_draw_signature(const unsigned char *image, size_t size) {
    return image && size >= ISS_NATIVE_DRAW_OFFSET + ISS_NATIVE_DRAW_PROLOGUE_SIZE &&
           !memcmp(image + ISS_NATIVE_DRAW_OFFSET, iss_native_draw_prologue,
                   ISS_NATIVE_DRAW_PROLOGUE_SIZE);
}

/* FF 25 [rip+0] followed by absolute guest address: no GPR is clobbered. */
static inline void iss_native_jump14(unsigned char out[14], uintptr_t address) {
    out[0] = 0xff; out[1] = 0x25;
    memset(out + 2, 0, 4);
    const uint64_t target = (uint64_t)address;
    memcpy(out + 6, &target, 8);
}

static inline size_t iss_native_make_trampoline(unsigned char out[36], uintptr_t resume) {
    /* Preserve RCX (4th SysV parameter) and R11 across the guest SYSCALL.
       The hook callback resumes at byte 5, where POPs restore both registers.
       No host ARM64 ABI call is made from translated x86 code. */
    const unsigned char prefix[8] = {
        0x51,                   /* push rcx */
        0x41,0x53,              /* push r11 */
        0x0f,0x05,              /* syscall => FEX guest_cpu_hook */
        0x41,0x5b,              /* pop r11 */
        0x59                    /* pop rcx */
    };
    memcpy(out, prefix, sizeof(prefix));
    memcpy(out + 8, iss_native_draw_prologue, ISS_NATIVE_DRAW_PROLOGUE_SIZE);
    iss_native_jump14(out + 22, resume);
    return ISS_NATIVE_DRAW_TRAMPOLINE_SIZE;
}
#endif
