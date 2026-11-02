#ifndef SHADOWSD_ALIAS_MAP_H
#define SHADOWSD_ALIAS_MAP_H
/* Pure C MBR selection / sector translation. No Amiga API. */
#include <stdint.h>
#include <stddef.h>
#define AM_SECTOR 512U
struct am_partition { uint64_t start,count; uint32_t mbr_slot; };
static uint32_t am_le32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
/* Strict single-0x76 layout POC: reject ambiguity rather than guess unit numbers. */
static int am_parse_single_76(const uint8_t mbr[512],uint64_t disk_sectors,
                              struct am_partition *part) {
    uint32_t i,n=0;
    if(!mbr || !part || disk_sectors==0 || mbr[510]!=0x55 || mbr[511]!=0xaa)return 0;
    for(i=0;i<4;i++){
        const uint8_t *e=mbr+446+i*16;
        uint64_t start=(uint64_t)am_le32(e+8),count=(uint64_t)am_le32(e+12);
        if(e[4]!=0x76)continue;
        if(start==0 || count==0 || start>=disk_sectors || count>disk_sectors-start)
            return 0;
        part->start=start;part->count=count;part->mbr_slot=i;
        ++n;
    }
    return n==1;
}
/* Device unit0 offsets are absolute. Unit1 offsets are within MBR 0x76. */
static int am_translate(const struct am_partition *part,uint8_t unit,
                         uint64_t byte_off,uint32_t len,uint64_t disk_sectors,
                         uint64_t *physical_byte) {
    uint64_t n,first,limit,base;
    if(!part || !physical_byte || !len || len%512 || byte_off%512)return 0;
    n=len/512;first=byte_off/512;
    if(unit==0){base=0;limit=disk_sectors;}
    else if(unit==1){base=part->start;limit=part->count;}
    else return 0;
    if(first>=limit || n>limit-first || base>UINT64_MAX-first)return 0;
    /* Validate BEFORE multiplication, no wrapped byte offset accepted. */
    if(base+first>UINT64_MAX/512ULL)return 0;
    *physical_byte=(base+first)*512ULL;
    return 1;
}
#endif
