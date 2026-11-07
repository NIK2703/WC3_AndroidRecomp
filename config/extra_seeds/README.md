# Explicit address-taken seeds

`entry.exe` and `thunkseeds.exe` prove function starts from executable
references, PE metadata, and a narrow class of unaligned address-taken tails.
Some real functions are reached only through a data pointer and are not covered
by those rules.  Each file in this directory adds that missing evidence for one
module, without widening the global discovery heuristics.

Format: one hexadecimal guest VA per line.  Everything after the address is a
reason and is ignored.  `#` starts a comment.

`closure_pipeline.exe --extra-seeds <file>` passes the addresses to
`seed.exe --add`, so the module report records `extra_seed_entries` and
`extra_seeds_file`.  `corpusclosure.exe` automatically looks for
`config/extra_seeds/<module stem>.txt` for every module, so the canonical
multi-module closure run uses these files without a separate command-line
argument.

Current files:

* `Mss32_a35.txt` - mixer service callbacks installed through Mss32.
* `Mp3dec_a35.txt` - the CRT initializer reached from Mp3dec.asi DllMain
  through a `.data` pointer.
