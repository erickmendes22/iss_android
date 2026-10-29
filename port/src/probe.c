/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "runtime.h"
#include "guest_cpu.h"
#include "cpu_check.h"
#include "isa_rewrite.h"
#include "orb_hook.h"
#include "cheat_hook.h"
#include "actor_hook.h"
#include "damage_hook.h"
#include "view_hook.h"
#include "iss_menu_state.h"
#include "gpu/tlggpu.h"
#if !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#ifdef __ANDROID__
#include <unwind.h>
#else
#include <execinfo.h>
#endif
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

#if defined(__ANDROID__) && !defined(_WIN32)
// Android Bionic does not export glibc backtrace(). Retain native fault
// diagnostics through the NDK's unwinder, as in the GoW3 Bionic runtime.
typedef struct { void **frames; int count, max; } IssUnwindState;
static _Unwind_Reason_Code iss_unwind_step(struct _Unwind_Context *ctx, void *user) {
    IssUnwindState *state = user;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (!pc || state->count >= state->max) return _URC_END_OF_STACK;
    state->frames[state->count++] = (void *)pc;
    return _URC_NO_REASON;
}
static int backtrace(void **frames, int max) {
    IssUnwindState state = {frames, 0, max};
    _Unwind_Backtrace(iss_unwind_step, &state);
    return state.count;
}
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
static unsigned cpu_missing_sets; /* CPU_SET_* the host lacks; the guest code needs them rewritten */
#ifdef GUEST_CPU_NATIVE
static IsaTable *isa_table; /* native PC instruction rewrites */
#endif
#ifndef _WIN32
#ifdef GUEST_CPU_NATIVE
static uintptr_t guest_code_end;
#endif /* end of the image and its stub areas (emulation, red zone); TLG_CLOBBER_RED_ZONE */
#endif
#ifdef GUEST_CPU_NATIVE
static void isa_report_hook(void) { isa_rewrite_report(isa_table, stdout); }
#endif
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           guest_cpu_return_address((uintptr_t)__builtin_return_address(0)) - (uintptr_t)image, argument);
    uintptr_t caller=guest_cpu_return_address((uintptr_t)__builtin_return_address(0))-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
/* TLG_IMPORT_TRACE=<n>: each import the host runtime resolves goes through a stub that logs the
 * call (name, first four integer arguments, caller offset) for its first n calls, then jumps to
 * the implementation. Shows which system call precedes a guest fault during bring-up. */
