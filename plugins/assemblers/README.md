# Assemblers

Four assemblers that turn the active document into a program you can run in
an emulator. Each is on the **Tools** menu:

| Menu | CPU | Saves | Default `ORG` |
|---|---|---|---|
| **Z80 Assembler → Compile** | Z80 | ZX Spectrum `.tap` with an auto-running BASIC loader | `$8000` |
| **Z80 Assembler → Compile and Run** | Z80 | Saves the `.tap`, then opens it with its Windows-associated application | `$8000` |
| **Z80 Assembler → Configure Emulator...** | Z80 | Selects an emulator and optional arguments for Compile and Run | — |
| **CHIP-8 Assembler → Compile** | CHIP-8 and SUPER-CHIP 1.1 | `.ch8` program | `$200` |
| **6502 Assembler → Compile C64 PRG** | MOS 6502 | Commodore 64 `.prg` | `$C000` |
| **6502 Assembler → Compile Raw Binary** | MOS 6502 | `.bin` with no header, for other 6502 machines | `$C000` |
| **Game Boy Assembler → Compile ROM** | Sharp SM83 (Game Boy) | 32 KB `.gb` cartridge ROM with its header filled in | `$0150` |

If the source has an error, a message gives its line and the line is
selected. When the file is saved, a message reports its size, origin, entry
point, end address and number of symbols.

## Source syntax

All four assemblers share one front end (`core.lua`), so everything in this
section applies to each of them.

One instruction or directive per line. `;` starts a comment. A label is a
name followed by `:`, at the start of a line or alone on it. Mnemonics,
register names and symbols are case-insensitive.

```asm
; CHIP-8: draw a sprite and wait
start:  LD I,sprite
        DRW V0,V1,5
.wait:  JP .wait
sprite: DB $F0,$90,$90,$90,$F0
```

A label beginning with `.` is local to the most recent label without one, so
each routine can have its own `.loop`.

### Expressions

- Numbers: decimal, `$FF` or `0xFF` (hex), `%1010` (binary), and character
  literals such as `'A'` and `'\n'`.
- `$` on its own is the address of the current line.
- Unary `+ - ~`; binary `* / %`, `+ -`, `<< >>`, `&`, `^`, `|`; and the
  comparisons `= == != <> < <= > >=`, which give 1 or 0 (for `IF` and
  `ASSERT`).
- `EQU` constants may refer to constants and labels defined later; circular
  definitions are reported.

### Directives

| Directive | Meaning |
|---|---|
| `ORG address` | Where the following code goes. It can move forward but not back; the gap is filled with zeros. |
| `name EQU value` | A constant. |
| `DB`/`DEFB`/`DEFM values` | Bytes; strings in quotes give one byte per character. |
| `DW`/`DEFW values` | 16-bit words: low byte first on the Z80 and 6502, high byte first on CHIP-8. |
| `DS`/`DEFS`/`FILL count[,value]` | `count` bytes of `value` (default 0). |
| `ALIGN boundary[,value]` | Pads to the next multiple of `boundary`. |
| `IF expression` … `ELSE` … `ENDIF` | Assembles one branch. The expression must be known before layout. |
| `ASSERT expression[,"message"]` | Stops with the message when the expression is 0. |
| `ENTRY address` | Where the program starts, if not at the first byte. |
| `END [address]` | Stops assembling; an address sets the entry point. |
| `INCLUDE "path"` | Splices another file's source in at that point. The document being assembled must be saved: `path` is resolved relative to its folder (`..` can reach a sibling folder, but `path` itself must not be absolute), and included files can nest up to 32 deep. |

None of the four has macros, `INCBIN`, or a repeat/loop directive (`REPT`,
`.rept`); a source file plus whatever it `INCLUDE`s is assembled as one unit,
top to bottom. `INCLUDE` is spliced in before `IF`/`ELSE`/`ENDIF` are
evaluated, so a conditionally-excluded `INCLUDE` line still reads the file -
what it spliced in is then excluded like any other code. An error inside an
included file is reported as `name:line: message` instead of `Line N:`, and
isn't highlighted in the editor (only the document actually open there can
be).

