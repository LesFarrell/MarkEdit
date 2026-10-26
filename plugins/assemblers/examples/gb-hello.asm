; Game Boy "HELLO, WORLD!" for the bundled SM83 assembler.
;
; Open this file in MarkEdit, then choose
;   Tools -> Assemblers -> Game Boy Assembler -> Compile ROM
; and open the saved .gb in an emulator such as SameBoy, BGB or mGBA.
;
; The Game Boy has no text routines, so the program brings its own font: it
; switches the screen off (only allowed during vertical blank), copies the
; letter tiles into video RAM, writes the message into the background map
; and switches the screen on again. The assembler writes the cartridge
; header at $0100-$014F itself, so code starts at $0150.

rLCDC   EQU $FF40           ; LCD control
rLY     EQU $FF44           ; the line being drawn; 144-153 is vertical blank
rBGP    EQU $FF47           ; background palette
TILES   EQU $8000           ; tile data
MAP     EQU $9800           ; the 32x32 background map

        ORG $0150
start:  DI
.vblank: LDH A,(rLY)
        CP 144
        JR C,.vblank
        XOR A
        LDH (rLCDC),A       ; screen off, so video RAM can be written

        LD HL,TILES         ; copy the font
        LD DE,font
        LD BC,font_end-font
.tiles: LD A,(DE)
        LD (HL+),A
        INC DE
        DEC BC
        LD A,B
        OR C
        JR NZ,.tiles

        LD HL,MAP           ; clear the map to tile 0, a blank
        LD BC,32*32
.clear: XOR A
        LD (HL+),A
        DEC BC
        LD A,B
        OR C
        JR NZ,.clear

        LD HL,MAP+32*8+3    ; the message on row 8, from column 3
        LD DE,message
.text:  LD A,(DE)
        CP $FF
        JR Z,.show
        LD (HL+),A
        INC DE
        JR .text

.show:  LD A,%11100100      ; the usual shades: white, light, dark, black
        LDH (rBGP),A
        LD A,%10010001      ; screen on, tiles at $8000, background on
        LDH (rLCDC),A
.done:  JR .done

; Tile numbers for "HELLO, WORLD!", then $FF.
message: DB 1,2,3,3,4,5,0,6,4,7,3,8,9,$FF

; 8x8 tiles, 16 bytes each: every row is given twice, once for each of the
; two bit planes, which draws it in the darkest shade.
font:
        DB $00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00 ; 0 space
        DB $66,$66,$66,$66,$66,$66,$7E,$7E,$66,$66,$66,$66,$66,$66,$00,$00 ; 1 H
        DB $7E,$7E,$60,$60,$60,$60,$7C,$7C,$60,$60,$60,$60,$7E,$7E,$00,$00 ; 2 E
        DB $60,$60,$60,$60,$60,$60,$60,$60,$60,$60,$60,$60,$7E,$7E,$00,$00 ; 3 L
        DB $3C,$3C,$66,$66,$66,$66,$66,$66,$66,$66,$66,$66,$3C,$3C,$00,$00 ; 4 O
        DB $00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$18,$18,$18,$18,$30,$30 ; 5 ,
        DB $63,$63,$63,$63,$63,$63,$6B,$6B,$7F,$7F,$77,$77,$63,$63,$00,$00 ; 6 W
        DB $7C,$7C,$66,$66,$66,$66,$7C,$7C,$6C,$6C,$66,$66,$66,$66,$00,$00 ; 7 R
        DB $78,$78,$6C,$6C,$66,$66,$66,$66,$66,$66,$6C,$6C,$78,$78,$00,$00 ; 8 D
        DB $18,$18,$18,$18,$18,$18,$18,$18,$18,$18,$00,$00,$18,$18,$00,$00 ; 9 !
font_end:
