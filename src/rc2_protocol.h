#ifndef AMINESIAC_RC4_PROTOCOL_H
#define AMINESIAC_RC4_PROTOCOL_H
#include <stdint.h>
#include <exec/types.h>
#include <exec/resident.h>
#include <exec/memory.h>
#include "cow.h"
#include "alias_map.h"
#define RC2_MAGIC 0x53445234UL /* SDR4, reject earlier persistent states */
#define RC2_VERSION 7UL
#define RC2_NAME "aminesiac.RC4"
#define RC2_KICK_NAME "aminesiac.RC4.Kick"
#define RC2_DEVICE "brcm-sdhc.device"
#define RC2_DEFAULT_MB 32UL
#define RC2_MAX_HUNKS 12
#define RC2_KICK_MAX_ENTRIES (RC2_MAX_HUNKS+2+RC_DYN_MAX_BLOCKS)
#define RC2_CLEAN 0x434c454eUL
#define RC2_DIRTY 0x44495254UL
#define RC2_READY 0x52445932UL
#define RC2_FAULT 0x464c5432UL
#define RC2_SECTOR 512UL
struct rc2_store {
    ULONG magic,version,self,bytes,capacity,buckets,block_bytes,slot_bytes;
    ULONG integrity,boot_count,armed_count,worker_ready,change_count;
    ULONG disk_fingerprint,disk_fingerprint_inverse;
    ULONG busy,busy_inverse,fail_code;
    ULONG gate_add,gate_open,gate_begin,gate_abort;
    struct MemList *kick_mem;
    ULONG kick_base_entries,kick_max_entries;
    struct am_partition part;
    struct rc_cow cow;
};
struct rc2_exports {
    ULONG magic,version;
    struct Resident *resident;
    struct rc2_store **store_pointer;
    ULONG *last_boot_result;
    APTR add_entry,open_entry,begin_entry,abort_entry;
};
#endif
