/* SPDX-License-Identifier: MIT
 * aminesiac RC4 dynamic-COW one-shot installer / status (DOS-facing, NOT resident).
 * Loaded module owns all runtime code; installer may exit without losing it.
 * NEVER run on original microSD. This rebrand requires independent hardware tests.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <exec/devices.h>
#include <devices/trackdisk.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "rc2_protocol.h"
#include "rc2_driver_compat.h"
#include "rc3_memory_layout.h"
#define RC2_INSTALL_MAGIC 0x52433448UL /* RC4H; reject prior installer header */
#define RC2_MAX_ENTRIES RC2_KICK_MAX_ENTRIES
#define RC2_LINK_MAX 64
#define RC2_HASH 2166136261UL
#define RC2_OPEN_LVO (-444L)
#define RC2_ADD_LVO (-432L)
struct rc2_memlist {
    struct Node ml_Node;UWORD ml_NumEntries;
    struct MemEntry ml_ME[RC2_MAX_ENTRIES];
};
struct rc2_header {
    struct rc2_memlist mem;
    ULONG magic,version,self,bytes,prev_mem,prev_tag;
    ULONG tags[3];
    char name[24];
    struct rc2_store state;
};
#ifdef __m68k__
typedef char rc2_ptr_32[(sizeof(void*)==4)?1:-1];
typedef char rc2_mem_off[(offsetof(struct rc2_memlist,ml_ME)==offsetof(struct MemList,ml_ME))?1:-1];
#endif
static ULONG addr(const void*p){return (ULONG)(uintptr_t)p;}
static int checksum_valid(struct ExecBase*s){return
  (!s->KickMemPtr&&!s->KickTagPtr)||addr(s->KickCheckSum)==SumKickData();}
static int unit_open(ULONG n,struct MsgPort**port,struct IOStdReq**io){
    *port=CreateMsgPort();*io=NULL;if(!*port)return 0;
    *io=(struct IOStdReq*)CreateIORequest(*port,sizeof(struct IOStdReq));
    if(!*io){DeleteMsgPort(*port);*port=NULL;return 0;}
    if(OpenDevice((STRPTR)RC2_DEVICE,n,(struct IORequest*)*io,0)){
        DeleteIORequest((struct IORequest*)*io);DeleteMsgPort(*port);
        *port=NULL;*io=NULL;return 0;}
    return 1;
}
static void unit_close(struct MsgPort*p,struct IOStdReq*io){
    if(io){CloseDevice((struct IORequest*)io);DeleteIORequest((struct IORequest*)io);}
    if(p)DeleteMsgPort(p);
}
static int physical_read(struct IOStdReq*io,uint64_t off,ULONG len,UBYTE*dst){
    int rc;io->io_Command=0xc000;io->io_Data=dst;io->io_Length=len;
    io->io_Offset=(ULONG)off;io->io_Actual=(ULONG)(off>>32);io->io_Error=0;
    rc=DoIO((struct IORequest*)io);
    return !rc&&!io->io_Error&&io->io_Actual==len;
}
static ULONG fingerprint(const UBYTE*data,ULONG n,ULONG h){
    ULONG i;for(i=0;i<n;i++){h^=data[i];h*=16777619UL;}return h;
}
static int audit(struct IOStdReq*u0,struct IOStdReq*u1,
                 struct am_partition*part,uint64_t*total,ULONG*hash){
    struct DriveGeometry d0,d1;
    static const ULONG samples[]={0,1,4,8,32,128,4096,8192};
    UBYTE *data,*mbr,*a,*b;ULONG i,j,h=RC2_HASH;uint64_t off;
    int ok=0;
    if(u0->io_Device!=u1->io_Device||u0->io_Unit==u1->io_Unit)return 0;
    data=(UBYTE*)AllocMem(1536,MEMF_PUBLIC|MEMF_CLEAR);
    if(!data)return 0;
    mbr=data;a=data+512;b=data+1024;
    memset(&d0,0,sizeof(d0));memset(&d1,0,sizeof(d1));
    u0->io_Command=TD_GETGEOMETRY;u0->io_Data=&d0;u0->io_Length=sizeof(d0);
    if(DoIO((struct IORequest*)u0)||u0->io_Error||d0.dg_SectorSize!=512||!d0.dg_TotalSectors)goto out;
    u1->io_Command=TD_GETGEOMETRY;u1->io_Data=&d1;u1->io_Length=sizeof(d1);
    if(DoIO((struct IORequest*)u1)||u1->io_Error||d1.dg_SectorSize!=512||!d1.dg_TotalSectors)goto out;
    *total=(uint64_t)d0.dg_TotalSectors;
    if(!physical_read(u0,0,512,mbr)||!am_parse_single_76(mbr,*total,part)||
       d1.dg_TotalSectors!=part->count||part->count<=8192)goto out;
    h=fingerprint(mbr,512,h);
    for(i=0;i<sizeof(samples)/sizeof(samples[0]);i++){
        if(!am_translate(part,1,(uint64_t)samples[i]*512,512,*total,&off)||
           !physical_read(u0,off,512,a)||
           !physical_read(u1,(uint64_t)samples[i]*512,512,b))goto out;
        for(j=0;j<512;j++)if(a[j]!=b[j])goto out;
        h=fingerprint(a,512,h);
    }
    *hash=h;ok=1;
out: FreeMem(data,1536);return ok;
}
static struct rc2_header*find_header(struct ExecBase*s){
    struct MemList*m=(struct MemList*)s->KickMemPtr;unsigned i;
    if(!checksum_valid(s))return NULL;
    for(i=0;m&&i<RC2_LINK_MAX;i++){
        struct rc2_header*h;
        if(!(TypeOfMem(m)&MEMF_CHIP))return NULL;
        if(m->ml_Node.ln_Type==NT_KICKMEM&&m->ml_Node.ln_Name&&
           !strcmp(m->ml_Node.ln_Name,RC2_KICK_NAME)){
            h=(struct rc2_header*)m;
            if(h->magic==RC2_INSTALL_MAGIC&&h->version==RC2_VERSION&&
              h->self==addr(h)&&h->bytes==sizeof(*h)&&
              h->state.magic==RC2_MAGIC&&h->state.self==addr(&h->state))return h;
            return NULL;
        }
        m=(struct MemList*)m->ml_Node.ln_Succ;
    }
    return NULL;
}
/* Never coexist with legacy RC2/RC3 residents or an already installed
 * aminesiac resident. Preserve old internal names ONLY for refusal checks. */