static unsigned char *trace_stubs;
static uintptr_t *trace_made;
static uint32_t *trace_counts, trace_limit;
#define TRACE_STUB_SIZE 192
static ABI void trace_import(uint32_t index, const uint64_t *regs) {
    /* regs: rax, r9, r8, rcx, rdx, rsi, rdi, return address */
    if (__atomic_fetch_add(&trace_counts[index], 1, __ATOMIC_RELAXED) >= trace_limit) return;
    fprintf(stderr, "IMPORT %s(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ") from +%#" PRIxPTR "\n",
            runtime_import_name(names[index]), regs[6], regs[5], regs[4], regs[3], (uintptr_t)regs[7] - (uintptr_t)image);
}
static uintptr_t trace_stub(uint64_t index, uintptr_t target) {
    if (trace_made[index]) return trace_made[index];
    unsigned char *t = trace_stubs + index * TRACE_STUB_SIZE, *p = t;
    static const unsigned char save[] = {0x57,0x56,0x52,0x51,0x41,0x50,0x41,0x51,0x50, 0x48,0x81,0xec,0x80,0,0,0};
    memcpy(p, save, sizeof(save)); p += sizeof(save);
    for (int x = 0; x < 8; ++x) { unsigned char m[] = {0xf3,0x0f,0x7f,(unsigned char)(0x44|(x<<3)),0x24,(unsigned char)(x*16)}; memcpy(p, m, 6); p += 6; }
    uint32_t id = (uint32_t)index;
    *p++ = 0xbf; memcpy(p, &id, 4); p += 4;
    static const unsigned char lea[] = {0x48,0x8d,0xb4,0x24,0x80,0,0,0};
    memcpy(p, lea, sizeof(lea)); p += sizeof(lea);
    uintptr_t logger = guest_cpu_host_function((void *)trace_import);
    *p++ = 0x48; *p++ = 0xb8; memcpy(p, &logger, 8); p += 8;
    *p++ = 0xff; *p++ = 0xd0;
    for (int x = 0; x < 8; ++x) { unsigned char m[] = {0xf3,0x0f,0x6f,(unsigned char)(0x44|(x<<3)),0x24,(unsigned char)(x*16)}; memcpy(p, m, 6); p += 6; }
    static const unsigned char restore[] = {0x48,0x81,0xc4,0x80,0,0,0, 0x58,0x41,0x59,0x41,0x58,0x59,0x5a,0x5e,0x5f};
    memcpy(p, restore, sizeof(restore)); p += sizeof(restore);
    *p++ = 0x49; *p++ = 0xbb; memcpy(p, &target, 8); p += 8;
    *p++ = 0x41; *p++ = 0xff; *p++ = 0xe3;
    if (p - t > TRACE_STUB_SIZE) fail("trace stub too large");
    return trace_made[index] = (uintptr_t)t;
}
#if defined(GUEST_CPU_NATIVE) && !defined(_WIN32)
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
#ifdef _WIN32
static void describe_address(char *out, size_t size, uintptr_t address) {
    HMODULE module=NULL;
    char name[MAX_PATH]="?";
    int site=isa_table ? isa_rewrite_site_of_stub(isa_table,address) : -1;
    uint64_t rz_site=0;
    int rz=isa_red_zone_site_of_stub(isa_table,address,&rz_site);
    if (site>=0 || (image && address-(uintptr_t)image<0x20000000)) {
        int n=snprintf(out,size,"guest offset 0x%" PRIxPTR,address-(uintptr_t)image);
        if (site>=0 && n>=0 && (size_t)n<size)
            snprintf(out+n,size-n," While emulating site 0x%" PRIx64 " (%s)",isa_table->sites[site].offset,isa_kind_name(isa_table->sites[site].kind));
        else if (rz && n>=0 && (size_t)n<size)
            snprintf(out+n,size-n," (red-zone stub of 0x%" PRIx64 ")",rz_site);
        return;
    }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)address,&module) && module) {
        GetModuleFileNameA(module,name,sizeof(name));
        const char *base=strrchr(name,'\\');
        snprintf(out,size,"%s+0x%" PRIxPTR,base ? base+1 : name,address-(uintptr_t)module);
        return;
    }
    snprintf(out,size,"%p",(void *)address);
}
static int fatal_reported;
static LONG fatal_exception(EXCEPTION_POINTERS *info) {
    if (__atomic_exchange_n(&fatal_reported,1,__ATOMIC_ACQ_REL)) { Sleep(INFINITE); }
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    const CONTEXT *c=info->ContextRecord;
    char where[MAX_PATH+64];
    describe_address(where,sizeof(where),(uintptr_t)c->Rip);
    if (record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2)
        fprintf(stderr,"Fault: %s access to %p at %s (thread %lu)\n",
                record->ExceptionInformation[0]==1 ? "write" : record->ExceptionInformation[0]==8 ? "execute" : "read",
                (void *)record->ExceptionInformation[1],where,GetCurrentThreadId());
    else
        fprintf(stderr,"Fault: exception 0x%08lx at %s (thread %lu)\n",record->ExceptionCode,where,GetCurrentThreadId());
    fprintf(stderr,"  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp);
    if (gpu_enabled) tlggpu_dump_guest_writes(info);
    /* rbp chain (guest and host code keep frame pointers). */
    uintptr_t rbp=(uintptr_t)c->Rbp;
    for (int depth=0; depth<24 && rbp; ++depth) {
        uintptr_t frame[2];
        SIZE_T got=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(void *)rbp,frame,sizeof(frame),&got) || got!=sizeof(frame) || frame[0]<=rbp) break;
        describe_address(where,sizeof(where),frame[1]);
        fprintf(stderr,"  #%d %s\n",depth,where);
        rbp=frame[0];
    }
    if (isa_table) isa_rewrite_report(isa_table,stderr);
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(),3);
    return EXCEPTION_CONTINUE_SEARCH;
}
/* CONTEXT <-> IsaRegs (gpr in x86 numbering). */
static void isa_context_regs(CONTEXT *c, IsaRegs *regs, int to_context) {
    DWORD64 *g[16]={&c->Rax,&c->Rcx,&c->Rdx,&c->Rbx,&c->Rsp,&c->Rbp,&c->Rsi,&c->Rdi,
                    &c->R8,&c->R9,&c->R10,&c->R11,&c->R12,&c->R13,&c->R14,&c->R15};
    for (int i=0;i<16;++i) { if (to_context) *g[i]=regs->gpr[i]; else regs->gpr[i]=*g[i]; }
    if (to_context) c->EFlags=(DWORD)regs->rflags; else regs->rflags=c->EFlags;
}
static LONG CALLBACK vectored_fault(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    DWORD code=record->ExceptionCode;
    if (code==EXCEPTION_BREAKPOINT && isa_table) {
        /* An int3 ISA site: emulated here; any other breakpoint goes on to the report below. */
        IsaRegs regs;
        uintptr_t rip;
        isa_context_regs(info->ContextRecord,&regs,0);
        if (isa_rewrite_trap(isa_table,(uintptr_t)record->ExceptionAddress,&regs,
                             &info->ContextRecord->FltSave.XmmRegisters[0],&rip)) {
            isa_context_regs(info->ContextRecord,&regs,1);
            info->ContextRecord->Rip=(DWORD64)rip;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2) {
        /* GPU page tracking (write-protected guest pages) is resolved first. */
        if (gpu_enabled && tlggpu_handle_fault(info,(void *)record->ExceptionInformation[1])) return EXCEPTION_CONTINUE_EXECUTION;
        if (runtime_fault_recover) {
            /* A speculative guest memory read (GPU draw preparation) faulted: the thread resumes
             * in tlg_longjmp, outside the exception dispatcher, back at its recovery point. */
            unsigned long long *buffer=*runtime_fault_recover;
            runtime_fault_recover=NULL;
            CONTEXT *c=info->ContextRecord;
            c->Rsp=((c->Rsp-128)&~(DWORD64)15)-40;
            c->Rcx=(DWORD64)(uintptr_t)buffer;
            c->Rdx=1;
            c->Rip=(DWORD64)(uintptr_t)tlg_longjmp;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_ILLEGAL_INSTRUCTION || code==EXCEPTION_PRIV_INSTRUCTION ||
        code==EXCEPTION_STACK_OVERFLOW || code==EXCEPTION_INT_DIVIDE_BY_ZERO || code==EXCEPTION_DATATYPE_MISALIGNMENT) {
        /* Faults in guest code, the loader or the runtime end the process with a report; drivers
         * and system libraries may handle their own exceptions. */
        uintptr_t rip=(uintptr_t)info->ContextRecord->Rip;
        HMODULE self=GetModuleHandleA(NULL), module=NULL;
        int ours=(image && rip-(uintptr_t)image<0x20000000) ||
                 (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&module) && module==self) ||
                 !module;
        if (ours) return fatal_exception(info);
    }
    /* TLG_STRICT_HANDLES=1: a closed or invalid handle used anywhere is reported (diagnostics). */
    if (code==0xC0000008 /* STATUS_INVALID_HANDLE */) return fatal_exception(info);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI unhandled_fault(EXCEPTION_POINTERS *info) { return fatal_exception(info); }
/* Guest code reads its TCB with `mov reg, gs:[0]` (link_*.py rewrote fs:[0]); on Windows
 * the displacement becomes the TEB TLS slot that holds the guest TCB (runtime_thread.c). */
uint32_t runtime_thread_tcb_offset(void);
static uint64_t patch_tcb_loads(const Segment *segments, uint64_t count) {
    /* gs, REX.W (48/4c), 8b, ModRM mod=00 rm=100 (any register), SIB 25, disp32 0. */
    static const unsigned char zero[4]={0};
    uint32_t displacement=runtime_thread_tcb_offset();
    uint64_t patched=0;
    for (uint64_t s=0;s<count;++s) {
        if (!(segments[s].flags&1) || segments[s].size<9) continue;
        unsigned char *at=image+segments[s].address, *end=at+segments[s].size-9;
        for (; at<=end; ++at)
            if (at[0]==0x65 && (at[1]==0x48 || at[1]==0x4c) && at[2]==0x8b && (at[3]&0xc7)==0x04 && at[4]==0x25 &&
                !memcmp(at+5,zero,4)) { memcpy(at+5,&displacement,4); ++patched; at+=8; }
    }
    return patched;
}
#endif
#ifndef _WIN32
#ifdef GUEST_CPU_NATIVE
/* ucontext gregs of IsaRegs.gpr (x86 numbering). */
static const int isa_gregs[16]={REG_RAX,REG_RCX,REG_RDX,REG_RBX,REG_RSP,REG_RBP,REG_RSI,REG_RDI,
                                REG_R8,REG_R9,REG_R10,REG_R11,REG_R12,REG_R13,REG_R14,REG_R15};
/* TLG_CLOBBER_RED_ZONE=<bytes> (test, Linux): after each guest fault the handler resolved (GPU page
 * tracking, int3 ISA sites), zero that many bytes below the interrupted rsp, as Windows' exception
 * dispatch overwrites them: reproduces the Windows-only corruption here ("1" = the 128-byte red
 * zone). Over 128 bytes the handler needs an alternate signal stack (the kernel's frame starts 128
 * bytes below rsp): each guest thread gets one; elsewhere the clobber stays at 128. The first
 * distinct guest RIPs are logged. */
static unsigned clobber_red_zone;
static void clobber_thread_init(void) {
    if (clobber_red_zone <= 128) return;
    stack_t ss = {0};
    ss.ss_size = 256 * 1024;
    ss.ss_sp = mmap(NULL, ss.ss_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ss.ss_sp != MAP_FAILED) sigaltstack(&ss, NULL);
}
static void clobber_below(ucontext_t *uc) {
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP], rsp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
    if (!clobber_red_zone || rip < (uintptr_t)image || rip >= guest_code_end) return;
    unsigned bytes = clobber_red_zone;
    stack_t current;
    if (bytes > 128 && (sigaltstack(NULL, &current) || !(current.ss_flags & SS_ONSTACK))) bytes = 128;
    memset((void *)(rsp - bytes), 0, bytes);
    static uintptr_t seen[32];
    static unsigned seen_count;
    for (unsigned i = 0; i < seen_count && i < 32; ++i) if (seen[i] == rip) return;
    unsigned slot = __atomic_fetch_add(&seen_count, 1, __ATOMIC_RELAXED);
    if (slot >= 32) return;
    seen[slot] = rip;
    char line[96];
    uint64_t site = 0;
    int in_stub = isa_red_zone_site_of_stub(isa_table, rip, &site);
    int n = snprintf(line, sizeof line, "Test: red zone clobbered (%u bytes) at guest offset 0x%lx%s%lx\n", bytes,
                     (unsigned long)(rip - (uintptr_t)image), in_stub ? ", red-zone stub of 0x" : "", in_stub ? (unsigned long)site : 0ul);
    if (!in_stub) n = snprintf(line, sizeof line, "Test: red zone clobbered (%u bytes) at guest offset 0x%lx\n", bytes,
                               (unsigned long)(rip - (uintptr_t)image));
    if (n > 0) { ssize_t written_ = write(2, line, (size_t)n); (void)written_; }
}
static void fault(int sig, siginfo_t *info, void *context) {
    if (sig == SIGTRAP && isa_table) {
        /* An int3 ISA site (RIP is past the CC byte); any other trap ends in the report below. */
        greg_t *g = ((ucontext_t *)context)->uc_mcontext.gregs;
        IsaRegs regs;
        uintptr_t rip;
        for (int i = 0; i < 16; ++i) regs.gpr[i] = (uint64_t)g[isa_gregs[i]];
        regs.rflags = (uint64_t)g[REG_EFL];
        /* XMM0-15 of the signal frame (FXSAVE layout at 160): an SSE4A site changes one of them,
         * and the kernel restores them on return. */
        unsigned char *fpu = (unsigned char *)((ucontext_t *)context)->uc_mcontext.fpregs;
        if (isa_rewrite_trap(isa_table, (uintptr_t)g[REG_RIP] - 1, &regs, fpu ? fpu + 160 : NULL, &rip)) {
            /* XSAVE frame (FP_XSTATE_MAGIC1): XSTATE_BV bit 1 (SSE) set, or XRSTOR would reset the
             * XMM registers instead of loading the changed ones. XSAVE wrote their real values. */
            uint32_t magic;
            if (fpu && (memcpy(&magic, fpu + 464, 4), magic == 0x46505853u)) fpu[512] |= 2;
            for (int i = 0; i < 16; ++i) g[isa_gregs[i]] = (greg_t)regs.gpr[i];
            g[REG_EFL] = (greg_t)regs.rflags;
            g[REG_RIP] = (greg_t)rip;
            clobber_below(context);
            return;
        }
    }
    /* A speculative guest memory read (GPU draw-preparation workers) failed: resume its recovery
     * point before any readback. With precise readbacks the page may be read-protected because
     * the GPU wrote it; flushing it from a GPU worker waits on that worker's own pipeline
     * (deadlock), while giving up leaves the draw to the GPU thread, which reads it safely. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* GPU page tracking (write-protected guest pages) is resolved next. */
    if (gpu_enabled && sig == SIGSEGV && tlggpu_handle_fault(context, info->si_addr)) {
        clobber_below(context);
        return;
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *uc = context;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    char line[512];
    Dl_info where;
    int site = isa_table ? isa_rewrite_site_of_stub(isa_table, rip) : -1;
    char emulating[64] = "";
    uint64_t rz_site = 0;
    if (site >= 0)
        snprintf(emulating, sizeof(emulating), " While emulating site 0x%" PRIx64 " (%s)",
                 isa_table->sites[site].offset, isa_kind_name(isa_table->sites[site].kind));
    else if (isa_red_zone_site_of_stub(isa_table, rip, &rz_site))
        snprintf(emulating, sizeof(emulating), " (red-zone stub of 0x%" PRIx64 ")", rz_site);
    if (site >= 0 || rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p%s\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr, emulating);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (gpu_enabled) tlggpu_dump_guest_writes(context);
    /* Engine ASSERT (int 0x41): the formatted "file(line): func() ASSERT(cond)" message sits in a
     * stack buffer of the asserting frame; its logger may be null, so print it from here. */
    if (rip - (uintptr_t)image < 0x10000000) {
        unsigned char op[2] = {0};
        struct iovec lo = {op, 2}, ro = {(void *)rip, 2};
        if (process_vm_readv(getpid(), &lo, 1, &ro, 1, 0) == 2 && op[0] == 0xcd && op[1] == 0x41) {
            static char stack[0x4000];
            uintptr_t rsp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
            struct iovec ls = {stack, sizeof(stack) - 1}, rs = {(void *)rsp, sizeof(stack) - 1};
            ssize_t got = process_vm_readv(getpid(), &ls, 1, &rs, 1, 0);
            for (ssize_t i = 0; got > 0 && i + 8 < got; ++i) {
                if (!memcmp(stack + i, "D:\\trico", 8) || !memcmp(stack + i, "C:\\", 3)) {
                    stack[got] = 0;
                    const char *msg = stack + i;
                    if (strstr(msg, "ASSERT")) {
                        snprintf(line, sizeof(line), "  engine assert: %.400s\n", msg);
                        ssize_t written_=write(2, line, strlen(line)); (void)written_;
                        break;
                    }
                }
            }
        }
    }
    /* Guest call chain: the eboot keeps frame pointers, so follow rbp (reads cannot fault). */
    if (rip - (uintptr_t)image < 0x10000000) {
        uintptr_t rbp = (uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
        static const char *regs[] = {"rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp"};
        const int ids[] = {REG_RAX,REG_RBX,REG_RCX,REG_RDX,REG_RSI,REG_RDI,REG_RBP,REG_RSP};
        for (int r = 0; r < 8; ++r) {
            snprintf(line, sizeof(line), "  %s=%016llx%s", regs[r], (unsigned long long)uc->uc_mcontext.gregs[ids[r]], r == 3 || r == 7 ? "\n" : " ");
            ssize_t written_=write(2, line, strlen(line)); (void)written_;
        }
        for (int depth = 0; depth < 24 && rbp; ++depth) {
            uintptr_t frame[2];
            struct iovec local = {frame, sizeof(frame)}, remote = {(void *)rbp, sizeof(frame)};
            if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)sizeof(frame) || frame[0] <= rbp) break;
            if (frame[1] - (uintptr_t)image < 0x10000000)
                snprintf(line, sizeof(line), "  guest #%d return 0x%lx\n", depth, (unsigned long)(frame[1] - (uintptr_t)image));
            else
                snprintf(line, sizeof(line), "  guest #%d return %p (host)\n", depth, (void *)frame[1]);
            ssize_t written_=write(2, line, strlen(line)); (void)written_;
            rbp = frame[0];
        }
    }
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    if (isa_table) { isa_rewrite_report(isa_table, stderr); fflush(stderr); }
    _exit(128 + sig);
}
#else
static unsigned clobber_red_zone;
static void clobber_thread_init(void) {}
static void fault(int sig, siginfo_t *info, void *context) {
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (gpu_enabled && sig == SIGSEGV && tlggpu_handle_fault(context, info->si_addr)) return;
    /* Faults the guest CPU itself raises (JIT bookkeeping) are handled and resumed. */
    if (guest_cpu_handle_fault(sig, info, context)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    GuestRegs regs;
    const int in_guest = guest_cpu_signal_regs(context, &regs);
    uintptr_t rip = in_guest == GUEST_CPU_IN_GUEST ? (uintptr_t)regs.rip : guest_cpu_host_pc(context);
    char line[512];
    Dl_info where;
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
#if defined(__ANDROID__) && defined(__aarch64__)
    // PC=0: LR points at the native callsite that jumped through NULL.
    // Print a single symbolized return address in the crash report.
    const ucontext_t *arm = (const ucontext_t *)context;
    uintptr_t lr = (uintptr_t)arm->uc_mcontext.regs[30];
    if (lr) {
        if (dladdr((void *)(lr - 4), &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  native caller LR: %s+0x%lx (%s)\n",
                     where.dli_fname, (unsigned long)(lr - 4 - (uintptr_t)where.dli_fbase),
                     where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  native caller LR: %p\n", (void *)lr);
        { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    }
#endif
    if (gpu_enabled) tlggpu_dump_guest_writes(context);
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    _exit(128 + sig);
}
#endif

#endif
#ifndef _WIN32
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    GuestRegs regs; guest_cpu_signal_regs(uc,&regs);
    uintptr_t rip=regs.rip, rbp=regs.gpr[GUEST_RBP];
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,regs.gpr[GUEST_RDI]);
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    {   /* Thread name (the process is terminating: stdio-free reads are enough). */
        char path[64], name[32]={0};
        snprintf(path,sizeof(path),"/proc/self/task/%lu/comm",(unsigned long)tid);
        int fd=open(path,O_RDONLY);
        if (fd>=0) { ssize_t got=read(fd,name,sizeof(name)-1); (void)got; close(fd); }
        char head[96]; snprintf(head,sizeof(head),"  tid=%lu name=%s",(unsigned long)tid,name[0] ? name : "?\n");
        { ssize_t written_=write(2,head,strlen(head)); (void)written_; }
    }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        Dl_info where;
        if (rip-(uintptr_t)image>=0x10000000 && dladdr((void *)rip,&where) && where.dli_fname) {
            char sym[320];
            const char *file=strrchr(where.dli_fname,'/');
            snprintf(sym,sizeof(sym),"      = %s+0x%lx (%s)\n",file ? file+1 : where.dli_fname,
                     (unsigned long)(rip-(uintptr_t)where.dli_fbase),where.dli_sname ? where.dli_sname : "?");
            ssize_t written_=write(2,sym,strlen(sym)); (void)written_;
        }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
static void dump_all_threads(void *context, const char *head) {
    { ssize_t written_=write(2,head,strlen(head)); (void)written_; }
    dump_frames(context);
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
    if (dir>=0) close(dir);
    usleep(100000);
}
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    dump_all_threads(context, "STOP: watchdog timeout; thread stacks:\n");
    _exit(128 + sig);
}
/* kill -USR1 <pid>: dump every thread's stack into the log and keep running (diagnose hangs). */
static void dump_request(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    dump_all_threads(context, "DUMP: thread stacks on request (SIGUSR1); the game keeps running\n");
    const char tail[]="DUMP: end\n";
    ssize_t written_=write(2,tail,sizeof(tail)-1); (void)written_;
}
#endif
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
/* inFAMOUS camera update, decrypted 1.07 SELF SHA256 2d1ca796...
 * 0x582514: mov rax,[r15]; the view/projection matrices have just been written
 * by 0x5821b0..0x582513. The FEX hook must execute the replaced load itself
 * before continuing at 0x582517. Never intercept unknown executable revisions.
 */
#if !defined(GUEST_CPU_NATIVE)
enum { ISS_CAMERA_FUNCTION = 0x5821b0, ISS_CAMERA_HOOK = 0x582514,
       ISS_CAMERA_FUNCTION_LENGTH = 0x37d };
static void iss_camera_updated(GuestRegs *regs) {
    const uintptr_t object = (uintptr_t)regs->gpr[GUEST_RBX];
    if (object) {
        TlgIssCameraSnapshot sample = {0};
        sample.guest_object = object;
        memcpy(sample.projection, (const void *)(object + 0x1c0), sizeof(sample.projection));
        memcpy(sample.view_transform, (const void *)(object + 0x200), sizeof(sample.view_transform));
        memcpy(sample.composite, (const void *)(object + 0x430), sizeof(sample.composite));
        memcpy(sample.inverse, (const void *)(object + 0x4b0), sizeof(sample.inverse));
        memcpy(&sample.near_plane, (const void *)(object + 0x584), sizeof(float));
        memcpy(&sample.far_plane, (const void *)(object + 0x588), sizeof(float));
        memcpy(&sample.vertical_fov, (const void *)(object + 0x590), sizeof(float));
        memcpy(&sample.aspect, (const void *)(object + 0x59c), sizeof(float));
        memcpy(&sample.tangent_horizontal, (const void *)(object + 0x5a0), sizeof(float));
        memcpy(&sample.tangent_vertical, (const void *)(object + 0x5a4), sizeof(float));
        tlggpu_iss_camera_publish(&sample);
    }
    /* Preserve guest RAX: the replaced instruction is exactly mov rax,[r15].
     * No x86 flags, vector register or stack content are modified. */
    const uintptr_t canary = (uintptr_t)regs->gpr[GUEST_R15];
    if (canary) memcpy(&regs->gpr[GUEST_RAX], (const void *)canary, sizeof(uint64_t));
    regs->rip += 3;
}
static void install_iss_camera_capture(const char *app0, Segment *segments,
                                       uint64_t count, uint64_t size) {
    char path[4096], serial[32] = "";
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
    sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
    if (strcmp(serial, "CUSA00004") && strcmp(serial, "CUSA00223") &&
        strcmp(serial, "CUSA00263")) return;
    if (size < ISS_CAMERA_FUNCTION + ISS_CAMERA_FUNCTION_LENGTH ||
        !mapped(segments, count, ISS_CAMERA_FUNCTION, ISS_CAMERA_FUNCTION_LENGTH)) {
        puts("FSR3 ISS camera: unsupported image range, no hook installed");
        return;
    }
    const unsigned char *function = image + ISS_CAMERA_FUNCTION;
    uint64_t fingerprint = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < ISS_CAMERA_FUNCTION_LENGTH; ++i) {
        fingerprint ^= function[i];
        fingerprint *= UINT64_C(0x100000001b3);
    }
    static const unsigned char original[] = {0x49, 0x8b, 0x07};
    if (fingerprint != UINT64_C(0x63f0e204edc47713) ||
        memcmp(image + ISS_CAMERA_HOOK, original, sizeof(original))) {
        puts("FSR3 ISS camera: function identity mismatch, hook disabled");
        return;
    }
    if (!guest_cpu_hook((uintptr_t)(image + ISS_CAMERA_HOOK), sizeof(original),
                        iss_camera_updated)) {
        puts("FSR3 ISS camera: FEX hook unavailable");
        return;
    }
    puts("FSR3 ISS camera: verified guest update capture active (post-matrix, 4x4)");
}
#else
static void install_iss_camera_capture(const char *app0, Segment *segments,
                                       uint64_t count, uint64_t size) {
    (void)app0; (void)segments; (void)count; (void)size;
}
#endif

/* inFAMOUS native SMAA T2x vs FSR3: the verified CUSA00004 eboot has
 * the same post-processing instruction signatures as upstream iss_temporal_hook,
 * at the original offsets (verified against SELF SHA-256 2d1ca796...). In the SMAA branch FUN_00472810 applies the game's own
 * temporal camera window and its AA pass accumulates before Android FSR3.
 * FSR3 owns temporal reconstruction. Choose the game's FXAA/spatial branch,
 * WITHOUT skipping the AA pass (which also composites scene colour).
 *
 * On FEX the hook replaces cmp [rcx],0 ; je 0x4729e0 in one syscall trap.
 * HandleSyscall clobbers RCX/R11, so we never read them. The original branch
 * when the AA mode byte is zero jumps to 0x4729e0. The next instruction
 * overwrites flags, so the jumped path has the same architectural inputs.
 *
 * No texture, depth, velocity, FSR timing, or HUD redirection changes.
 * Experimental disable: ISS_FSR3_DISABLE_NATIVE_T2X=0. */
#if !defined(GUEST_CPU_NATIVE)
enum {
    ISS_AA_JITTER_BEFORE = 0x47293c,
    ISS_AA_JITTER_SITE = 0x472944,
    ISS_AA_JITTER_NEXT = 0x47294d,
    ISS_AA_JITTER_FXAA = 0x4729e0,
    ISS_AA_ENTRY = 0x1929c0,
    ISS_AA_SITE = 0x192c4c,
    ISS_AA_NEXT = 0x192c59,
    ISS_AA_DONE = 0x192cff,
    ISS_AA_MODE = 0xda07f9
};
static const unsigned char iss_aa_before_bytes[] = {0x41,0x88,0x9c,0x24,0x2c,0xdc,0x01,0x00};
static const unsigned char iss_aa_jitter_bytes[] = {0x80,0x39,0x00,0x0f,0x84,0x93,0x00,0x00,0x00};
static const unsigned char iss_aa_next_bytes[] = {0x41,0xc7,0x84,0x24,0x10,0xcd,0x01,0x00,0x00,0x00,0x00,0x00};
static const unsigned char iss_aa_entry_bytes[] = {
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
    0x48,0x81,0xec,0x18,0x08,0x00,0x00};
static const unsigned char iss_aa_site_bytes[] = {
    0x48,0x8d,0x05,0x7d,0x90,0xd8,0x00,0x8a,0x80,0x05,0x0f,0xdf,0x00};
static const unsigned char iss_aa_after_bytes[] = {
    0x41,0x0a,0x86,0x8d,0xeb,0x01,0x00,0x0f,0x85,0x99,0x00,0x00,0x00};
static const unsigned char iss_aa_done_bytes[] = {0x48,0x8d,0x1d,0xa2,0x91,0xbb,0x07};
static const unsigned char iss_aa_fx_target_bytes[] = {0x49,0x8b,0x06,0x48,0x3b,0x45,0xd0};
static const unsigned char iss_aa_done_site_bytes[] = {0x48,0x8d,0x1d,0xa2,0x91,0xbb,0x07};
static const unsigned char iss_aa_done_next_bytes[] = {0x4c,0x8d,0x3d,0xd3,0xe4,0xd7,0x07};

/* Exact inFAMOUS AA marker. Unlike guessing stencil/UI shader states, this
 * appears in the PM4 stream immediately after the game's AA/colour composite.
 * Uses a compact NOP with [magic, version, GPU address lo/hi, width, height].
 * If the command writer is full, skip the marker and retain the safe late FSR.
 * If FSR is off or the eboot signature differs, this callback is not installed. */
enum { ISS_AA_DONE_NEXT = 0x192d06 };
/* Preserve the AA's original input image as well as the output image.
 * The upstream inFAMOUS PC hook records both, but the Android marker only
 * reported the output. The extra capture is completely passive: FSR3 still
 * consumes the same verified output and runs at the same command boundary.
 * One tagged snapshot per guest thread avoids mixing gameplay and menu draws. */
static __thread uintptr_t iss_aa_pending_context;
static __thread uint64_t iss_aa_pending_input;
static __thread uint64_t iss_aa_pending_output;
static uint64_t iss_aa_descriptor_gpu_address(uintptr_t object) {
    if (object <= 0x10000 || object >= (UINT64_C(1) << 40)) return 0;
    uint32_t desc[3];
    memcpy(desc, (const void *)(object + 0xb4), sizeof(desc));
    return (((uint64_t)(desc[1] & 0x3fu) << 32) | desc[0]) << 8;
}
static void iss_fsr3_aa_begin_marker(GuestRegs *regs) {
    const uintptr_t context = (uintptr_t)regs->gpr[GUEST_R13];
    const uintptr_t input = (uintptr_t)regs->gpr[GUEST_R12];
    const uintptr_t output = (uintptr_t)regs->gpr[GUEST_R15];
    const uintptr_t post = (uintptr_t)regs->gpr[GUEST_R14];
    const uintptr_t game_flags = (uintptr_t)(image + 0xf1bcd0);
    const unsigned char disabled = *(const unsigned char *)(game_flags + 0xdf0f05);
    iss_aa_pending_context = 0;
    iss_aa_pending_input = iss_aa_pending_output = 0;
    if (!disabled && post > 0x10000 && post < (UINT64_C(1) << 40) &&
        !*((const unsigned char *)(post + 0x1eb8d)) &&
        context > 0x10000 && context < (UINT64_C(1) << 40)) {
        const uint64_t input_addr = iss_aa_descriptor_gpu_address(input);
        const uint64_t output_addr = iss_aa_descriptor_gpu_address(output);
        if (input_addr && output_addr) {
            iss_aa_pending_context = context;
            iss_aa_pending_input = input_addr;
            iss_aa_pending_output = output_addr;
        }
    }
    /* Replace precisely these guest instructions:
     *   lea rax, [rip+0xd8907d] ; mov al, [rax+0xdf0f05]
     * Neither changes CPU flags. Keep all other GPRs untouched. */
    regs->gpr[GUEST_RAX] = (game_flags & ~UINT64_C(0xff)) | disabled;
    regs->rip = (uintptr_t)(image + ISS_AA_NEXT);
}

static unsigned iss_aa_marker_wrote, iss_aa_marker_skipped;
static void iss_fsr3_aa_done_marker(GuestRegs *regs) {
    const uintptr_t context = (uintptr_t)regs->gpr[GUEST_R13];
    const uintptr_t output = (uintptr_t)regs->gpr[GUEST_R15];
    const uint64_t aa_input_addr = iss_aa_pending_context == context &&
        iss_aa_pending_output == iss_aa_descriptor_gpu_address(output)
        ? iss_aa_pending_input : 0;
    iss_aa_pending_context = 0;
    iss_aa_pending_input = iss_aa_pending_output = 0;
    // Mirror the game's own two "no AA" gates at 0x192c59/0x192c60.
    // Without these gates the DONE site also fires in menus and photo mode.
    const uintptr_t post = (uintptr_t)regs->gpr[GUEST_R14];
    const int aa_enabled = post > 0x10000 && post < (UINT64_C(1) << 40) &&
        image[0xf1bcd0 + 0xdf0f05] == 0 &&
        *((const unsigned char *)(post + 0x1eb8d)) == 0;
    if (aa_enabled && context > 0x10000 && output > 0x10000 &&
        context < (UINT64_C(1) << 40) && output < (UINT64_C(1) << 40)) {
        uint32_t descriptor[8];
        memcpy(descriptor, (const void *)(output + 0xb4), sizeof(descriptor));
        const uint64_t gpu_address =
            ((uint64_t)(descriptor[1] & 0x3f) << 32 | descriptor[0]) << 8;
        const uint32_t width = (descriptor[2] & 0x3fff) + 1;
        const uint32_t height = ((descriptor[2] >> 14) & 0x3fff) + 1;
        if (gpu_address && width >= 320 && height >= 180 &&
            width <= 8192 && height <= 8192) {
            unsigned char *writer = (unsigned char *)context + 8;
            unsigned char *end = NULL, *cursor = NULL;
            memcpy(&end, writer + 8, sizeof(end));
            memcpy(&cursor, writer + 0x10, sizeof(cursor));
            if (end && cursor && end >= cursor && (size_t)(end-cursor) >= 36) {
                const uint32_t marker[9] = {
                    0xc0071000u, 0x49534101u, 1u,
                    (uint32_t)gpu_address, (uint32_t)(gpu_address >> 32),
                    width, height,
                    (uint32_t)aa_input_addr, (uint32_t)(aa_input_addr >> 32),
                };
                memcpy(cursor, marker, sizeof(marker));
                cursor += sizeof(marker);
                memcpy(writer + 0x10, &cursor, sizeof(cursor));
                if (++iss_aa_marker_wrote <= 3) {
                    fprintf(stderr, "FSR3 ISS AA marker written #%u: GPU=0x%llx %ux%u\n",
                            iss_aa_marker_wrote, (unsigned long long)gpu_address, width, height);
                }
            } else if (++iss_aa_marker_skipped <= 3) {
                fputs("FSR3 ISS AA marker: command writer has no room; late fallback\n", stderr);
            }
        }
    }
    /* The replaced instruction is lea rbx,[rip+0x7bb91a2]. */
    regs->gpr[GUEST_RBX] = (uintptr_t)(image + 0x7d4bea8);
    regs->rip = (uintptr_t)(image + ISS_AA_DONE_NEXT);
}

static unsigned iss_aa_reported;
static void iss_fsr3_avoid_game_t2x(GuestRegs *regs) {
    /* This guest global is read at 0x192c66 and chooses the SMAA T2x
     * branch when nonzero. Zero chooses FXAA. Preserve all GPRs. */
    unsigned char *mode = image + ISS_AA_MODE;
    const unsigned char original = *mode;
    *mode = 0;
    if (!iss_aa_reported++) {
        fprintf(stderr,
                "FSR3 ISS native AA: original game mode=%u (%s), forcing FXAA "
                "instead of SMAA T2x; FSR3 still owns temporal upscale\\n",
                (unsigned)original, original ? "SMAA T2x" : "already FXAA");
    }
    regs->rip = (uintptr_t)(image + ISS_AA_JITTER_FXAA);
}
static void install_iss_fsr3_native_aa(const char *app0, Segment *segments,
                                      uint64_t count, uint64_t size) {
    const char *upscaler = getenv("TLG_UPSCALER");
    if (!upscaler || strcmp(upscaler,"fsr3")) return;
    const char *disable = getenv("ISS_FSR3_DISABLE_NATIVE_T2X");
    if (disable && disable[0] == '0') return;
    char path[4096], serial[32] = "";
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
    sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
    if (strcmp(serial,"CUSA00004") && strcmp(serial,"CUSA00223") &&
        strcmp(serial,"CUSA00263")) return;
    struct { uint64_t address; const unsigned char *bytes; size_t length; } checks[] = {
        {ISS_AA_JITTER_BEFORE, iss_aa_before_bytes, sizeof(iss_aa_before_bytes)},
        {ISS_AA_JITTER_SITE, iss_aa_jitter_bytes, sizeof(iss_aa_jitter_bytes)},
        {ISS_AA_JITTER_NEXT, iss_aa_next_bytes, sizeof(iss_aa_next_bytes)},
        {ISS_AA_JITTER_FXAA, iss_aa_fx_target_bytes, sizeof(iss_aa_fx_target_bytes)},
        {ISS_AA_ENTRY, iss_aa_entry_bytes, sizeof(iss_aa_entry_bytes)},
        {ISS_AA_SITE, iss_aa_site_bytes, sizeof(iss_aa_site_bytes)},
        {ISS_AA_NEXT, iss_aa_after_bytes, sizeof(iss_aa_after_bytes)},
        {ISS_AA_DONE, iss_aa_done_bytes, sizeof(iss_aa_done_bytes)},
        {ISS_AA_DONE_NEXT, iss_aa_done_next_bytes, sizeof(iss_aa_done_next_bytes)}
    };
    if (!mapped(segments,count,ISS_AA_MODE,1) || ISS_AA_MODE >= size) return;
    for (size_t i=0;i<sizeof(checks)/sizeof(checks[0]);++i) {
        if (!mapped(segments,count,checks[i].address,checks[i].length) ||
            checks[i].address+checks[i].length > size ||
            memcmp(image+checks[i].address,checks[i].bytes,checks[i].length)) {
            fprintf(stderr,"FSR3 ISS native AA: signature mismatch at guest offset 0x%llx, no change\\n", (unsigned long long)checks[i].address);
            return;
        }
    }
    if (!guest_cpu_hook((uintptr_t)(image+ISS_AA_JITTER_SITE),
                        sizeof(iss_aa_jitter_bytes),iss_fsr3_avoid_game_t2x)) {
        fputs("FSR3 ISS native AA: guest hook unavailable, no change\\n",stderr);
        return;
    }
    // The verified guest hook skips FUN_00581a50, so the native SMAA T2x
    // projection jitter is no longer applied. Tell the FSR renderer that it
    // must use its FULL Halton sequence instead of the old 0.5x blend.
    setenv("ISS_FSR3_NATIVE_T2X_DISABLED", "1", 1);
    puts("FSR3 ISS native AA: verified AA selection hook installed (FXAA + FSR3)");
    const char *marker_off = getenv("ISS_FSR3_AA_MARKER");
    if (!marker_off || marker_off[0] != '0') {
        if (!guest_cpu_hook((uintptr_t)(image + ISS_AA_SITE),
                            sizeof(iss_aa_site_bytes), iss_fsr3_aa_begin_marker)) {
            fputs("FSR3 ISS AA input hook unavailable; output-only marker will continue\n", stderr);
        }
        if (guest_cpu_hook((uintptr_t)(image + ISS_AA_DONE),
                           sizeof(iss_aa_done_site_bytes), iss_fsr3_aa_done_marker)) {
            puts("FSR3 ISS AA PM4 marker installed (after verified AA pass)");
        } else {
            puts("FSR3 ISS AA marker hook unavailable; keeping late FSR3 fallback");
        }
    }
}
#else
static void install_iss_fsr3_native_aa(const char *app0, Segment *segments,
                                      uint64_t count, uint64_t size) {
    (void)app0; (void)segments; (void)count; (void)size;
}
#endif

static void install_infamous_menu_watch(const char *app0,Segment *segments,uint64_t count,uint64_t size){
    char path[4096],serial[32]="";
    snprintf(path,sizeof(path),"%s/sce_sys/param.sfo",app0?app0:".");
    sfo_value(path,"TITLE_ID",serial,sizeof(serial),NULL);
    if(strcmp(serial,"CUSA00004")&&strcmp(serial,"CUSA00223")&&strcmp(serial,"CUSA00263"))return;
    if(!mapped(segments,count,ISS_MENU_OPEN_SITE,sizeof(iss_menu_open_signature)) ||
       !mapped(segments,count,ISS_MENU_CLOSE_SITE,sizeof(iss_menu_close_signature)) ||
       !mapped(segments,count,ISS_MENU_COUNT_OFFSET,4) || !iss_menu_signatures_match(image,size)){
        puts("Input: game menu state unavailable (executable signature mismatch); Start/manual fallback");return;
    }
    runtime_pad_menu_counter((const uint32_t *)(image+ISS_MENU_COUNT_OFFSET));
    puts("Input: real inFAMOUS menu lifecycle enabled (read-only counter)");
}
extern ABI float tlg_orbs_gain(void *player, float requested);
extern void tlg_orbs_status(int supported);
extern uintptr_t tlg_cheat_flag(unsigned id);
extern uintptr_t tlg_actor_slots_address(void);
extern void tlg_actor_status(int supported);
extern void tlg_cheat_status(unsigned id);
extern uintptr_t tlg_damage_multiplier(int taken);
extern uintptr_t tlg_orb_pickup_multiplier(unsigned kind);
extern void tlg_stat_multipliers_status(int damage, int orb_pickup);

static void install_cheats(const char *app0, Segment *segments, uint64_t count, uint64_t size) {
    char path[4096], serial[32] = "", version[32] = "";
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
    sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
    sfo_value(path, "APP_VER", version, sizeof(version), NULL);
    if (strcmp(serial, "CUSA01623") || strcmp(version, "01.02")) return;
    const char *names[] = {"health", "magic", "item meter", "Rage of Sparta", "red orbs",
                           "Very Hard unlock", "infinite double jumps"};
    for (unsigned id = 0; id < TLG_CHEAT_COUNT; ++id) {
        int valid = 1;
        for (unsigned i = 0; i < TLG_CHEAT_HOOK_COUNT; ++i) {
            const TlgCheatHook *hook = &tlg_cheat_hooks[i];
            if (hook->id != id) continue;
            if (hook->offset > size || hook->length > size - hook->offset ||
                !mapped(segments, count, hook->offset, hook->length) ||
                !tlg_cheat_signature_match(hook, image + hook->offset, size - hook->offset)) valid = 0;
        }
        if (!valid) {
            printf("Cheats: %s unavailable (code signature mismatch)\n", names[id]);
            continue;
        }
        unsigned char *stubs = allocate(page_size);
        int32_t relative[TLG_CHEAT_HOOK_COUNT] = {0};
        for (unsigned i = 0; i < TLG_CHEAT_HOOK_COUNT; ++i) {
            const TlgCheatHook *hook = &tlg_cheat_hooks[i];
            if (hook->id != id) continue;
            unsigned char *stub = stubs + i * 256;
            int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + hook->offset + 5);
            if (delta < INT32_MIN || delta > INT32_MAX) { valid = 0; break; }
            relative[i] = (int32_t)delta;
            tlg_cheat_build_stub(stub, hook, tlg_cheat_flag(id),
                                 (uintptr_t)(image + hook->offset + hook->length));
        }
        if (!valid) {
            printf("Cheats: %s unavailable (hook outside relative-jump range)\n", names[id]);
            continue;
        }
        protect(stubs, page_size, 5);
        guest_cpu_code((uintptr_t)stubs, page_size); /* FEX must know this is executable */
        for (unsigned i = 0; i < TLG_CHEAT_HOOK_COUNT; ++i) {
            const TlgCheatHook *hook = &tlg_cheat_hooks[i];
            if (hook->id != id) continue;
            image[hook->offset] = 0xe9;
            memcpy(image + hook->offset + 1, &relative[i], 4);
            memset(image + hook->offset + 5, 0x90, hook->length - 5);
        }
        tlg_cheat_status(id);
        printf("Cheats: %s ready (live toggle, initially disabled unless saved)\n", names[id]);
    }
}

/* Damage hook for the enemy health bar (src/actor_hook.h). Always
 * installed when the code matches: it only records values; the overlay decides what to show. */
static void install_actor_watch(const char *app0, Segment *segments, uint64_t count, uint64_t size) {
    char path[4096], serial[32] = "", version[32] = "";
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
    sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
    sfo_value(path, "APP_VER", version, sizeof(version), NULL);
    const uint64_t at = TLG_ACTOR_HOOK_OFFSET;
    if (strcmp(serial, "CUSA01623") || strcmp(version, "01.02") || at > size ||
        !mapped(segments, count, at, TLG_ACTOR_HOOK_LENGTH) ||
        !tlg_actor_signature_match(image + at, size - at)) {
        printf("Health display: unavailable (game version or code signature mismatch)\n");
        return;
    }
    unsigned char *stub = allocate(page_size);
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) {
        printf("Health display: unavailable (hook outside relative-jump range)\n");
        return;
    }
    tlg_actor_build_stub(stub, tlg_actor_slots_address(), (uintptr_t)(image + at + TLG_ACTOR_HOOK_LENGTH));
    protect(stub, page_size, 5);
    guest_cpu_code((uintptr_t)stub, page_size);
    const int32_t relative = (int32_t)delta;
    image[at] = 0xe9;
    memcpy(image + at + 1, &relative, 4);
    memset(image + at + 5, 0x90, TLG_ACTOR_HOOK_LENGTH - 5);
    tlg_actor_status(1);
    printf("Health display: damage hook ready (enemy health bar in the menu)\n");
}

