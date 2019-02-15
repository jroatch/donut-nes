; "Donut", NES CHR codec decompressor,
; Copyright (c) 2018  Johnathan Roatch
;
; Copying and distribution of this file, with or without
; modification, are permitted in any medium without royalty provided
; the copyright notice and this notice are preserved in all source
; code copies.  This file is offered as-is, without any warranty.
;
; Version History:
; 2019-02-15: Swapped the M and L bits, for conceptual consistency.
;             Also rearranged branches for speed.
; 2019-02-07: Removed "Duplicate" block type, and moved
;             Uncompressed block to below 0xc0 to make room
;             for block handling commands in the 0xc0~0xff space
; 2018-09-29: Removed block option of XORing with existing block
;             for extra speed in decoding.
; 2018-08-13: Changed the format of raw blocks to not be reversed.
;             Register X is now an argument for the buffer offset.
; 2018-04-30: Initial release.
;

.export donut_decompress_block, donut_block_ayx, donut_block_x
.export donut_block_buffer
.exportzp donut_stream_ptr
.exportzp donut_block_count

temp = $00  ; 15 bytes are used

donut_block_buffer = $0100  ; 64 bytes

.segment "ZEROPAGE"
donut_stream_ptr:       .res 2
donut_block_count:      .res 1

.segment "CODE"

;;
; Decompresses a single variable sized block pointed to by donut_stream_ptr
; Outputing 64 bytes to donut_block_buffer offsetted by the X register.
; On success, 64 will be added to the X register, and donut_block_count
; will be decremented.
;
; Block header:
; LMlmbbBR
; |||||||+-- Rotate plane bits (135° reflection)
; ||||||+--- 0: use bits 'bb' to choose what planes are 0x00
; ||||||     1: Ignore 'bb' bits and load header byte.
; ||||||        For each bit starting from MSB
; ||||||       0: 0x00 plane
; ||||||       1: pb8 plane
; ||||00---- All planes: 0x00
; ||||01---- L planes: 0x00, M planes:  pb8
; ||||10---- L planes:  pb8, M planes: 0x00
; ||||11---- All planes: pb8
; |||+------ M planes predict from 0xff
; ||+------- L planes predict from 0xff
; |+-------- M = M XOR L
; +--------- L = M XOR L
; 00101010-- Uncompressed block of 64 bytes (bit pattern is ascii '*' )
; Header >= 0xc0: Error, avaliable for outside processing.
;
; Trashes Y, A, temp 0 ~ temp 15.
; bytes: 246, cycles: 1262 ~ 7137(compressor limit) or 7202(actual max).
.proc donut_decompress_block
plane_buffer        = temp+0 ; 8 bytes
pb8_ctrl            = temp+8
temp_y              = pb8_ctrl
even_odd            = temp+9
block_offset        = temp+10
plane_def           = temp+11
block_offset_end    = temp+12
block_header        = temp+13
is_rotated          = temp+14
;_donut_unused_temp  = temp+15
  txa
  clc
  adc #64
  sta block_offset_end

  ldy #$00
  lda (donut_stream_ptr), y
    ; Reading a byte from the stream pointer will be pre-increment
    ; So save the last increment until routine is done

  cmp #$2a
  beq do_raw_block
  ;,; bne do_normal_block
do_normal_block:
  cmp #$c0
  bcc continue_normal_block
  ;,; bcs exit_error

; I'm inserting these things here instead of above the donut_decompress_block
; at the cost of 1 cycle with the continue_normal_block branch for these reasons:
; The start of the main routine remains at the start of the .proc scope
; and I can save 1 byte with 'bcs end_block'

exit_error:
rts
; If we don't exit here, xor_l_onto_m can underflow into zeropage.

shorthand_plane_def_table:
  .byte $00, $55, $aa, $ff

read_plane_def_from_stream:
  iny
  lda (donut_stream_ptr), y
bcs plane_def_ready  ;,; jmp plane_def_ready

do_raw_block:
  ;,; ldx block_offset
  raw_block_loop:
    iny
    lda (donut_stream_ptr), y
    sta donut_block_buffer, x
    inx
    cpy #65-1  ; size of a raw block, minus pre-increment
  bcc raw_block_loop
  sty temp_y
bcs end_block  ;,; jmp end_block

