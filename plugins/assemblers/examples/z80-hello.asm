; ZX Spectrum "Hello, World!" for the bundled Z80 assembler.
;
; Open this file in Tinker Editor, then choose
;   Tools -> Assemblers -> Z80 Assembler -> Compile
; and save the .tap file, or use Compile and Run to open it in the application
; associated with .tap files. Load it in a Spectrum emulator (or real hardware)
; with
;   LOAD ""
; The TAP's BASIC loader CLEARs memory below the code, loads it and starts
; execution at the entry point ($8000 here).

        ORG  $8000

start:
        LD   A,2            ; channel 2 is the screen
        CALL $1601          ; ROM CHAN-OPEN: select the screen channel
        LD   HL,message
print:
        LD   A,(HL)         ; next character
        OR   A              ; a zero byte ends the string
        JR   Z,done
        RST  $10            ; ROM: print the character in A
        INC  HL
        JR   print
done:
        RET

message:
        DB   "Hello, World!", 13, 0