static void install_orb_multiplier(const char *app0, Segment *segments, uint64_t count,
                                   uint64_t size) {
    char path[4096], serial[32] = "", version[32] = "";
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
    sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
    sfo_value(path, "APP_VER", version, sizeof(version), NULL);
    if (strcmp(serial, "CUSA01623") || strcmp(version, "01.02") ||
        !tlg_orb_signatures_match(image, size)) {
        printf("Red orbs: multiplier unavailable (game version or gain signatures mismatch)\n");
        return;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!mapped(segments, count, tlg_orb_calls[i], 5)) return;
    }
    /* SysV call sites: preserve the receiver across the helper, then tail-call the original
     * setter. Its cap and upgrade notifications remain in the game's own code. */
    unsigned char *stub = allocate(page_size);
    uintptr_t helper = (uintptr_t)tlg_orbs_gain, setter = (uintptr_t)(image + 0x6a9f0);
    unsigned char code[26];
    tlg_orb_build_stub(code, helper, setter);
    int32_t displacements[3];
    for (unsigned i = 0; i < 3; ++i) {
        int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + tlg_orb_calls[i] + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) {
            printf("Red orbs: multiplier unavailable (hook outside relative-call range)\n");
            return;
        }
        displacements[i] = (int32_t)delta;
    }
    memcpy(stub, code, sizeof(code));
    protect(stub, page_size, 5);
    guest_cpu_code((uintptr_t)stub, page_size);
    for (unsigned i = 0; i < 3; ++i)
        memcpy(image + tlg_orb_calls[i] + 1, &displacements[i], 4);
    tlg_orbs_status(1);
    printf("Red orbs: live multiplier ready (3 validated gain paths, 0.1x to 100x)\n");
}
/* Writes a jump from image+at to `stub`; the rest of the replaced bytes become nops (the stub
 * replays them). 0 when the stub is out of relative-jump range. */
