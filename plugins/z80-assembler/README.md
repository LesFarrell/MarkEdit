# Z80 Assembler

The bundled assembler creates an auto-loading ZX Spectrum TAP from the active
document. `ORG` defaults to `$8000`.

## Symbols and expressions

Symbols are case-insensitive. A label beginning with `.` is local to the most
recent non-local colon label. Expressions support decimal, `$ffff`, `0xffff`
and `%1010` numbers, character literals, `$` for the current address, unary
`+ - ~`, arithmetic, shifts, bitwise operators and comparisons.

```asm
main:
.loop:  djnz .loop
```

`EQU` constants may refer to constants declared later. Circular definitions
are diagnosed.

## Directives

- `ORG address`
- `DB`/`DEFB` values and strings; `DEFM` is an alias
- `DW`/`DEFW` words
- `DS`/`DEFS`/`FILL count[,value]`
- `ALIGN boundary[,value]`
- `ASSERT expression[,"message"]`
- `IF expression`, `ELSE`, `ENDIF`
- `ENTRY address`
- `END [address]`

`ENTRY` and the optional `END` address select the address called by the BASIC
loader. Otherwise the first emitted address is used.

## Output

**Compile** saves a TAP containing a tokenized BASIC loader followed by the
CODE block. For code above Spectrum BASIC memory, the loader lowers RAMTOP
with `CLEAR`, loads the CODE block and calls the selected entry point.

The completion message reports origin, entry, end, size and symbol count.

Alongside documented Z80 instructions, the assembler accepts `SLL`/`SL1`,
`IXH`, `IXL`, `IYH`, `IYL`, `IN (C)`, `IN F,(C)`, and indexed-CB operations
with register destinations.
