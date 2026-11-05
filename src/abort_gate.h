#ifndef SHADOWSD_RC1_ABORT_GATE_H
#define SHADOWSD_RC1_ABORT_GATE_H
/* AbortIO LVO -36: A1=request. helper returns 0 if accepted, -1 otherwise.
 * Preserve caller registers on both paths; handled branch returns D0=0.
 * Tested separately from the known 60-byte BeginIO gate.
 */
#include <stdint.h>
#define RA_GATE_LEN 32U
static void ra_word(uint8_t *p,uint16_t w){p[0]=(uint8_t)(w>>8);p[1]=(uint8_t)w;}
static void ra_long(uint8_t *p,uint32_t w){ra_word(p,(uint16_t)(w>>16));ra_word(p+2,(uint16_t)w);}
static uint16_t ra_get16(const uint8_t *p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
static uint32_t ra_get32(const uint8_t *p){return ((uint32_t)ra_get16(p)<<16)|ra_get16(p+2);}
static int ra_emit(uint8_t p[RA_GATE_LEN],uint32_t helper,uint32_t orig){
    /* 0  movem.l d0-d7/a0-a6,-(sp)
     * 4  move.l a1,-(sp)
     * 6  jsr helper
     * 12 addq.l #4,sp
     * 14 tst.l d0
     * 16 movem.l (sp)+,d0-d7/a0-a6
     * 20 bne.s original@26
     * 22 moveq #0,d0
     * 24 rts
     * 26 jmp orig
     */
    ra_word(p,0x48e7);ra_word(p+2,0xfffe);
    ra_word(p+4,0x2f09);
    ra_word(p+6,0x4eb9);ra_long(p+8,helper);
    ra_word(p+12,0x588f);ra_word(p+14,0x4a80);
    ra_word(p+16,0x4cdf);ra_word(p+18,0x7fff);
    ra_word(p+20,0x6604);ra_word(p+22,0x7000);
    ra_word(p+24,0x4e75);ra_word(p+26,0x4ef9);ra_long(p+28,orig);
    return 1;
}
static int ra_validate(const uint8_t *p,uint32_t h,uint32_t o){
    return ra_get16(p)==0x48e7&&ra_get16(p+2)==0xfffe&&
           ra_get16(p+4)==0x2f09&&ra_get16(p+6)==0x4eb9&&
           ra_get32(p+8)==h&&ra_get16(p+12)==0x588f&&
           ra_get16(p+14)==0x4a80&&ra_get16(p+16)==0x4cdf&&
           ra_get16(p+18)==0x7fff&&ra_get16(p+20)==0x6604&&
           22+(int8_t)(p[21])==26&&ra_get16(p+22)==0x7000&&
           ra_get16(p+24)==0x4e75&&ra_get16(p+26)==0x4ef9&&ra_get32(p+28)==o;
}
#endif
