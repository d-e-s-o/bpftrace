Relevant headers:
#include <bcc/bcc_elf.h>
#include <bcc/bcc_syms.h>

bpftrace.{h,cpp}:
- `symbol_table_cache_`:
  - mapping from file path (of ELF file) to `map<Addr, ElfSymbol>`
  - this is effectively a pre-loaded ELF symbol table for each traced
    file; it is populated when a probe is attached
  - it is consulted when `user_symbol_cache_type` (type: `UserSymbolCacheType`) is
    `per_program`
    - apparently this `user_symbol_cache_type` thing is relevant for
      ASLR, where one type is buggy without it but smaller in footprint
    - c.f. `BPFTRACE_CACHE_USER_SYMBOLS` @
           https://docs.bpftrace.org/latest &
           https://manpages.ubuntu.com/manpages/kinetic/man8/bpftrace.8.html
  - the `map<Addr, ElfSymbol>` construct is populated by iterating over
    all symbols in the file (woah!?)
- `exe_sym_`:
  - mapping from path (to ELF file) to (Pid, SymCache)
  - also only used when `user_symbol_cache_type` is `per_program`
- `pid_sym_`:
  - map<Pid, SymCache>
  - consulted when `user_symbol_cache_type` is `per_pid`

- symbolization is done via BCC's symcache:
  - `bcc_symcache_new`
  - for user space symbols that is just a `ProcSyms`
  - upon creation it "loads" all modules:
    https://github.com/iovisor/bcc/blob/ba73657cb8c4dab83dfb89eed4a8b3866255569a/src/cc/bcc_syms.cc#L140
    - this means that for each file backed mapping
      (https://github.com/iovisor/bcc/blob/ba73657cb8c4dab83dfb89eed4a8b3866255569a/src/cc/bcc_proc.c#L75)
      it registers the address range and file offset in use

get_symbol_table_for_elf()
get_func_symbols_from_file():
  - retrieves symbols from an ELF file, taking into account debug symbols

VENDOR_GTEST shows how to vendor sources from GitHub

run with BPFTRACE_STACK_MODE=perf to change the "StackType". That maps
to "show_module" in the user space symbolization code.

Build the thing:
NEW (nix):
nix develop .#bpftrace-llvm18
cmake -B build-nix -GNinja
cd build-nix
ninja

OLD:
- prerequisites:
  - on Fedora: bcc-devel cereal-devel llvm-devel gtest-devel gmock-devel
    maybe but probably not: lldb-devel
    (see
    https://github.com/bpftrace/bpftrace/blob/master/docker/Dockerfile.fedora
    for full list)

rm -rf build/
mkdir build/
cd build/
cmake -DCMAKE_BUILD_TYPE=Release -DSTATIC_LINKING=0 -DUSE_BLAZESYM=1 ../
make VERBOSE=1 -j
