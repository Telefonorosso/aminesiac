#ifndef SHADOWSD_DUAL_GATE_H
#define SHADOWSD_DUAL_GATE_H
/* Two-unit BeginIO trampoline. Both units enter C for ALL commands, so
 * unknown command IDs on protected units can be rejected fail-closed.
 * Original POC3-B 208-byte gate is retained in untouched baseline archive.
 * 68k instruction positions are tested independently in tests/test_gate.c.
 * Register save/restore mask and C call ABI match validated POC3-B gate.
 */
#include <stdint.h>
#include <stddef.h>
#define SW_GATE_LEN 60U
static void sw_put16(uint8_t *p,uint16_t x){p[0]=(uint8_t)(x>>8);p[1]=(uint8_t)x;}
static void sw_put32(uint8_t *p,uint32_t x){sw_put16(p,(uint16_t)(x>>16));sw_put16(p+2,(uint16_t)x);}
static uint16_t sw_get16(const uint8_t *p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
static uint32_t sw_get32(const uint8_t *p){return ((uint32_t)sw_get16(p)<<16)|sw_get16(p+2);}
static int sw_emit_gate(uint8_t out[SW_GATE_LEN],uint32_t u0,uint32_t u1,
                        uint32_t helper,uint32_t original){
    size_t at=0;
    sw_put16(out+at,0x0ca9);sw_put32(out+at+2,u0);sw_put16(out+at+6,24);at+=8; /* cmp.l #u0,24(a1) */
    sw_put16(out+at,0x6700);sw_put16(out+at+2,14);at+=4; /* beq.w dispatch@24: PC@10 */
    sw_put16(out+at,0x0ca9);sw_put32(out+at+2,u1);sw_put16(out+at+6,24);at+=8;
    sw_put16(out+at,0x6600);sw_put16(out+at+2,32);at+=4; /* bne.w fallback@54: PC@22 */
    sw_put16(out+at,0x48e7);sw_put16(out+at+2,0xfffe);at+=4;
    sw_put16(out+at,0x2f09);at+=2;
    sw_put16(out+at,0x4eb9);sw_put32(out+at+2,helper);at+=6;
    sw_put16(out+at,0x588f);at+=2;
    sw_put16(out+at,0x4a80);at+=2;
    sw_put16(out+at,0x4cdf);sw_put16(out+at+2,0x7fff);at+=4;
    sw_put16(out+at,0x6606);at+=2;
    sw_put16(out+at,0x4ef9);sw_put32(out+at+2,original);at+=6;
    sw_put16(out+at,0x4e75);at+=2;
    sw_put16(out+at,0x4ef9);sw_put32(out+at+2,original);at+=6;
    return at==SW_GATE_LEN;
}
static int sw_validate_gate(const uint8_t *p,uint32_t u0,uint32_t u1,
                            uint32_t helper,uint32_t orig){
    if(sw_get16(p)!=0x0ca9 || sw_get32(p+2)!=u0 || sw_get16(p+6)!=24 ||
       sw_get16(p+8)!=0x6700 || 10+(int16_t)sw_get16(p+10)!=24 ||
       sw_get16(p+12)!=0x0ca9 || sw_get32(p+14)!=u1 || sw_get16(p+18)!=24 ||
       sw_get16(p+20)!=0x6600 || 22+(int16_t)sw_get16(p+22)!=54 ||
       sw_get16(p+24)!=0x48e7 || sw_get16(p+26)!=0xfffe ||
       sw_get16(p+28)!=0x2f09 || sw_get16(p+30)!=0x4eb9 ||
       sw_get32(p+32)!=helper || sw_get16(p+36)!=0x588f ||
       sw_get16(p+38)!=0x4a80 || sw_get16(p+40)!=0x4cdf ||
       sw_get16(p+42)!=0x7fff || sw_get16(p+44)!=0x6606 ||
       sw_get16(p+46)!=0x4ef9 || sw_get32(p+48)!=orig ||
       sw_get16(p+52)!=0x4e75 || sw_get16(p+54)!=0x4ef9 ||
       sw_get32(p+56)!=orig)return 0;
    return 1;
}
#endif
