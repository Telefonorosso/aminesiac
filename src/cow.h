#ifndef SHADOWSD_RC1_COW_H
#define SHADOWSD_RC1_COW_H
/* Preallocated, single-worker-owned map. No allocations or physical writes.
 * All I/O data are copied only inside the resident task, never from BeginIO.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define RC_SECTOR 512U
#define RC_NOCMD (-3)
#define RC_BADLENGTH (-4)
#define RC_BADADDRESS (-5)
#define RC_ABORTED (-2)
#define RC_UNITBUSY (-6)
#define RC_OUTOFMEM (-3)
struct rc_entry { uint64_t lba; uint8_t bytes[RC_SECTOR]; };
struct rc_cow {
    struct rc_entry *entries;
    uint32_t *slots; /* index+1, zero=empty */
    uint32_t capacity,buckets,used;
    uint64_t disk_sectors;
    uint32_t reads,writes,physical_reads,errors,full,blocked,aborted;
};
static uint32_t rc_hash(uint64_t lba,uint32_t mask){
    uint32_t x=(uint32_t)lba^(uint32_t)(lba>>32);
    x^=x>>16;x*=0x7feb352dUL;x^=x>>15;x*=0x846ca68bUL;x^=x>>16;
    return x&mask;
}
static int rc_find(const struct rc_cow *s,uint64_t lba){
    uint32_t i,p=rc_hash(lba,s->buckets-1U);
    for(i=0;i<s->buckets;i++){
        uint32_t v=s->slots[p];
        if(!v)return -1;
        if(s->entries[v-1U].lba==lba)return (int)(v-1U);
        p=(p+1U)&(s->buckets-1U);
    }
    return -1;
}
static int rc_bounds(const struct rc_cow *s,uint64_t off,uint32_t len){
    uint64_t first,n;
    if(!len||(len&(RC_SECTOR-1U))||(off&(RC_SECTOR-1U)))return 0;
    first=off/RC_SECTOR;n=len/RC_SECTOR;
    return first<s->disk_sectors && n<=s->disk_sectors-first;
}
/* A failed write must leave the entire map unchanged. Preflight all missing
 * sectors before copying the first block. Processed in one serialized worker. */
static int rc_write(struct rc_cow *s,uint64_t off,uint32_t len,const void *data){
    uint64_t base;uint32_t n,i,missing=0;
    if(!data||!len||len%RC_SECTOR)return RC_BADLENGTH;
    if(!rc_bounds(s,off,len))return RC_BADADDRESS;
    n=len/RC_SECTOR;base=off/RC_SECTOR;
    if(n>s->capacity){++s->full;return RC_OUTOFMEM;}
    for(i=0;i<n;i++)if(rc_find(s,base+i)<0)missing++;
    if(missing>s->capacity-s->used){++s->full;return RC_OUTOFMEM;}
    for(i=0;i<n;i++){
        uint64_t lba=base+i;
        const uint8_t *source=(const uint8_t *)data+(size_t)i*RC_SECTOR;
        int existing=rc_find(s,lba);
        if(existing>=0){memcpy(s->entries[existing].bytes,source,RC_SECTOR);continue;}
        {uint32_t p=rc_hash(lba,s->buckets-1U);
         while(s->slots[p])p=(p+1U)&(s->buckets-1U);
         s->entries[s->used].lba=lba;
         memcpy(s->entries[s->used].bytes,source,RC_SECTOR);
         s->slots[p]=++s->used;
        }
    }
    ++s->writes;return 0;
}
/* Returns 1 if physical read is unnecessary, zero if any block is missing. */
static int rc_all_ram(const struct rc_cow *s,uint64_t off,uint32_t len){
    uint32_t n,i;uint64_t base;
    if(!rc_bounds(s,off,len))return 0;
    n=len/RC_SECTOR;base=off/RC_SECTOR;
    if(n>s->used)return 0;
    for(i=0;i<n;i++)if(rc_find(s,base+i)<0)return 0;
    return 1;
}
/* Apply only changed blocks after original device's read has succeeded.
 * With all_ram==true the destination buffer need not be initialized. */
static int rc_overlay(const struct rc_cow *s,uint64_t off,uint32_t len,void *dst){
    uint32_t i;uint64_t first,last;
    if(!dst||!rc_bounds(s,off,len))return 0;
    first=off/RC_SECTOR;last=first+len/RC_SECTOR;
    /* For small filesystem reads, hash lookup per requested LBA is cheaper
     * than scanning a nearly full 32 MiB snapshot every time. */
    if((len/RC_SECTOR)<s->used){
        uint32_t n=len/RC_SECTOR;
        for(i=0;i<n;i++){
            int slot=rc_find(s,first+i);
            if(slot>=0)memcpy((uint8_t *)dst+(size_t)i*RC_SECTOR,
                              s->entries[slot].bytes,RC_SECTOR);
        }
    }else{
        for(i=0;i<s->used;i++){
            uint64_t lba=s->entries[i].lba;
            if(lba>=first&&lba<last)
                memcpy((uint8_t *)dst+(size_t)(lba-first)*RC_SECTOR,
                       s->entries[i].bytes,RC_SECTOR);
        }
    }
    return 1;
}
static uint64_t rc_decode_offset(uint16_t cmd,uint32_t high,uint32_t low){
    return (cmd==0xc000||cmd==0xc001)?(((uint64_t)high<<32)|low):(uint64_t)low;
}
#endif