static int install_jump(uint64_t at, unsigned length, unsigned char *stub) {
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) return 0;
    const int32_t relative = (int32_t)delta;
    image[at] = 0xe9;
    memcpy(image + at + 1, &relative, 4);
    memset(image + at + 5, 0x90, length - 5);
    return 1;
}

/* Damage dealt/taken and green/blue/gold orb multipliers (src/damage_hook.h, src/orb_hook.h):
 * the stubs read the menu's floats directly, so changes apply on the next hit or pickup. */
static void install_stat_multipliers(const char *app0, Segment *segments, uint64_t count, uint64_t size) {
    char path[4096], serial[32] = "", version[32] = "";
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
    sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
    sfo_value(path, "APP_VER", version, sizeof(version), NULL);
    const int game = !strcmp(serial, "CUSA01623") && !strcmp(version, "01.02");
    const uint64_t damage_at = TLG_DAMAGE_HOOK_OFFSET, pickup_at = TLG_ORB_PICKUP_OFFSET;
    int damage = game && damage_at < size && mapped(segments, count, damage_at, TLG_DAMAGE_HOOK_LENGTH) &&
                 tlg_damage_signature_match(image + damage_at, size - damage_at);
    int pickup = game && pickup_at < size && mapped(segments, count, pickup_at, TLG_ORB_PICKUP_LENGTH) &&
                 tlg_orb_pickup_signature_match(image + pickup_at, size - pickup_at);
    unsigned char *stubs = damage || pickup ? allocate(page_size) : NULL;
    if (damage) {
        tlg_damage_build_stub(stubs, tlg_damage_multiplier(1), tlg_damage_multiplier(0),
                               (uintptr_t)(image + damage_at + TLG_DAMAGE_HOOK_LENGTH));
    }
    if (pickup) {
        tlg_orb_pickup_build_stub(stubs + 256, tlg_orb_pickup_multiplier(0), tlg_orb_pickup_multiplier(1),
                                   tlg_orb_pickup_multiplier(2),
                                   (uintptr_t)(image + pickup_at + TLG_ORB_PICKUP_LENGTH));
    }
    if (stubs) {
        protect(stubs, page_size, 5);
        guest_cpu_code((uintptr_t)stubs, page_size);
    }
    damage = damage && install_jump(damage_at, TLG_DAMAGE_HOOK_LENGTH, stubs);
    pickup = pickup && install_jump(pickup_at, TLG_ORB_PICKUP_LENGTH, stubs + 256);
    tlg_stat_multipliers_status(damage, pickup);
    printf("Multipliers: damage %s, green/blue/gold orbs %s\n", damage ? "ready" : "unavailable",
           pickup ? "ready" : "unavailable");
}
extern uintptr_t tlg_view_region_params(void);
extern uintptr_t tlg_view_fade_inverse(void);
extern void tlg_view_attach(unsigned char *guest_image);

