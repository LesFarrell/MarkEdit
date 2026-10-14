; Commodore 64 "HELLO, WORLD!" for the bundled 6502 assembler.
;
; Open this file in MarkEdit, then choose
;   Tools -> 6502 Assembler -> Compile C64 PRG
; and save the .prg file. In a C64 (or emulator):
;   LOAD "HELLO",8,1
;   SYS 49152
; 49152 is the default ORG, $C000. The KERNAL CHROUT routine at $FFD2 prints
; the PETSCII character in A.

        ORG  $C000

start:
        LDX  #0
loop:
        LDA  message,X      ; next character
        BEQ  done           ; a zero byte ends the string
        JSR  $FFD2          ; KERNAL CHROUT
        INX
        BNE  loop
done:
        RTS                 ; back to BASIC

message:
        DB   "HELLO, WORLD!", 13, 0
