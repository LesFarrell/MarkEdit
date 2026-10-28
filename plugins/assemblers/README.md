# Assemblers

Four assemblers that turn the active document into a program you can run in
an emulator. Each is on the **Tools** menu:

| Menu | CPU | Saves | Default `ORG` |
|---|---|---|---|
| **Z80 Assembler → Compile** | Z80 | ZX Spectrum `.tap` with an auto-running BASIC loader | `$8000` |
| **Z80 Assembler → Compile and Run** | Z80 | Saves the `.tap`, then opens it with its Windows-associated application | `$8000` |
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

## Z80

The documented Z80 instruction set, plus `SLL`/`SL1`, `IXH`, `IXL`, `IYH`,
`IYL`, `IN (C)`, `IN F,(C)` and indexed CB operations with a register
destination.

**Compile** saves a TAP: a tokenized BASIC loader followed by the code. For
code above Spectrum BASIC memory the loader lowers RAMTOP with `CLEAR`, then
loads the code and calls the entry point (`ENTRY`/`END`, otherwise the first
byte). **Compile and Run** saves the same TAP and asks Windows to open it with
the application associated with `.tap` files, such as a Spectrum emulator.

## CHIP-8 and SUPER-CHIP

Mnemonics follow Cowgod's widely used reference. Every instruction is two
bytes. Registers are `V0`–`VF`; addresses are 12-bit (`$000`–`$FFF`).

| Instruction | Opcode | Instruction | Opcode |
|---|---|---|---|
| `CLS` | `00E0` | `RET` | `00EE` |
| `SYS addr` | `0nnn` | `JP addr` | `1nnn` |
| `JP V0,addr` | `Bnnn` | `CALL addr` | `2nnn` |
| `SE Vx,byte` | `3xkk` | `SE Vx,Vy` | `5xy0` |
| `SNE Vx,byte` | `4xkk` | `SNE Vx,Vy` | `9xy0` |
| `LD Vx,byte` | `6xkk` | `LD Vx,Vy` | `8xy0` |
| `ADD Vx,byte` | `7xkk` | `ADD Vx,Vy` | `8xy4` |
| `OR` / `AND` / `XOR Vx,Vy` | `8xy1` / `8xy2` / `8xy3` | `SUB` / `SUBN Vx,Vy` | `8xy5` / `8xy7` |
| `SHR Vx[,Vy]` | `8xy6` | `SHL Vx[,Vy]` | `8xyE` |
| `LD I,addr` | `Annn` | `ADD I,Vx` | `Fx1E` |
| `RND Vx,byte` | `Cxkk` | `DRW Vx,Vy,n` | `Dxyn` |
| `SKP Vx` | `Ex9E` | `SKNP Vx` | `ExA1` |
| `LD Vx,DT` | `Fx07` | `LD Vx,K` | `Fx0A` |
| `LD DT,Vx` | `Fx15` | `LD ST,Vx` | `Fx18` |
| `LD F,Vx` | `Fx29` | `LD B,Vx` | `Fx33` |
| `LD [I],Vx` | `Fx55` | `LD Vx,[I]` | `Fx65` |

`[I]` may also be written `(I)`. A byte may be negative (`ADD V0,-1`). `SHR
Vx` and `SHL Vx` shift `Vx` in place; the optional `Vy` is the source register
the original COSMAC VIP interpreter used.

**SUPER-CHIP 1.1** adds:

| Instruction | Opcode | Meaning |
|---|---|---|
| `SCD n` | `00Cn` | Scroll down `n` lines |
| `SCR` | `00FB` | Scroll right 4 pixels |
| `SCL` | `00FC` | Scroll left 4 pixels |
| `EXIT` | `00FD` | Exit the interpreter |
| `LOW` | `00FE` | 64×32 display |
| `HIGH` | `00FF` | 128×64 display |
| `DRW Vx,Vy,0` | `Dxy0` | Draw a 16×16 sprite |
| `LD HF,Vx` | `Fx30` | Point I at the large font digit in Vx |
| `LD R,Vx` | `Fx75` | Save V0–Vx to the RPL flags |
| `LD Vx,R` | `Fx85` | Restore V0–Vx from the RPL flags |

Use them only with an interpreter that supports SUPER-CHIP.

**Compile** saves the program as a `.ch8` file, which has no header:
interpreters load it at `$200` and start there, so the program must begin at
`$200` (the default `ORG`).

## 6502

