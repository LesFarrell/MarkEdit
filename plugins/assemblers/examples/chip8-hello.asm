; CHIP-8 "HELLO" for the bundled CHIP-8 assembler.
;
; Open this file in Tinker Editor, then choose
;   Tools -> CHIP-8 Assembler -> Compile
; and save the .ch8 file. Load it in a CHIP-8 interpreter.
;
; A .ch8 program has no header: interpreters load it at $200 and start there,
; which is the assembler's default ORG, so no ORG line is needed.

start:
        LD   V0,14          ; x, roughly centred on the 64-pixel display
        LD   V1,12          ; y
        LD   I,letterH
        DRW  V0,V1,5
        ADD  V0,5
        LD   I,letterE
        DRW  V0,V1,5
        ADD  V0,5
        LD   I,letterL
        DRW  V0,V1,5
        ADD  V0,5
        DRW  V0,V1,5        ; the second L reuses letterL
        ADD  V0,5
        LD   I,letterO
        DRW  V0,V1,5

loop:
        JP   loop           ; nothing else to do

; Each glyph is 5 rows of 4 pixels, held in the high nibble of each byte
; (bit 7 is the leftmost pixel); the low nibble stays blank for letter spacing.
letterH:
        DB   $90,$90,$F0,$90,$90
letterE:
        DB   $F0,$80,$E0,$80,$F0
letterL:
        DB   $80,$80,$80,$80,$F0
letterO:
        DB   $60,$90,$90,$90,$60