/* Draw distance, LOD and clip planes (src/view_hook.h): the menu's values reach the game's data
 * through tlg_view_attach; the regions' LOD_ADJUST and FADE_DISTANCE go through four stubs. */
static void install_view_tuning(const char *app0, Segment *segments, uint64_t count, uint64_t size) {
    /* IMPORTANT: the pre-existing signature test alone matched CUSA00004.
     * On ARM64/FEX this accidentally installed GoW III x86 code stubs inside
     * inFAMOUS Second Son, followed by "NoExec instruction in entry block".
     * Never write GoW III hooks to an inFAMOUS executable. */
    char path[4096], serial[32] = "", version[32] = "";
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
    sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
    sfo_value(path, "APP_VER", version, sizeof(version), NULL);
    if (strcmp(serial, "CUSA01623") || strcmp(version, "01.02")) return;
    if (!mapped(segments, count, TLG_VIEW_LOD_TABLE, TLG_VIEW_LOD_ROWS * TLG_VIEW_LOD_STRIDE) ||
        !mapped(segments, count, TLG_VIEW_BANDS, 4 * TLG_VIEW_BAND_COUNT) ||
        !mapped(segments, count, TLG_VIEW_NEAR_CLIP, 8) ||
        !mapped(segments, count, tlg_view_region_sites[1], sizeof(tlg_view_region_code)) ||
        !tlg_view_signature_match(image, size)) {
        return; /* another game or build: silent, like the other hooks of other games */
    }
    unsigned char *stubs = allocate(page_size);
    const uint32_t *sites[2] = {tlg_view_region_sites, tlg_view_fade_sites};
    for (unsigned kind = 0; kind < 2; ++kind) {
        for (unsigned i = 0; i < 2; ++i) {
            const uint32_t at = sites[kind][i];
            unsigned char *stub = stubs + (kind * 2 + i) * 128;
            const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5);
            if (delta < INT32_MIN || delta > INT32_MAX) {
                printf("Draw distance: unavailable (hook outside relative-jump range)\n");
                return;
            }
            const uintptr_t resume = (uintptr_t)(image + at + TLG_VIEW_SITE_LENGTH);
            if (kind == 0) tlg_view_build_region_stub(stub, tlg_view_region_params(), resume);
            else tlg_view_build_fade_stub(stub, tlg_view_fade_inverse(), resume);
        }
    }
    protect(stubs, page_size, 5);
    guest_cpu_code((uintptr_t)stubs, page_size);
    for (unsigned kind = 0; kind < 2; ++kind)
        for (unsigned i = 0; i < 2; ++i)
            install_jump(sites[kind][i], TLG_VIEW_SITE_LENGTH, stubs + (kind * 2 + i) * 128);
    tlg_view_attach(image);
    printf("Draw distance: ready (LOD scale, LOD bands, region LOD_ADJUST and FADE_DISTANCE, near/far clip)\n");
}

