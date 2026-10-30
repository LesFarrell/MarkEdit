# CHIP-8 and SUPER-CHIP

See [the assemblers overview](README.md) for the shared source syntax,
expressions and directives (`ORG`, `EQU`, `DB`, `IF`, and so on) — this page
covers only what's specific to CHIP-8.

## Menu

| Command | Does |
|---|---|
| **CHIP-8 Assembler → Compile** | Saves a `.ch8` program for CHIP-8 and SUPER-CHIP 1.1. |

Default `ORG`: `$200`.

## Instruction set

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

**Supports:** every original CHIP-8 opcode and all of SUPER-CHIP 1.1's
additions above.
**Does not support:** XO-CHIP (Octo) extensions — no 16-bit `i := long`,
extra bitplane/scroll-up opcodes, audio pattern buffer, or `pitch`/`buffer`
registers. Sticking to CHIP-8/SUPER-CHIP mnemonics keeps a program portable
to interpreters that don't implement XO-CHIP either.

## Compile

Saves the program as a `.ch8` file, which has no header: interpreters load
it at `$200` and start there, so the program must begin at `$200` (the
default `ORG`).

## Example

`examples/chip8-hello.asm` — open it and use **Compile**, then run the saved
`.ch8` in any CHIP-8/SUPER-CHIP interpreter.
