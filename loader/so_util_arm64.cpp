#if defined(__aarch64__)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "so_util.h"
#include "arm64_encodings.h"
#include "logging.h"
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

extern uintptr_t so_alloc_arena(so_module *so, uintptr_t range, uintptr_t dst, size_t sz);

void hook_address(so_module *mod, uintptr_t addr, uintptr_t dst) {
  if (addr == 0)
    return;

  // Allocate and populate trampoline
  uint32_t trampoline[4];
  uintptr_t trampoline_addr = so_alloc_arena(mod, B_RANGE, B_OFFSET(addr), sizeof(trampoline));
  if (trampoline_addr) {
    // Safer 1 dword, two step trampoline strategy
    // guest -> patch arena -> host

    // step 1:
    // guest -> patch arena <...> 
    uint32_t hook[1];
    hook[0] = B(addr, trampoline_addr);
    memcpy((void *)addr, hook, sizeof(hook));

    // step 2:
    // patch arena -> host
    trampoline[0] = LDR_LIT_QWORD(X17, 0x8);
    trampoline[1] = BR(R17);
    *(uint64_t *)(trampoline + 2) = dst;
    memcpy((void *)trampoline_addr, (void *)trampoline, sizeof(trampoline));
  } else {
    // Unsafe 4 dword (!!!) backup strategy - clobbers instructions of stubbed
    // functions, and has caused crashes in the past.
    // guest -> host
    trampoline[0] = LDR_LIT_QWORD(X17, 0x8);
    trampoline[1] = BR(R17);
    *(uint64_t *)(trampoline + 2) = dst;
    memcpy((void *)addr, (void *)trampoline, sizeof(trampoline));
  }
}

// hook_address_detour: the entry redirect clobbers one 4-byte instruction; to
// reach the original we relocate just that instruction into a trampoline and
// resume at addr+4. Hence the single-instruction relocator below.

// single-instruction decoders (enough to relocate a function entry)
static inline int bd_is_adr (uint32_t i) { return (i & 0x9F000000u) == 0x10000000u; }
static inline int bd_is_adrp(uint32_t i) { return (i & 0x9F000000u) == 0x90000000u; }
static inline int bd_is_b   (uint32_t i) { return (i & 0xFC000000u) == 0x14000000u; }
static inline int bd_is_pcrel(uint32_t i) {
  return bd_is_adr(i) || bd_is_adrp(i) || bd_is_b(i)
      || ((i & 0xFC000000u) == 0x94000000u)   // BL
      || ((i & 0xFF000010u) == 0x54000000u)   // B.cond
      || ((i & 0x7E000000u) == 0x34000000u)   // CBZ/CBNZ
      || ((i & 0x7E000000u) == 0x36000000u)   // TBZ/TBNZ
      || ((i & 0x3B000000u) == 0x18000000u);  // LDR/LDRSW/PRFM (literal)
}
static inline int64_t bd_sx(uint64_t v, int bits) {
  uint64_t m = 1ull << (bits - 1);
  return (int64_t)((v ^ m) - m);
}
static uintptr_t bd_adr_target(uint32_t i, uintptr_t pc) {
  uint32_t immlo = (i >> 29) & 3u, immhi = (i >> 5) & 0x7FFFFu;
  int64_t imm = bd_sx(((uint64_t)immhi << 2) | immlo, 21);
  if (bd_is_adrp(i)) return (pc & ~(uintptr_t)0xFFF) + (uintptr_t)(imm << 12);
  return pc + (uintptr_t)imm; // ADR
}
static uintptr_t bd_b_target(uint32_t i, uintptr_t pc) {
  return pc + (uintptr_t)(bd_sx(i & 0x03FFFFFFu, 26) << 2);
}