static int resident_conflict(struct ExecBase *sys){
    struct MemList *m=(struct MemList*)sys->KickMemPtr;
    unsigned i;
    if(FindResident((STRPTR)RC2_NAME)||
       FindResident((STRPTR)"ShadowSD.RC2")||
       FindResident((STRPTR)"ShadowSD.RC3")||
       FindResident((STRPTR)"aminesiac.RC3"))return 1;
    for(i=0;m&&i<RC2_LINK_MAX;i++){
        const char *name=m->ml_Node.ln_Name;
        if(!TypeOfMem(m))return 1;
        if(name&&(!strcmp(name,"ShadowSD.RC2.Kick")||
                   !strcmp(name,"ShadowSD.RC3.Kick")||
                   !strcmp(name,RC2_KICK_NAME)||
                   !strcmp(name,"aminesiac.RC3.Kick")))return 1;
        m=(struct MemList*)m->ml_Node.ln_Succ;
    }
    return m!=NULL; /* truncated chain: refuse instead of assuming safe */
}
/* Status is deliberately read-only. We inspect the installed JMP vectors,
 * not just persistent bookkeeping, because a worker READY marker does not
 * establish that device I/O is actually intercepted. */
static ULONG vector_target(struct Library*lib,LONG lvo){
    UBYTE *v;
    if(!lib||lib->lib_NegSize<(UWORD)(-lvo))return 0;
    v=(UBYTE*)lib+lvo;
    if(*(volatile UWORD*)v!=0x4ef9U)return 0;
    return *(volatile ULONG*)(v+2);
}
static struct Device*find_brcm_device(struct ExecBase*s){
    struct Node*n;
    for(n=s->DeviceList.lh_Head;n&&n->ln_Succ;n=n->ln_Succ)
        if(n->ln_Name&&!strcmp(n->ln_Name,RC2_DEVICE))return (struct Device*)n;
    return NULL;
}
static void status(void){
    struct ExecBase*s=*(struct ExecBase**)4;
    struct rc2_header*h=find_header(s);struct rc2_store*p;
    ULONG open_now,add_now,begin_now=0,abort_now=0;
    ULONG actual_device=0;UWORD driver_version=0,driver_revision=0;
    if(!h){puts("[STATUS] RC4 not found in KickMem or checksum invalid");return;}
    p=&h->state;
    if(p->kick_mem!=(struct MemList*)&h->mem ||
       p->kick_max_entries!=RC2_MAX_ENTRIES){
        puts("[STATUS] CORRUPT: persistent KickMem header pointer/count mismatch");return;
    }
    Forbid();
    open_now=vector_target((struct Library*)s,RC2_OPEN_LVO);
    add_now=vector_target((struct Library*)s,RC2_ADD_LVO);
    {
        struct Device*d=find_brcm_device(s);
        if(d){actual_device=addr(d);
            driver_version=d->dd_Library.lib_Version;driver_revision=d->dd_Library.lib_Revision;
            begin_now=vector_target((struct Library*)d,RC2_BEGIN_LVO);
            abort_now=vector_target((struct Library*)d,RC2_ABORT_LVO);
        }
    }
    Permit();
    printf("[KICK] header=%08lx KickMem=%08lx KickTag=%08lx checksum=%08lx actual=%08lx\n",
        addr(h),addr(s->KickMemPtr),addr(s->KickTagPtr),addr(s->KickCheckSum),SumKickData());
    printf("[RESIDENT] boots=%lu arms=%lu worker=%08lx faults=%lu\n",
        p->boot_count,p->armed_count,p->worker_ready,p->fail_code);
    printf("[COW] used=%lu/%lu sectors allocated=%lu/%lu blocks (%lu payload MiB)\n",
        p->cow.used,p->cow.capacity,p->cow.allocated_blocks,
        p->capacity/RC_DYN_SECTORS_PER_BLOCK,p->cow.allocated_blocks);
    printf("[COW] writes=%lu reads=%lu changes=%lu busy=%08lx full=%lu\n",
        p->cow.writes,p->cow.reads,p->change_count,p->busy,p->cow.full);
    printf("[KICK] entries=%u base=%lu checksum=%s\n",
        (unsigned)p->kick_mem->ml_NumEntries,p->kick_base_entries,
        checksum_valid(s)?"PASS":"FAIL");
    printf("[GATE] Exec AddDevice=%08lx expected=%08lx %s\n",
        add_now,p->gate_add,add_now==p->gate_add?"PASS":"FAIL");
    printf("[GATE] Exec OpenDevice=%08lx expected=%08lx %s\n",
        open_now,p->gate_open,open_now==p->gate_open?"PASS":"FAIL");
    printf("[GATE] brcm device=%08lx BeginIO=%08lx expected=%08lx %s\n",
        actual_device,begin_now,p->gate_begin,
        actual_device&&begin_now==p->gate_begin?"PASS":"FAIL");
    printf("[GATE] brcm AbortIO=%08lx expected=%08lx %s\n",
        abort_now,p->gate_abort,
        actual_device&&abort_now==p->gate_abort?"PASS":"FAIL");
    printf("[DRIVER] brcm-sdhc.device %u.%u (version is diagnostic only)\n",
        (unsigned)driver_version,(unsigned)driver_revision);
    printf("[MEDIA] sectors=%lu fingerprint=%08lx partition=%lu+%lu\n",
        (ULONG)p->cow.disk_sectors,p->disk_fingerprint,
        (ULONG)p->part.start,(ULONG)p->part.count);
    if(add_now==p->gate_add&&open_now==p->gate_open&&
       actual_device&&begin_now==p->gate_begin&&abort_now==p->gate_abort&&
       p->worker_ready==RC2_READY&&!p->fail_code&&
       p->integrity==RC2_CLEAN&&p->busy==0&&checksum_valid(s))
        puts("[STATUS] ACTIVE-GATED: vectors match; physical isolation NOT independently proven");
    else puts("[STATUS] NOT VERIFIED: DO NOT TRUST THIS SESSION FOR WRITE ISOLATION");
    puts("[POLICY] Power OFF discards RAM overlay. Never use irreplaceable media.");
}
static ULONG read_mb(const char*s){unsigned long v;char*end;
    if(strncmp(s,"MEM=",4))return 0;
    v=strtoul(s+4,&end,10);
    return !*end&&v>=RC3_MEM_MIN_MB&&v<=RC3_MEM_MAX_MB?(ULONG)v:0;
}
/* PROBE has no SetFunction, no RAM registration and no write command.
 * It requires an unarmed system: inspecting a live overlay via DoIO would
 * NOT establish the state of the physical medium. */
