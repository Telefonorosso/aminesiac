#ifndef AMINESIAC_RC4_MEMORY_LAYOUT_H
#define AMINESIAC_RC4_MEMORY_LAYOUT_H
/* RC4: reserve only the fixed hash at ARM; entries grow by 1 MiB payload
 * per block. No allocation here, safe for resident COLDSTART validation. */
#include <stdint.h>
#include "cow.h"
#define RC3_MEM_MIN_MB 4U
#define RC3_MEM_MAX_MB 128U
#define RC3_SECTORS_PER_MIB RC_DYN_SECTORS_PER_BLOCK
struct rc3_arena_layout {
    uint32_t capacity,buckets,block_bytes,slot_bytes,max_blocks;
};
static int rc3_arena_layout(uint32_t mem_mb,struct rc3_arena_layout *out){
    uint32_t capacity,buckets=1U,required;
    if(!out||mem_mb<RC3_MEM_MIN_MB||mem_mb>RC3_MEM_MAX_MB)return 0;
    capacity=mem_mb*RC3_SECTORS_PER_MIB;
    required=capacity*2U;
    while(buckets<required)buckets<<=1;
    out->capacity=capacity;
    out->buckets=buckets;
    out->block_bytes=RC_DYN_BLOCK_BYTES;
    out->slot_bytes=buckets*(uint32_t)sizeof(uint32_t);
    out->max_blocks=mem_mb;
    return 1;
}
#endif