/* G3PATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"G3PATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
void runtime_restart(void) {
    fflush(NULL);
#ifdef _WIN32
    /* run.py sets TLG_RESTART_COMMAND: the launch command line, started again (new patches). */
    const char *command=getenv("TLG_RESTART_COMMAND");
    puts("Runtime: restarting through run.py");
    if (!command || !*command) { fputs("runtime_restart: TLG_RESTART_COMMAND is not set\n",stderr); _exit(1); }
    char *line=_strdup(command);
    /* Same output handles: a launcher reading the game's log keeps reading the new launch. */
    STARTUPINFOA startup={.cb=sizeof(startup),.dwFlags=STARTF_USESTDHANDLES,
                          .hStdInput=GetStdHandle(STD_INPUT_HANDLE),.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE),
                          .hStdError=GetStdHandle(STD_ERROR_HANDLE)};
    PROCESS_INFORMATION process;
    if (!line || !CreateProcessA(NULL,line,NULL,NULL,TRUE,0,NULL,NULL,&startup,&process)) {
        fprintf(stderr,"runtime_restart: CreateProcess failed (%lu)\n",GetLastError()); _exit(1);
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    /* The new launch waits for this process's GPU device to go away (run.py --after). */
    TerminateProcess(GetCurrentProcess(),0);
#else
    /* Android runs this native Linux/glibc process under an app-owned /system/bin/sh
     * supervisor, without bash or run.sh. An in-game restart must go through that
     * original supervisor, keeping stdout/stderr so GameActivity continues to log. */
    const char *android = getenv("ISS_ANDROID_INPUT");
    if (android && android[0]=='1') {
        const char *data = getenv("TLG_DATA_DIR");
        char script[1024];
        if (!data || (size_t)snprintf(script,sizeof(script),"%s/run-iss.sh",data)>=sizeof(script)) {
            fputs("runtime_restart: missing/invalid Android launcher path\\n",stderr);
            _exit(1);
        }
        puts("Runtime: restarting through Android glibc supervisor");
        fflush(NULL);
        execl("/system/bin/sh", "sh", script, (char *)NULL);
        perror("runtime_restart: Android supervisor exec");
        _exit(1);
    }
    puts("Runtime: restarting through run.sh");
    syscall(SYS_close_range, 3u, ~0u, 0u);
    execlp("bash", "bash", "run.sh", (char *)NULL);
    perror("runtime_restart: exec");
    _exit(1);
#endif
}

/* The guest code needs the PS4's instruction sets (#33, #66); TLG_CPU_CHECK=0 skips this. A
 * missing fatal set prints STOP and returns 0; the rewritable ones go to cpu_missing_sets, named
 * in missing. */
static int cpu_supported(char *missing, size_t size) {
    missing[0] = 0;
#ifdef GUEST_CPU_NATIVE
    const char *check = getenv("TLG_CPU_CHECK");
    if (check && !strcmp(check, "0")) return 1;
    const CpuFeatureWords words = cpu_feature_words();
    if (cpu_missing_fatal(words, missing, size)) {
        printf("STOP: this CPU lacks instruction sets the game's code uses: %s "
               "(Intel Sandy Bridge 2011 or newer, or AMD FX or newer, is needed)\n", missing);
        return 0;
    }
    cpu_missing_sets = cpu_missing_rewritable(words, missing, size);
#else
    (void)size;
#endif
    return 1;
}

/* Per-game environment prefix (inFAMOUS: ISS_). The core still reads TLG_* (renamed to a shared
 * prefix once every port uses ps4_core), so each ISS_X is copied to TLG_X at start, unless
 * TLG_X is set too. */