static int probe(void){
    struct ExecBase *sys=*(struct ExecBase**)4;
    struct MsgPort *p0=NULL,*p1=NULL;
    struct IOStdReq *io0=NULL,*io1=NULL;
    struct am_partition part;uint64_t total=0;ULONG hash=0,b=0,a=0;
    int rc=20;
    if(!checksum_valid(sys)||resident_conflict(sys)){
        puts("[PROBE] REFUSED: invalid Kick checksum or existing aminesiac/legacy resident; power-cycle first");return 10;
    }
    if(!unit_open(0,&p0,&io0)||!unit_open(1,&p1,&io1)){
        puts("[PROBE] FAIL: cannot open both brcm units");goto done;
    }
    if(!rc2_driver_prepatch((struct Device*)io0->io_Device,&b,&a)){
        puts("[PROBE] FAIL: incompatible brcm-sdhc Exec vector layout");goto done;
    }
    printf("[DRIVER] brcm-sdhc.device %u.%u, BeginIO=%08lx AbortIO=%08lx\n",
        (unsigned)((struct Device*)io0->io_Device)->dd_Library.lib_Version,
        (unsigned)((struct Device*)io0->io_Device)->dd_Library.lib_Revision,b,a);
    if(!audit(io0,io1,&part,&total,&hash)){
        puts("[PROBE] FAIL: read-only 0x76 unit0/unit1 media alias audit");goto done;
    }
    printf("[MEDIA] sectors=%lu partition=%lu+%lu sample-hash=%08lx\n",
        (ULONG)total,(ULONG)part.start,(ULONG)part.count,hash);
    puts("[PROBE] PASS: read-only compatibility checks. No hooks installed.");
    puts("[PROBE] This does NOT prove physical write isolation or warm-reset recovery.");
    rc=0;
done:
    unit_close(p1,io1);unit_close(p0,io0);return rc;
}
static int install(ULONG mem_mb,const char*module){
    struct ExecBase*sys=*(struct ExecBase**)4;
    struct MsgPort*p0=NULL,*p1=NULL,*q0=NULL,*q1=NULL;
    struct IOStdReq*io0=NULL,*io1=NULL,*warm0=NULL,*warm1=NULL;
    struct rc2_header*h=NULL;struct rc2_exports*exp=NULL;
    BPTR seg=0,it;struct MemEntry*e;
    APTR slots=NULL,oldmem,oldtag;
    uint64_t total=0;struct am_partition part;ULONG hash=0;
    ULONG capacity,buckets,block_bytes=0,slot_bytes=0,n=0;
    struct rc3_arena_layout layout;
    int rc=20,registered=0;
    if(sys->LibNode.lib_Version<39||!checksum_valid(sys)||resident_conflict(sys)){
       puts("[INSTALL] REFUSED: Exec version, Kick checksum or existing aminesiac/legacy resident");return 10;}
    if(!unit_open(0,&p0,&io0)||!unit_open(1,&p1,&io1)){
       puts("[INSTALL] REFUSED: cannot open both brcm units");goto fail;}
    { ULONG b=0,a=0;
      if(!rc2_driver_prepatch((struct Device*)io0->io_Device,&b,&a)||
         !audit(io0,io1,&part,&total,&hash)){
          puts("[INSTALL] REFUSED: Exec driver vectors or read-only unit alias audit failed");goto fail;
      }
      printf("[DRIVER] version=%u.%u BeginIO=%08lx AbortIO=%08lx: portable probe PASS\n",
          (unsigned)((struct Device*)io0->io_Device)->dd_Library.lib_Version,
          (unsigned)((struct Device*)io0->io_Device)->dd_Library.lib_Revision,b,a);
    }
    if(!rc3_arena_layout(mem_mb,&layout)){
       puts("[INSTALL] REFUSED: unsupported COW memory size");goto fail;}
    capacity=layout.capacity;buckets=layout.buckets;
    block_bytes=layout.block_bytes;slot_bytes=layout.slot_bytes;
    printf("[MEM] dynamic maximum=%lu payload MiB; sectors=%lu; hash=%lu buckets\n",
           mem_mb,capacity,buckets);
    printf("[MEM] initial FAST+KICK=%lu bytes (hash only); COW grows in %lu-byte blocks\n",
           slot_bytes,block_bytes);
    h=(struct rc2_header*)AllocMem(sizeof(*h),MEMF_CHIP|MEMF_PUBLIC|MEMF_CLEAR);
    slots=AllocMem(slot_bytes,MEMF_FAST|MEMF_KICK|MEMF_CLEAR);
    if(!h||!slots){puts("[INSTALL] FAIL: CHIP / FAST+KICK allocation");goto fail;}
    if(!(TypeOfMem(h)&MEMF_CHIP)||
       (TypeOfMem(slots)&(MEMF_FAST|MEMF_KICK))!=(MEMF_FAST|MEMF_KICK)){
       puts("[INSTALL] FAIL: allocation memory attributes");goto fail;}
    seg=LoadSeg((STRPTR)module);
    if(!seg){puts("[INSTALL] FAIL: cannot LoadSeg the standalone resident module");goto fail;}
    /* First HUNK MUST start with _start in entry.S; returns relocated exports. */
    exp=((struct rc2_exports*(*)(void))((UBYTE*)BADDR(seg)+4))();
    if(!exp||exp->magic!=RC2_MAGIC||exp->version!=RC2_VERSION||
       !exp->resident||!exp->store_pointer||!exp->last_boot_result||
       !exp->add_entry||!exp->open_entry||!exp->begin_entry||!exp->abort_entry||
       !exp->resident->rt_Name||
       strcmp((const char*)exp->resident->rt_Name,RC2_NAME)||
       exp->resident->rt_MatchWord!=RTC_MATCHWORD||
       exp->resident->rt_MatchTag!=exp->resident||
       exp->resident->rt_Pri!=0||exp->resident->rt_Flags!=RTF_COLDSTART){
       puts("[INSTALL] FAIL: resident export ABI mismatch");goto fail;}
    h->magic=RC2_INSTALL_MAGIC;h->version=RC2_VERSION;
    h->self=addr(h);h->bytes=sizeof(*h);
    strcpy(h->name,RC2_KICK_NAME);
    h->mem.ml_Node.ln_Name=h->name;h->mem.ml_Node.ln_Type=NT_KICKMEM;
    e=h->mem.ml_ME;
    e[n].me_Un.meu_Addr=h;e[n++].me_Length=sizeof(*h);
    e[n].me_Un.meu_Addr=slots;e[n++].me_Length=slot_bytes;
    for(it=seg;it;it=*(BPTR*)BADDR(it)){
        UBYTE*segment=(UBYTE*)BADDR(it);
        ULONG len=*(ULONG*)(segment-4);APTR base=segment-4;
        if(n-2>=RC2_MAX_HUNKS||n>=RC2_MAX_ENTRIES||len<12||
           !(TypeOfMem(base)&MEMF_CHIP)||
           !((ULONG)(uintptr_t)base<0x80000000UL)){
            puts("[INSTALL] FAIL: too many HUNKs or resident segment not in CHIP");goto fail;
        }
        e[n].me_Un.meu_Addr=base;e[n++].me_Length=len;
    }
    h->mem.ml_NumEntries=(UWORD)n;
    h->state.magic=RC2_MAGIC;h->state.version=RC2_VERSION;
    h->state.self=addr(&h->state);h->state.bytes=sizeof(h->state);
    h->state.capacity=capacity;h->state.buckets=buckets;
    h->state.block_bytes=block_bytes;h->state.slot_bytes=slot_bytes;
    h->state.integrity=RC2_CLEAN;
    h->state.disk_fingerprint=hash;h->state.disk_fingerprint_inverse=~hash;
    h->state.busy=0;h->state.busy_inverse=~(ULONG)0;
    h->state.gate_add=addr(exp->add_entry);
    h->state.gate_open=addr(exp->open_entry);
    h->state.gate_begin=addr(exp->begin_entry);
    h->state.gate_abort=addr(exp->abort_entry);
    h->state.part=part;
    h->state.cow.slots=(uint32_t*)slots;
    h->state.cow.capacity=capacity;h->state.cow.buckets=buckets;
    h->state.kick_mem=(struct MemList*)&h->mem;
    h->state.kick_base_entries=n;
    h->state.kick_max_entries=RC2_MAX_ENTRIES;
    h->state.cow.disk_sectors=total;
    *exp->store_pointer=&h->state;
    h->tags[0]=addr(exp->resident);h->tags[2]=0;
    if(!((ULONG)(uintptr_t)exp->resident<0x80000000UL)){
       puts("[INSTALL] FAIL: ROMTag outside linkable address");goto fail;}
    Forbid();Disable();
    if(!checksum_valid(sys)||resident_conflict(sys)){
       Enable();Permit();puts("[INSTALL] FAIL: Kick chains changed concurrently");goto fail;}
    oldmem=sys->KickMemPtr;oldtag=sys->KickTagPtr;
    if(oldtag&&(ULONG)(uintptr_t)oldtag>=0x80000000UL){
       Enable();Permit();puts("[INSTALL] FAIL: existing KickTag un-linkable");goto fail;}
    h->prev_mem=addr(oldmem);h->prev_tag=addr(oldtag);
    h->mem.ml_Node.ln_Succ=(struct Node*)oldmem;
    h->tags[1]=oldtag?(addr(oldtag)|0x80000000UL):0;
    sys->KickMemPtr=(APTR)&h->mem;
    sys->KickTagPtr=(APTR)h->tags;
    sys->KickCheckSum=(APTR)(uintptr_t)SumKickData();
    CacheClearU();registered=1;
    Enable();Permit();
    printf("[KICK] registered CHIP=%08lx FAST-slots=%08lx hunks=%lu reserved descriptors=%lu\n",
       addr(h),addr(slots),n-2,(ULONG)(RC2_MAX_ENTRIES-n));
    printf("[KICK] checksum=%08lx actual=%08lx; ROMTag pri=0\n",
       addr(sys->KickCheckSum),SumKickData());
    /* INITIAL arm in this boot: same callback Exec calls automatically on chord. */
    InitResident(exp->resident,0);
    printf("[COLDSTART] init result=%lu\n",*exp->last_boot_result);
    if(*exp->last_boot_result){puts("[INSTALL] WARNING: bootstrap failed; POWER OFF to recover");goto fail;}
    /* Reopen to engage Exec OpenDevice interception in current Exec session. */
    if(!unit_open(0,&q0,&warm0)||!unit_open(1,&q1,&warm1)){
        puts("[INSTALL] WARNING: hook did not observe both units");goto fail;}
    {
        UBYTE block[512];
        if(!physical_read(warm0,0,512,block)||h->state.worker_ready!=RC2_READY){
            puts("[INSTALL] WARNING: resident worker / media audit NOT READY");goto fail;
        }
    }
    puts("[ARMED] RC4 dynamic COW resident installed. DO NOT re-run ARM after chord.");
    puts("[ARMED] New boot restores the COW map automatically before brcm device I/O opened via Exec.");
    puts("[POLICY] Changes are RAM-only across chord; disappear on FULL power OFF.");
    rc=0;
fail:
    unit_close(q1,warm1);unit_close(q0,warm0);
    unit_close(p1,io1);unit_close(p0,io0);
    if(!registered){
       if(seg)UnLoadSeg(seg);
       if(slots)FreeMem(slots,slot_bytes);
       if(h)FreeMem(h,sizeof(*h));
    }else if(rc){
       puts("[CRITICAL] Persistent registration already committed. Do NOT unload it.");
       puts("[CRITICAL] Stop all SD writes and perform FULL power-off on a CLONE.");
    }
    return rc;
}
int main(int argc,char**argv){
    puts("aminesiac RC4 / DYNAMIC COW / SOURCE CANDIDATE (NOT HARDWARE VALIDATED)");
    if(argc==2&&!strcmp(argv[1],"STATUS")){status();return 0;}
    if(argc==2&&!strcmp(argv[1],"PROBE"))return probe();
    if(argc==4&&!strcmp(argv[1],"ARM")&&!strcmp(argv[3],"I_ACCEPT_RISK")){
        ULONG mb=read_mb(argv[2]);
        if(mb)return install(mb,"aminesiac.R4Module");
        puts("[ARM] Invalid MEM value. Supported: MEM=4 through MEM=128 (integer MiB).");
        return 10;
    }
    puts("Usage: aminesiac PROBE | ARM MEM=N I_ACCEPT_RISK | STATUS (N=4..128 MiB)");
    puts("Requires aminesiac.R4Module in current directory, compatible brcm-sdhc Exec vectors,");
    puts("single 0x76 MBR partition, CLONED test media. No DISARM. No Emu68 changes.");
    return 10;
}
