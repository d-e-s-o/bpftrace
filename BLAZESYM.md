```
cmake -DCMAKE_BUILD_TYPE=Release -DSTATIC_LINKING=0 ../
```

bpftrace.{h,cpp}:
- `symbol_table_cache_`:
  - mapping from file path (to ELF file) to `map<Addr, ElfSymbol>`
  - used when `user_symbol_cache_type` is `per_program`
  - the `map<Addr, ElfSymbol>` construct is populated by iterating over
    all symbols in the file (woah!?)
- `pid_sym_`:
  - map<Pid, SymCache>
- `exe_sym_`:
  - mapping from path (to ELF file) to (Pid, SymCache)


Disclaimers:
- bcc seems to support perf maps in their `ProcSyms` thingy
  - what the fuck are they?