### What each CPU supports, and what it doesn't

Each CPU's full instruction table, menu commands and save format are on its
own page:

| CPU | Supports | Does not support |
|---|---|---|
| [Z80](z80.md) | The full documented instruction set, plus the common undocumented extras: `SLL`/`SL1`, `IXH`/`IXL`/`IYH`/`IYL`, `IN (C)`/`IN F,(C)`, and indexed `CB` operations with a register destination (`RLC (IX+d),B` and the like). | Nothing from the documented or commonly-used undocumented set; this is the one assembler here with no deliberate gaps. |
| [CHIP-8 / SUPER-CHIP](chip8.md) | Every original CHIP-8 opcode and SUPER-CHIP 1.1's additions (scrolling, the 128×64 mode, the large font, RPL flag save/restore). | XO-CHIP (Octo) extensions: no 16-bit `i := long`, extra bitplane/scroll-up opcodes, audio pattern buffer, or `pitch`/`buffer` registers. Sticking to CHIP-8/SUPER-CHIP mnemonics keeps a program portable to interpreters that don't implement XO-CHIP either. |
| [6502](mos6502.md) | The 56 documented NMOS instructions in every addressing mode they come in. | The NMOS "illegal"/undocumented opcodes (`LAX`, `SAX`, `DCP`, `SLO`, `RLA`, `SRE`, `RRA`, `ISC`/`ISB`, `ANC`, `ALR`, `ARR`, `SBX`, …) and every 65C02/65816 addition (`PHX`/`PHY`/`PLX`/`PLY`, `STZ`, `TRB`/`TSB`, `BRA`, `(zp)` without an index, `RMB`/`SMB`/`BBR`/`BBS`). Code written for a 65C02 needs rewriting to NMOS 6502 equivalents first. |
| [Game Boy (SM83)](sm83.md) | The Z80 subset the hardware actually has, plus the SM83-only forms its page lists. | `IX`/`IY` and everything built on them, `EX`, `EXX`, `DJNZ`, `IN`/`OUT` and the I/O port instructions, the Z80 block instructions (`LDIR`/`LDDR`/`CPIR`/`CPDR`/…; `LDI`/`LDD` are repurposed instead), `IM`, `NEG`, `RETN`, and the `PO`/`PE`/`P`/`M` conditions. Each is reported as an error rather than silently misencoded. |

## Examples

`examples/` holds a runnable "Hello, World!" for each assembler. Open one in
Tinker Editor and use that CPU's **Compile** command:

| File | CPU | Run it with |
|---|---|---|
| `examples/z80-hello.asm` | [ZX Spectrum](z80.md) | The saved `.tap`, with `LOAD ""`. |
| `examples/chip8-hello.asm` | [CHIP-8 / SUPER-CHIP](chip8.md) | The saved `.ch8` in any interpreter. |
| `examples/c64-hello.asm` | [6502 (Commodore 64)](mos6502.md) | The saved `.prg`, with `SYS 49152`. |
| `examples/gb-hello.asm` | [Game Boy](sm83.md) | The saved `.gb` in an emulator such as SameBoy, BGB or mGBA. |

The `examples` folder has no `init.lua`, so Tinker Editor doesn't load it as a
plugin.

## Files

| File | Contents |
|---|---|
| `init.lua` | The menu commands and output formats |
| `core.lua` | The shared front end and two-pass layout |
| `z80.lua`, `chip8.lua`, `mos6502.lua`, `sm83.lua` | Instruction encoding for each CPU, documented in [z80.md](z80.md), [chip8.md](chip8.md), [mos6502.md](mos6502.md) and [sm83.md](sm83.md) |
| `tap.lua` | The ZX Spectrum TAP format |
| `gbrom.lua` | The Game Boy cartridge image and header |
| `examples/` | A hello-world program for each assembler |

A new CPU is a module returning `{ name, default_origin, memory_size,
big_endian, encode }` (described at the top of `core.lua`) and a menu command
in `init.lua`. The tests are in `tests/test_assemblers.lua`.
