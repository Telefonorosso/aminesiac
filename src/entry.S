/* Amiga m68k HUNK first-segment entry and Exec callback wrappers.
 * This entry object MUST be the first linker object in the resident module.
 * GCC m68k-amigaos follows the leading underscore external-name ABI.
 */
    .text
    .even
    .globl _start
_start:
    move.l #_rc2_exports,d0
    rts

    .even
    .globl _rc2_init_entry
_rc2_init_entry:
    movem.l d2-d7/a2-a6,-(sp)
    move.l a6,-(sp)
    jsr _rc2_coldstart
    addq.l #4,sp
    movem.l (sp)+,d2-d7/a2-a6
    moveq #0,d0
    rts


/* Exec AddDevice ABI: A1=device; callback runs after original AddDevice
 * and BEFORE any later OpenDevice from the filesystem. Does not allocate.
 * The device gate is installed here, before the normal open path. */
    .even
    .globl _rc2_add_entry
_rc2_add_entry:
    movem.l d0-d7/a0-a6,-(sp)
    move.l _rc2_previous_add,a2
    jsr (a2)
    move.l 36(sp),-(sp) /* saved A1 pointer */
    jsr _rc2_add_observe
    addq.l #4,sp
    movem.l (sp)+,d0-d7/a0-a6
    rts

/* Exec OpenDevice ABI: A0=name D0=unit A1=request D1=flags; result D0.
 * Call original, then observe the newly opened unit before returning to caller.
 */
    .even
    .globl _rc2_open_entry
_rc2_open_entry:
    movem.l d0-d7/a0-a6,-(sp)
    move.l _rc2_previous_open,a2
    jsr (a2)
    move.l d0,-(sp)
    move.l 40(sp),-(sp)  /* saved A1 IORequest */
    move.l 8(sp),-(sp)   /* saved D0 unit */
    move.l 44(sp),-(sp)  /* saved A0 name */
    jsr _rc2_open_observe
    lea 12(sp),sp
    move.l (sp)+,d0
    addq.l #4,sp        /* discard originally saved D0 */
    movem.l (sp)+,d1-d7/a0-a6
    rts

/* Device BeginIO ABI A1=IORequest; return 1 = we took ownership.
 * For all other I/O, chain to original driver unchanged. */
    .even
    .globl _rc2_begin_entry
_rc2_begin_entry:
    movem.l d0-d7/a0-a6,-(sp)
    move.l a1,-(sp)
    jsr _rc2_dispatch
    addq.l #4,sp
    tst.l d0
    movem.l (sp)+,d0-d7/a0-a6
    bne.s 1f
    move.l _rc2_previous_begin,-(sp)
    rts
1:  rts

/* AbortIO handled in RC1 fashion; 0 handled, -1 chain. */
    .even
    .globl _rc2_abort_entry
_rc2_abort_entry:
    movem.l d0-d7/a0-a6,-(sp)
    move.l a1,-(sp)
    jsr _rc2_abort
    addq.l #4,sp
    tst.l d0
    movem.l (sp)+,d0-d7/a0-a6
    bne.s 2f
    moveq #0,d0
    rts
2:  move.l _rc2_previous_abort,-(sp)
    rts