The 56 documented instructions in every addressing mode:

| Write | Mode |
|---|---|
| `LDA #$10` | Immediate. `#<value` and `#>value` take the low and high byte. |
| `LDA $10` / `LDA $1234` | Zero page / absolute |
| `LDA $10,X` / `LDA $1234,Y` | Zero page or absolute, indexed |
| `LDA ($10,X)` | Indexed indirect |
| `LDA ($10),Y` | Indirect indexed |
| `JMP ($1234)` | Indirect |
| `ASL` or `ASL A` | Accumulator |
| `BNE label` | Relative (within -128..127 bytes) |

The shorter zero-page form is used when the address is known to be below
`$100`. `EQU` constants are always known, wherever they're defined, so
declare zero-page variables with `EQU` (`counter EQU $10`). A label defined
further down gets the absolute form even if it turns out to be in zero page,
as in most 6502 assemblers, because the instruction's size has to be fixed
before that label's address is known.

**Compile C64 PRG** saves a `.prg`: the 2-byte load address followed by the
code. Load it with `LOAD "NAME",8,1` and start it with `SYS 49152` (for the
default `ORG $C000`). **Compile Raw Binary** saves just the bytes, for other
6502 systems and emulators.

## Game Boy (SM83)

The Game Boy's SM83 is a cut-down Z80. Its instructions are the Z80's
without IX/IY, `EX`, `EXX`, `DJNZ`, `IN`/`OUT`, the block instructions and
the PO/PE/P/M conditions, which are reported as errors, plus these of its
own:

| Write | Does |
|---|---|
| `LD A,(HL+)` / `LD (HL-),A` | Load and step HL. `(HLI)`/`(HLD)` and `LDI`/`LDD` are the same. |
| `LDH (n),A` / `LDH A,(n)` | The `$FF00`-`$FFFF` I/O page; `n` can be the full address or its low byte. |
| `LDH (C),A` / `LD A,(C)` | `$FF00+C`. |
| `LD HL,SP+e` / `ADD SP,e` | SP plus a signed byte. |
| `LD (nn),SP` | Stores SP. |
| `SWAP r`, `STOP`, `RETI` | Swap nibbles; stop the CPU; return from an interrupt. |

Memory operands can be written with brackets, `LD A,[HL+]`, as in RGBDS
sources, as well as with parentheses. Only A loads from or to an address:
`LD HL,(nn)` is reported rather than read as `LD HL,nn`.

**Compile ROM** saves a 32 KB cartridge with no memory bank controller, which
every emulator and flash cartridge runs. MarkEdit writes the header at
`$0100`-`$014F` itself: `NOP` and `JP` to the entry point (`ENTRY`/`END`,
otherwise the first byte of code), the Nintendo logo the boot ROM checks, a
title from the file name, and both checksums. So code starts at `$0150`;
`ORG $0000` first puts code in the RST and interrupt vectors (the gap to
`$0150` is left for the header). Code in the header, or past `$7FFF`, is
reported.

## Examples

`examples/` holds a runnable "Hello, World!" for each assembler. Open one in
MarkEdit and use that CPU's **Compile** command:

| File | CPU | Run it with |
|---|---|---|
| `examples/z80-hello.asm` | ZX Spectrum | The saved `.tap`, with `LOAD ""`. |
| `examples/chip8-hello.asm` | CHIP-8 / SUPER-CHIP | The saved `.ch8` in any interpreter. |
| `examples/c64-hello.asm` | 6502 (Commodore 64) | The saved `.prg`, with `SYS 49152`. |
| `examples/gb-hello.asm` | Game Boy | The saved `.gb` in an emulator such as SameBoy, BGB or mGBA. |

The `examples` folder has no `init.lua`, so MarkEdit doesn't load it as a
plugin.

## Files

| File | Contents |
|---|---|
| `init.lua` | The menu commands and output formats |
| `core.lua` | The shared front end and two-pass layout |
| `z80.lua`, `chip8.lua`, `mos6502.lua`, `sm83.lua` | Instruction encoding for each CPU |
| `tap.lua` | The ZX Spectrum TAP format |
| `gbrom.lua` | The Game Boy cartridge image and header |
| `examples/` | A hello-world program for each assembler |

A new CPU is a module returning `{ name, default_origin, memory_size,
big_endian, encode }` (described at the top of `core.lua`) and a menu command
in `init.lua`. The tests are in `tests/test_assemblers.lua`.