continue_normal_block:
  sta block_header
  stx block_offset

  ;,; lda block_header
  and #%11011111
    ; The 0 are bits selected for the even ("lower") planes
    ; The 1 are bits selected for the odd planes
    ; bits 0~3 should be set to allow the mask after this to work.
  sta even_odd
    ; even_odd toggles between the 2 fields selected above for each plane.

  ;,; lda block_header
  and #$0f
  lsr
  ror is_rotated
  lsr
  bcs read_plane_def_from_stream
  ;,; bcc unpack_shorthand_plane_def
  unpack_shorthand_plane_def:
    ;,; and #$03
    tax
    lda shorthand_plane_def_table, x
  plane_def_ready:
  sta plane_def
  sty temp_y

  lda block_offset
  plane_loop:
    clc
    adc #8
    sta block_offset

    lda even_odd
    eor block_header
    sta even_odd

    ;,; lda even_odd
    and #$30
    beq not_predicted_from_ff
      lda #$ff
    not_predicted_from_ff:
      ; else A = 0x00

    asl plane_def
    bcc do_zero_plane
    ;,; bcs do_pb8_plane
  do_pb8_plane:
    tax
    ldy temp_y
    iny
    lda (donut_stream_ptr), y
    sta pb8_ctrl
    txa

    bit is_rotated
  bmi do_rotated_pb8_plane
  ;,; bpl do_normal_pb8_plane
  do_normal_pb8_plane:
    ldx block_offset
    sec
    rol pb8_ctrl
    pb8_loop:
      bcc pb8_use_prev
        iny
        lda (donut_stream_ptr), y
      pb8_use_prev:
      dex
      sta donut_block_buffer, x
      asl pb8_ctrl
    bne pb8_loop
    sty temp_y
  ;,; beq end_plane  ;,; jmp end_plane
  end_plane:
    bit even_odd
    bpl not_xor_m_onto_l
    xor_m_onto_l:
      ldy #8
      xor_m_onto_l_loop:
        dex
        lda donut_block_buffer, x
        eor donut_block_buffer+8, x
        sta donut_block_buffer, x
        dey
      bne xor_m_onto_l_loop
    not_xor_m_onto_l:

    bvc not_xor_l_onto_m
    xor_l_onto_m:
      ldy #8
      xor_l_onto_m_loop:
        dex
        lda donut_block_buffer, x
        eor donut_block_buffer+8, x
        sta donut_block_buffer+8, x
        dey
      bne xor_l_onto_m_loop
    not_xor_l_onto_m:

    lda block_offset
    cmp block_offset_end
  bne plane_loop
  tax  ;,; ldx block_offset_end
end_block:
  sec  ; Add 1 to finalize the pre-increment setup
  lda temp_y
  adc donut_stream_ptr+0
  sta donut_stream_ptr+0
  bcc add_stream_ptr_no_inc_high_byte
    inc donut_stream_ptr+1
  add_stream_ptr_no_inc_high_byte:
  dec donut_block_count
rts

do_zero_plane:
  ldx block_offset
  ldy #8
  fill_plane_loop:
    dex
    sta donut_block_buffer, x
    dey
  bne fill_plane_loop
beq end_plane  ;,; jmp end_plane

do_rotated_pb8_plane:
  ldx #8
  buffered_pb8_loop:
    asl pb8_ctrl
    bcc buffered_pb8_use_prev
      iny
      lda (donut_stream_ptr), y
    buffered_pb8_use_prev:
    dex
    sta plane_buffer, x
  bne buffered_pb8_loop
  sty temp_y
  ldy #8
  ldx block_offset
  flip_bits_loop:
    asl plane_buffer+0
    ror
    asl plane_buffer+1
    ror
    asl plane_buffer+2
    ror
    asl plane_buffer+3
    ror
    asl plane_buffer+4
    ror
    asl plane_buffer+5
    ror
    asl plane_buffer+6
    ror
    asl plane_buffer+7
    ror
    dex
    sta donut_block_buffer, x
    dey
  bne flip_bits_loop
beq end_plane  ;,; jmp end_plane
.endproc

;;
; helper subroutine for passing parameters with registers
; decompress X*64 bytes starting at AAYY to PPU_DATA
.proc donut_block_ayx
  sty donut_stream_ptr+0
  sta donut_stream_ptr+1
;,; jmp donut_block_x
.endproc
.proc donut_block_x
PPU_DATA = $2007
  stx donut_block_count
  block_loop:
    ldx #64
    jsr donut_decompress_block
    cpx #128
    bne end_block_upload
      ; bail if donut_decompress_block does not
      ; advance X by 64 bytes, indicating a header error.

    ldx #64
    upload_loop:
      lda donut_block_buffer, x
      sta PPU_DATA
      inx
    bpl upload_loop
    ldx donut_block_count
  bne block_loop
end_block_upload:
rts
.endproc