#ifndef PORT_ENV_PREFIX
#define PORT_ENV_PREFIX "ISS_"
#endif
#ifdef _WIN32
#define environ _environ
#else
extern char **environ;
#endif
static void alias_port_environment(void) {
    const size_t prefix = strlen(PORT_ENV_PREFIX);
    size_t count = 0;
    while (environ && environ[count]) ++count;
    char **copy = calloc(count + 1, sizeof(*copy)); /* setenv may reallocate environ */
    if (!copy) return;
    for (size_t i = 0; i < count; ++i) copy[i] = strdup(environ[i]);
    for (size_t i = 0; i < count; ++i) {
        char *entry = copy[i], *equals = entry ? strchr(entry, '=') : NULL;
        if (!equals || strncmp(entry, PORT_ENV_PREFIX, prefix) || equals == entry + prefix) continue;
        char name[256];
        const size_t length = (size_t)(equals - entry - prefix);
        if (length + 5 > sizeof(name)) continue;
        memcpy(name, "TLG_", 4);
        memcpy(name + 4, entry + prefix, length);
        name[4 + length] = 0;
        if (getenv(name)) continue;
#ifdef _WIN32
        _putenv_s(name, equals + 1);
#else
        setenv(name, equals + 1, 0);
#endif
    }
    for (size_t i = 0; i < count; ++i) free(copy[i]);
    free(copy);
}

/* Detailed log mode (ISS_VERBOSE_LOG=1 or 2, copied to TLG_VERBOSE_LOG above): turns on the
 * bounded trace switches that are safe together and asks the GPU log for debug (1) or trace (2).
 * A variable the player already set is never overwritten. Switches left off on purpose, because
 * they change behavior or are heavy single-subsystem debugging: TLG_IMPORT_TRACE (reroutes every
 * import call through a stub), TLG_WRITE_LOG (ring buffer plus scans on every GPU write),
 * TLG_REGION_TRACE (writes a file), TLG_DEBUG_MOTION (draws a debug overlay), TLG_SCENE_DEBUG
 * (dumps frames and deletes the trigger file). */
static void verbose_default(const char *name, const char *value, char *list, size_t size) {
    if (getenv(name)) return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
    size_t used = strlen(list);
    snprintf(list + used, size - used, "%s%s=%s", used ? ", " : "", name, value);
}
static void apply_verbose_log(void) {
    const char *mode = getenv("TLG_VERBOSE_LOG");
    const int level = mode ? atoi(mode) : 0;
    if (level <= 0) return;
    char list[512] = "";
    verbose_default("TLG_GPU_LOG", level >= 2 ? "trace" : "debug", list, sizeof list);
    verbose_default("TLG_AUDIO_TRACE", "1", list, sizeof list);        /* 400 lines: sound opens, Ajm */
    verbose_default("TLG_MOD_TRACE", "1", list, sizeof list);          /* 32 lines, only with TLG_MODS_DIR */
    verbose_default("TLG_OCCLUSION_LOG", "1", list, sizeof list);      /* first 48 events, then 1 in 2000 */
    verbose_default("TLG_MOTION_SELECT_LOG", "1", list, sizeof list);  /* one line per G-buffer shader */
    verbose_default("TLG_PIPELINE_LOG", "1", list, sizeof list);       /* one line per pipeline built */
    if (level >= 2) {
        verbose_default("TLG_TRACE_SEMA", "1", list, sizeof list);     /* capped at 2000 lines by the runtime */
        verbose_default("TLG_JSON_LOG", "1", list, sizeof list);       /* sce::Json serialize results */
        verbose_default("TLG_MOUSE_LOG", "1", list, sizeof list);      /* 4 lines/s while the mouse is used */
        verbose_default("TLG_RECORDER_TRACE", "1", list, sizeof list); /* top callers every 2000 recordings */
    }
    printf("Verbose log: on (level %d); switches: %s\n", level, list[0] ? list : "(all already set by the user)");
}
/* Detailed log mode: boot step with the time since the first one (ms). */
static void boot_step(const char *what) {
    if (!runtime_verbose()) return;
    static struct timespec start;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!start.tv_sec && !start.tv_nsec) start = now;
    printf("Verbose: boot step '%s' at +%ld ms\n", what,
           (long)((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000));
}

#ifndef TLG_BUILD_COMMIT
#define TLG_BUILD_COMMIT "unknown"
#endif
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alias_port_environment();
    apply_verbose_log();
    boot_step("start");
    guest_cpu_init(runtime_low_map);
#ifndef _WIN32
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
#ifdef __ANDROID__
    /* Scudo has no glibc arena/brk tunables. Guest-visible pointers continue
       to use runtime_low_map; disable heap tags as in GoW3 Bionic. */
    mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE);
#else
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
#endif
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
    char missing[128];
    if (argc == 2 && !strcmp(argv[1], "--cpu-features")) {
        if (!cpu_supported(missing, sizeof missing)) return 20;
        if (cpu_missing_sets) printf("CPU: missing %s\n", missing);
        else puts("CPU: complete");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--gamepad")) {
        /* Before the memory reservation: this only asks SDL, nothing of the game is set up yet. */
        runtime_pad_describe();
        return 0;
    }
#ifdef _WIN32
    /* The PS4 user range must stay free of host allocations (GPU driver, heaps), and the guest
     * TCB needs one of the 64 TEB TLS slots before DLLs (Vulkan drivers) take them. */
    runtime_memory_reserve();
    runtime_thread_tcb_offset();
#endif
    int cpu_only = 0, strict_imports = 0, check_imports = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL, *isa_file=NULL;
    const char *mounts[8]; int mount_count=0;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--check-imports")) check_imports = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--isa-rewrite") && i+1<argc) isa_file=argv[++i];
        else if (!strcmp(argv[i], "--mount") && i+1<argc) {
            /* guest=host; the guest part never has '=', so the first one splits */
            const char *spec=argv[++i], *eq=strchr(spec,'=');
            if (mount_count==8 || !eq || eq==spec || !eq[1]) { fprintf(stderr, "Bad --mount (guest=host, at most 8): %s\n", spec); return 1; }
            mounts[mount_count++]=spec;
        }
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--check-imports] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--isa-rewrite file] [--mount guest=host]... [--timeout seconds] | --vulkan-only | --cpu-features | --gamepad\n", argv[0]);
        return 1;
    }
    {
        /* The first log line says which build and which game this is (players often can't tell
         * which zip they ran, or which dump: only CUSA00223 1.07 is supported). */
        char path[4096], serial[32] = "?", version[32] = "?";
        snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
        sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
        sfo_value(path, "APP_VER", version, sizeof(version), NULL);
        printf("Build: %s (built %s %s); game %s %s; threads inherit the creator's MXCSR/x87 control\n",
               TLG_BUILD_COMMIT, __DATE__, __TIME__, serial, version);
    }
    if (!cpu_supported(missing, sizeof missing)) return 20;
    if (cpu_missing_sets) printf("CPU: missing %s\n", missing);
    boot_step("cpu check done");
    /* TLG_ISA_REWRITE=0 never rewrites, =1 rewrites every emulated site (MOVBE, BMI1, LZCNT, F16C, SSE4A:
     * a test on a complete CPU), a list such as "sse4a", "f16c" or "movbe,bmi1" rewrites those sets; by
     * default the sets the CPU lacks. */
#ifdef GUEST_CPU_NATIVE
    const char *policy = getenv("TLG_ISA_REWRITE");
    int policy_valid;
    const unsigned isa_forced = cpu_sets_named(policy, &policy_valid);
    if (!policy_valid) printf("ISA: TLG_ISA_REWRITE=%s ignored (use 0, 1 or a list of movbe, bmi1, lzcnt, f16c, sse4a)\n", policy);
    const int forced = isa_forced != 0;
    unsigned isa_sets = forced ? isa_forced | cpu_missing_sets : policy && !strcmp(policy, "0") ? 0 : cpu_missing_sets;
    if (cpu_missing_sets && !isa_sets) {
        printf("STOP: this CPU lacks %s and the rewrite is disabled (TLG_ISA_REWRITE=0)\n", missing);
        return 20;
    }
    if (cpu_missing_sets && !isa_file) {
        printf("STOP: this CPU lacks %s and no rewrite table matches the image\n", missing);
        return 20;
    }
    /* Red-zone protection (scripts/red_zone.py, the table's G3RZONE1 section): guest code keeps data
     * below rsp (SysV red zone) and Windows' exception dispatch overwrites it on every fault (#29,
     * #41). On by default on Windows; TLG_RED_ZONE_REWRITE=1 forces it (Linux tests, with
     * TLG_CLOBBER_RED_ZONE), =0 turns it off. */
    const char *red_zone_policy = getenv("TLG_RED_ZONE_REWRITE");
#ifdef _WIN32
    int red_zone_on = !(red_zone_policy && !strcmp(red_zone_policy, "0"));
#else
    int red_zone_on = red_zone_policy && !strcmp(red_zone_policy, "1");
#endif
#else
    unsigned isa_sets=0;
    int red_zone_on=0;
    (void)isa_file;