// Emit a "run original then resume at addr+4" trampoline body into out[] (<=8
// words). Returns word count, or 0 if the first instruction is unrelocatable.
static int bd_emit_orig(uint32_t ins, uintptr_t addr, uintptr_t tpc, uint32_t *o) {
  if (bd_is_b(ins)) {                       // entry is a tail branch: jump to its target
    uintptr_t t = bd_b_target(ins, addr);
    o[0] = LDR_LIT_QWORD(X17, 0x8); o[1] = BR(R17);
    o[2] = (uint32_t)t; o[3] = (uint32_t)((uint64_t)t >> 32);
    return 4;
  }
  if (bd_is_adr(ins) || bd_is_adrp(ins)) {  // PC-rel address into Rd: materialize abs
    uint32_t rd = ins & 0x1Fu;
    uintptr_t t = bd_adr_target(ins, addr);
    o[0] = LDR_LIT_QWORD(rd, 0x8);          // Rd = [tpc+8] = .quad t
    o[1] = B(tpc + 4, tpc + 16);            // skip the literal -> o[4]
    o[2] = (uint32_t)t; o[3] = (uint32_t)((uint64_t)t >> 32);
    o[4] = LDR_LIT_QWORD(X17, 0x8); o[5] = BR(R17);   // resume at addr+4
    o[6] = (uint32_t)(addr + 4); o[7] = (uint32_t)((uint64_t)(addr + 4) >> 32);
    return 8;
  }
  if (!bd_is_pcrel(ins)) {                   // non-PC-relative: copy verbatim
    o[0] = ins;
    o[1] = LDR_LIT_QWORD(X17, 0x8); o[2] = BR(R17);
    o[3] = (uint32_t)(addr + 4); o[4] = (uint32_t)((uint64_t)(addr + 4) >> 32);
    return 5;
  }
  return 0; // BL / B.cond / CBZ / TBZ / LDR-literal as entry: unsupported (rare)
}

// Mirror of so_util.cpp's (static) so_flush_caches, using public so_module fields.
static void bd_arena_protect(so_module *mod, int write) {
  __builtin___clear_cache((void *)mod->patch_base, (void *)mod->cave_head);
  mprotect((void *)mod->patch_base, mod->cave_head - mod->patch_base,
           PROT_EXEC | (write ? PROT_WRITE | PROT_READ : PROT_READ));
}

// Re-protect the page(s) covering [a, a+len) in the loaded .so text.
static void bd_text_protect(uintptr_t a, size_t len, int prot) {
  long ps = sysconf(_SC_PAGESIZE);
  if (ps <= 0) ps = 0x1000;
  uintptr_t s = a & ~((uintptr_t)ps - 1);
  uintptr_t e = (a + len + (uintptr_t)ps - 1) & ~((uintptr_t)ps - 1);
  mprotect((void *)s, e - s, prot);
}

void hook_address_detour(so_module *mod, uintptr_t addr, uintptr_t dst, uintptr_t *orig_out) {
  if (orig_out) *orig_out = 0;
  if (addr == 0 || mod == NULL) return;

  uint32_t ins = *(volatile uint32_t *)addr;

  // Arena writable while we build the trampoline (and while hook_address writes
  // its own 2-step trampoline below).
  bd_arena_protect(mod, 1);

  uintptr_t tramp = so_alloc_arena(mod, 0, addr, 8 * sizeof(uint32_t));
  if (tramp == 0) {
    BD_LOG("HOOK", "detour: arena full, cannot hook %p", (void *)addr);
    bd_arena_protect(mod, 0);
    return;
  }

  uint32_t body[8];
  int n = bd_emit_orig(ins, addr, tramp, body);
  if (n == 0) {
    BD_LOG("HOOK", "detour: unrelocatable entry insn 0x%08x at %p, leaving original intact",
           ins, (void *)addr);
    bd_arena_protect(mod, 0);
    return; // no redirect installed
  }
  memcpy((void *)tramp, body, n * sizeof(uint32_t));

  // Install the entry redirect addr -> dst. hook_address writes to .so text and
  // to the arena, so make the text page writable for the duration.
  bd_text_protect(addr, 4, PROT_READ | PROT_WRITE | PROT_EXEC);
  hook_address(mod, addr, dst);
  bd_text_protect(addr, 4, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((void *)addr, (void *)(addr + 4));

  // Arena back to RX (also flushes i-cache over the new trampolines).
  bd_arena_protect(mod, 0);

  if (orig_out) *orig_out = tramp;
  BD_LOG("HOOK", "detour installed at %p -> %p (orig tramp %p)",
         (void *)addr, (void *)dst, (void *)tramp);
}
#endif
