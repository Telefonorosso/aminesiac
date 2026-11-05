#ifndef AMINESIAC_RC4_COW_H
#define AMINESIAC_RC4_COW_H
/* Single-worker-owned COW. The hash stays in FAST+KICK RAM; sector records
 * are allocated in 2048-entry FAST+KICK blocks, never from BeginIO.
 * Block pointers live in the persistent CHIP header. No physical writes.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define RC_SECTOR 512U
#define RC_DYN_SECTORS_PER_BLOCK 2048U
#define RC_DYN_MAX_BLOCKS 128U
#define RC_NOCMD (-3)
#define RC_BADLENGTH (-4)
#define RC_BADADDRESS (-5)
#define RC_ABORTED (-2)
#define RC_UNITBUSY (-6)
#define RC_OUTOFMEM (-3)
struct rc_entry { uint64_t lba; uint8_t bytes[RC_SECTOR]; };
#define RC_DYN_BLOCK_BYTES (RC_DYN_SECTORS_PER_BLOCK * (uint32_t)sizeof(struct rc_entry))
struct rc_cow {
    struct rc_entry *blocks[RC_DYN_MAX_BLOCKS];
    uint32_t *slots; /* index+1; zero means empty. Hash never resized. */
    uint32_t capacity,buckets,used,allocated_blocks;
    uint64_t disk_sectors;
    uint32_t reads,writes,physical_reads,errors,full,blocked,aborted;
};
static struct rc_entry *rc_entry_at(const struct rc_cow *s,uint32_t index){
    uint32_t block=index/RC_DYN_SECTORS_PER_BLOCK;
    if(block>=s->allocated_blocks || !s->blocks[block])return NULL;
    return &s->blocks[block][index%RC_DYN_SECTORS_PER_BLOCK];
}
static uint32_t rc_hash(uint64_t lba,uint32_t mask){
    uint32_t x=(uint32_t)lba^(uint32_t)(lba>>32);
    x^=x>>16;x*=0x7feb352dUL;x^=x>>15;x*=0x846ca68bUL;x^=x>>16;
    return x&mask;
}
static int rc_find(const struct rc_cow *s,uint64_t lba){
    uint32_t i,p;
    if(!s->slots || !s->buckets || (s->buckets&(s->buckets-1U)))return -1;
    p=rc_hash(lba,s->buckets-1U);
    for(i=0;i<s->buckets;i++){
        uint32_t v=s->slots[p];
        const struct rc_entry *e;
        if(!v)return -1;
        if(v>s->used)return -1; /* recovery validation checks this earlier */
        e=rc_entry_at(s,v-1U);
        if(!e)return -1;
        if(e->lba==lba)return (int)(v-1U);
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
/* Pure preflight; no map mutations. Caller must allocate/register ALL blocks
 * before calling rc_write. Contiguous LBA ranges contain no duplicates.
 * Returns a negative I/O error on capacity overflow or bad arguments. */
static int rc_growth_needed(const struct rc_cow *s,uint64_t off,uint32_t len,
                            uint32_t *extra_blocks){
    uint64_t base;
    uint32_t i,n,missing=0,needed;
    if(extra_blocks)*extra_blocks=0;
    if(!extra_blocks || !s || !rc_bounds(s,off,len))return RC_BADADDRESS;
    n=len/RC_SECTOR;base=off/RC_SECTOR;
    if(n>s->capacity || s->used>s->capacity){return RC_OUTOFMEM;}
    for(i=0;i<n;i++)if(rc_find(s,base+i)<0)missing++;
    if(missing>s->capacity-s->used)return RC_OUTOFMEM;
    needed=(s->used+missing+RC_DYN_SECTORS_PER_BLOCK-1U)/RC_DYN_SECTORS_PER_BLOCK;
    if(needed>RC_DYN_MAX_BLOCKS || needed<s->allocated_blocks)return RC_OUTOFMEM;
    *extra_blocks=needed-s->allocated_blocks;
    return 0;
}
/* A failed write must leave the entire map unchanged. The worker first
 * ensures the required blocks are allocated AND KickMem-registered. */
static int rc_write(struct rc_cow *s,uint64_t off,uint32_t len,const void *data){
    uint64_t base;uint32_t n,i,extra;
    if(!data||!len||len%RC_SECTOR)return RC_BADLENGTH;
    if(!rc_bounds(s,off,len))return RC_BADADDRESS;
    if(rc_growth_needed(s,off,len,&extra)){
        ++s->full;return RC_OUTOFMEM;
    }
    if(extra){++s->full;return RC_OUTOFMEM;}
    n=len/RC_SECTOR;base=off/RC_SECTOR;
    for(i=0;i<n;i++){
        uint64_t lba=base+i;
        const uint8_t *source=(const uint8_t *)data+(size_t)i*RC_SECTOR;
        int existing=rc_find(s,lba);
        if(existing>=0){
            struct rc_entry *e=rc_entry_at(s,(uint32_t)existing);
            if(!e)return RC_BADADDRESS;
            memcpy(e->bytes,source,RC_SECTOR);continue;
        }
        {uint32_t p=rc_hash(lba,s->buckets-1U);
         struct rc_entry *e=rc_entry_at(s,s->used);
         if(!e)return RC_BADADDRESS;
         while(s->slots[p])p=(p+1U)&(s->buckets-1U);
         e->lba=lba;
         memcpy(e->bytes,source,RC_SECTOR);
         s->slots[p]=++s->used;
        }
    }
    ++s->writes;return 0;
}
static int rc_all_ram(const struct rc_cow *s,uint64_t off,uint32_t len){
    uint32_t n,i;uint64_t base;
    if(!rc_bounds(s,off,len))return 0;
    n=len/RC_SECTOR;base=off/RC_SECTOR;
    if(n>s->used)return 0;
    for(i=0;i<n;i++)if(rc_find(s,base+i)<0)return 0;
    return 1;
}
static int rc_overlay(const struct rc_cow *s,uint64_t off,uint32_t len,void *dst){
    uint32_t i;uint64_t first,last;
    if(!dst||!rc_bounds(s,off,len))return 0;
    first=off/RC_SECTOR;last=first+len/RC_SECTOR;
    if((len/RC_SECTOR)<s->used){
        uint32_t n=len/RC_SECTOR;
        for(i=0;i<n;i++){
            int slot=rc_find(s,first+i);
            if(slot>=0){
                const struct rc_entry *e=rc_entry_at(s,(uint32_t)slot);
                if(!e)return 0;
                memcpy((uint8_t *)dst+(size_t)i*RC_SECTOR,e->bytes,RC_SECTOR);
            }
        }
    }else{
        for(i=0;i<s->used;i++){
            const struct rc_entry *e=rc_entry_at(s,i);
            uint64_t lba;
            if(!e)return 0;
            lba=e->lba;
            if(lba>=first&&lba<last)
                memcpy((uint8_t *)dst+(size_t)(lba-first)*RC_SECTOR,e->bytes,RC_SECTOR);
        }
    }
    return 1;
}
static uint64_t rc_decode_offset(uint16_t cmd,uint32_t high,uint32_t low){
    return (cmd==0xc000||cmd==0xc001)?(((uint64_t)high<<32)|low):(uint64_t)low;
}
#endif