#endif
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"G3CONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        for (int i = 0; i < mount_count; ++i) {
            const char *eq = strchr(mounts[i], '=');
            char guest[64];
            size_t glen = (size_t)(eq - mounts[i]);
            if (glen >= sizeof(guest)) { fprintf(stderr, "Cannot mount %s: guest path longer than 63 characters\n", mounts[i]); return 1; }
            memcpy(guest, mounts[i], glen); guest[glen] = 0;
            if (runtime_file_mount(guest, eq + 1)) { fprintf(stderr, "Cannot mount %s: too many mounts or host path longer than 511 characters\n", mounts[i]); return 1; }
            printf("Runtime: mount %s -> %s\n", guest, eq + 1);
        }
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
    }
    boot_step("content mounted");
    tlggpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    AddVectoredExceptionHandler(1, vectored_fault);
    SetUnhandledExceptionFilter(unhandled_fault);
    if (getenv("TLG_STRICT_HANDLES")) {
        PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strict={0};
        strict.RaiseExceptionOnInvalidHandleReference=1;
        strict.HandleExceptionsPermanentlyEnabled=1;
        if (!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy,&strict,sizeof(strict)))
            fprintf(stderr,"TLG_STRICT_HANDLES: policy not set (%lu)\n",GetLastError());
    }
    (void)timeout_seconds; /* no watchdog on Windows */
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    {
        const char *clobber = getenv("TLG_CLOBBER_RED_ZONE");
        long bytes = clobber ? strtol(clobber, NULL, 0) : 0;
        clobber_red_zone = bytes == 1 ? 128 : bytes > 0 && bytes <= 65536 ? (unsigned)bytes : 0;
        if (clobber_red_zone) {
            printf("Test: fault handler zeroes %u bytes below the guest rsp (TLG_CLOBBER_RED_ZONE)\n", clobber_red_zone);
            runtime_set_thread_start_hook(clobber_thread_init);
        }
    }
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO | (clobber_red_zone > 128 ? SA_ONSTACK : 0);
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL); /* int3 ISA sites */
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction request = {0}; request.sa_sigaction = dump_request; request.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&request.sa_mask); sigaction(SIGUSR1, &request, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
#endif
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "G3PROBE1", 8) && memcmp(magic, "G3PROBE2", 8) && memcmp(magic,"G3PROBE3",8) && memcmp(magic,"G3PROBE4",8) && memcmp(magic,"G3PROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "G3PROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    boot_step("boot file read, starting runtime");
    runtime_start(strict_imports ? 0 : capabilities);
    boot_step("runtime started");
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"G3PROBE5",8);
    int linked=!memcmp(magic,"G3PROBE3",8) || !memcmp(magic,"G3PROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* G3PROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"G3PROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
#ifdef GUEST_CPU_NATIVE
    isa_table = (isa_sets || red_zone_on) && isa_file ? isa_table_load(isa_file) : NULL;
    if (!isa_table && cpu_missing_sets) { printf("STOP: this CPU lacks %s and no rewrite table matches the image\n", missing); return 20; }
    /* The stubs follow the image in the same low mapping (rel32 jmps reach them): the emulation
     * stubs, then the red-zone stub code at the offset scripts/red_zone.py encoded it for. */
    size_t stub_area = isa_table ? round_page(isa_stub_area_size(isa_table)) : 0;
    size_t red_zone_area = 0;
    if (red_zone_on) {
        if (!isa_table || !isa_table->rz) printf("Red zone: protection unavailable (no red-zone section in the rewrite table)\n");
        else if (isa_table->rz_code_offset != round_page(size) + stub_area) printf("Red zone: protection unavailable (stub offset does not match this page size)\n");
        else red_zone_area = round_page(isa_red_zone_code_size(isa_table));
        if (!red_zone_area) red_zone_on = 0;
    }
    image = allocate(round_page(size) + stub_area + red_zone_area);
    unsigned char *stubs = image + round_page(size);
#ifndef _WIN32
    guest_code_end = (uintptr_t)image + round_page(size) + stub_area + red_zone_area;
#endif
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (isa_table && isa_table_matches(isa_table, image, size)) {
        if (cpu_missing_sets) {
            printf("STOP: this CPU lacks %s and no rewrite table matches the image\n", missing);
            return 20;
        }
        printf("Red zone: protection unavailable (the rewrite table is for another image)\n");
        isa_table = NULL;
        red_zone_on = 0;
    }
#else
    size_t stub_area=0, red_zone_area=0;
    (void)isa_sets; (void)red_zone_on;
    image=allocate(round_page(size));
    if (fread(image,1,size,f)!=size || fgetc(f)!=EOF) fail("incorrect memory image size");
    fclose(f);
#endif
    if (!cpu_only) {
        char title[128]="inFAMOUS Second Son", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        TlgGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        boot_step("initializing GPU");
        if (tlggpu_init(&gpu)) fail("GPU initialization failed");
        boot_step("GPU ready");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,tlggpu_symbol_count());
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    guest_cpu_code((uintptr_t)image,round_page(size)+stub_area+red_zone_area);
    guest_cpu_code((uintptr_t)traps,round_page((import_count+1)*32));
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        uintptr_t handler = guest_cpu_host_function((void *)unresolved);
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    const char *import_trace = getenv("TLG_IMPORT_TRACE");
    if (import_trace && *import_trace) {
        trace_limit = atoi(import_trace) > 0 ? (uint32_t)atoi(import_trace) : 50;
        trace_stubs = allocate(round_page((import_count + 1) * TRACE_STUB_SIZE));
        trace_made = calloc(import_count + 1, sizeof(*trace_made));
        trace_counts = calloc(import_count + 1, sizeof(*trace_counts));
        if (!trace_made || !trace_counts) fail("allocation failed");
        guest_cpu_code((uintptr_t)trace_stubs,round_page((import_count+1)*TRACE_STUB_SIZE));
    }
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            if (resolved) {
                value = relocs[i].kind==1 ? guest_cpu_host_function((void *)resolved) : resolved + relocs[i].addend;
                if (trace_stubs && relocs[i].kind == 1 && !relocs[i].addend) value = trace_stub(relocs[i].value, guest_cpu_host_function((void *)resolved));
            } else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
            }
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    if (check_imports) {
        /* Lists every import the game would stop at, without running it (needs the GPU
         * library: without --cpu-only its symbols are registered above). */
        unsigned char *checked=calloc(import_count ? import_count : 1,1);
        if (!checked) fail("allocation failed");
        uint64_t missing=0;
        for (uint64_t i=0;i<nr;++i) {
            uint64_t index=relocs[i].value;
            if (!relocs[i].kind || checked[index]) continue;
            checked[index]=1;
            if (runtime_resolve(names[index],relocs[i].kind==2) || (native_libc && bindings[index])) continue;
            printf("MISSING: %s (%s)\n",names[index],runtime_import_name(names[index]));
            ++missing;
        }
        printf("Imports: %" PRIu64 " missing\n",missing);
        fflush(NULL);
        _exit(missing ? 1 : 0); /* no destructors: GPU threads are running */
    }
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    install_infamous_menu_watch(app0,segments,ns,size);
    if (gpu_enabled) install_iss_camera_capture(app0, segments, ns, size);
    if (gpu_enabled) install_iss_fsr3_native_aa(app0, segments, ns, size);
    if (gpu_enabled) install_orb_multiplier(app0, segments, ns, size);
    if (gpu_enabled) install_cheats(app0, segments, ns, size);
    if (gpu_enabled) install_actor_watch(app0, segments, ns, size);
    if (gpu_enabled) install_stat_multipliers(app0, segments, ns, size);
    if (gpu_enabled) install_view_tuning(app0, segments, ns, size);
#ifdef _WIN32
    printf("Guest TCB loads redirected to the TEB TLS slot: %" PRIu64 "\n", patch_tcb_loads(segments, ns));
#endif
#ifdef GUEST_CPU_NATIVE
    if (isa_table) {
        isa_table->protect_red_zone = red_zone_on;
        IsaReport r = isa_rewrite_apply(isa_table, image, stubs, isa_sets);
        if (r.unsupported_needed) {
            printf("STOP: this CPU lacks %s and %u sites use encodings the rewrite cannot emulate\n", missing, r.unsupported_needed);
            fflush(NULL);
            _exit(20); /* no destructors: GPU threads are running */
        }
        if (forced) printf("ISA: forced rewrite of %u sites (TLG_ISA_REWRITE=%s; %u jmp, %u relocated, %u int3), %u skipped\n",
                           r.rewritten, policy, r.jmp, r.relocated, r.int3, r.skipped);
        else if (isa_sets) printf("ISA: CPU lacks %s: %u sites rewritten (%u jmp, %u relocated, %u int3), %u skipped\n",
                                  missing, r.rewritten, r.jmp, r.relocated, r.int3, r.skipped);
        if (red_zone_on) {
            const uint32_t *st = isa_table->rz_stats;   /* scripts/red_zone.py STATS_FIELDS */
            IsaRedZoneReport z = isa_red_zone_apply(isa_table, image, stubs + stub_area);
            printf("Red zone: %u instructions protected in %u patches (%u short-jump relays), %u patches skipped; "
                   "%u red-zone functions of %u (%u conservative); not protected: %u use rsp, %u control flow, "
                   "%u unrelocatable\n",
                   z.protected_instructions, z.groups, z.short_relays, z.skipped, st[1], st[0], st[2], st[7], st[8],
                   st[10]);
            if (r.int3) printf("Red zone: %u int3 ISA sites trap through the exception handler (not protected)\n", r.int3);
        }
        runtime_set_report_hook(isa_report_hook);
        protect(stubs, stub_area + red_zone_area, 5);
    }
#endif
    protect(traps, round_page((import_count + 1) * 32), 5);
    if (trace_stubs) protect(trace_stubs, round_page((import_count + 1) * TRACE_STUB_SIZE), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    boot_step("image mapped and relocated");
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            int result=(int)guest_cpu_call((uintptr_t)(image+modules[m].init),3,(const uint64_t[]){0,0,0});
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    {   /* TLG_FREEZE_REPORT_SECONDS (default 10, 0 = off): no new frame for that long writes
         * every thread's wait, RIP and frames to the log (runtime_freeze.c). Needs the GPU. */
        const char *freeze = getenv("TLG_FREEZE_REPORT_SECONDS");
        unsigned seconds = freeze && *freeze ? (unsigned)strtoul(freeze, NULL, 10) : 10;
        runtime_freeze_start((uintptr_t)image, round_page(size) + stub_area + red_zone_area, gpu_enabled ? seconds : 0);
    }
    boot_step("entering the game");
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
#ifdef _WIN32
    /* The guest main thread starts with the PS4's (FreeBSD's) x87 control word: 64-bit
     * precision (0x037F, as on Linux). Windows' default is 53-bit (0x027F). Threads the game
     * creates copy the creator's x87 and SSE control words (runtime_thread.c host_start). */
    { const uint16_t cw = 0x037F; __asm__ volatile("fldcw %0" : : "m"(cw)); }
#endif
    entered_game=1;
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
#ifdef _WIN32
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
#else
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
#ifdef GUEST_CPU_NATIVE
    clobber_thread_init();
    enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-64);
#else
    guest_cpu_thread_stack(stack+MAIN_STACK-64,MAIN_STACK-64);
    guest_cpu_call((uintptr_t)(image+entry),2,(const uint64_t[]){(uintptr_t)&params,guest_cpu_host_function((void *)guest_exit)});
#endif
#endif
    fail("entry unexpectedly returned");
}
