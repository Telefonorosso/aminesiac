/* SPDX-License-Identifier: MIT
 * aminesiac RC3 resident kernel, HUNK-loaded, all CODE/DATA/BSS reserved in CHIP.
 * No libc, no DOS, no Emu68 modifications. Uses Exec only on warm boot.
 * SAFETY: Rebranded binary requires its own hardware validation. Use cloned test media.
 */
#include <stdint.h>
#include <stddef.h>
#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/tasks.h>
#include <exec/devices.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <exec/memory.h>
#include <devices/trackdisk.h>
#include <proto/exec.h>
#include "rc2_protocol.h"
#include "rc2_driver_compat.h"

#define RC2_PENDING 512U
#define RC2_SIGNAL 18
#define RC2_IO_SIGNAL 19
#define RC2_STACK 16384UL
#define RC2_OPEN_LVO (-444L)
#define RC2_ADD_LVO (-432L)
#define RC2_FNV 2166136261UL
#define RC2_BAD (-5)
#define RC2_BUSY (-6)
#define RC2_ABORTED (-2)

struct ExecBase *SysBase; /* Rebound on EVERY COLDSTART. No preserved Exec ptr. */
struct rc2_store *rc2_store_pointer;
ULONG rc2_last_boot_result;
APTR rc2_previous_open;
APTR rc2_previous_add;
APTR rc2_previous_begin;
APTR rc2_previous_abort;
extern void rc2_init_entry(void),rc2_open_entry(void),rc2_begin_entry(void),rc2_abort_entry(void),rc2_add_entry(void);

static const char rc2_name[] = RC2_NAME;
static const char rc2_id[] = "aminesiac RC3 portable SD / COLDSTART";
struct Resident rc2_resident = {
    RTC_MATCHWORD, &rc2_resident, (APTR)(&rc2_resident+1),
    RTF_COLDSTART,2,NT_UNKNOWN,0,
    (STRPTR)rc2_name,(STRPTR)rc2_id,(APTR)rc2_init_entry
};
struct rc2_exports rc2_exports={RC2_MAGIC,RC2_VERSION,&rc2_resident,
                               &rc2_store_pointer,&rc2_last_boot_result,
                               (APTR)rc2_add_entry,(APTR)rc2_open_entry,
                               (APTR)rc2_begin_entry,(APTR)rc2_abort_entry};
struct rc2_pending {struct IOStdReq *req;UBYTE abort,phase;};
struct rc2_live {
    struct rc2_store *store;
    struct Device *dev;
    struct Unit *unit0,*unit1;
    struct Task *task;
    struct MsgPort work,private_port;
    struct IOStdReq *io0,*io1;
    struct rc2_pending pending[RC2_PENDING];
    ULONG pending_used,queued,completed;
    UBYTE patched,failed,ready,seen,worker_live;
    UBYTE reserved[3];
};
static struct rc2_live g;

static void rc2_zero(void *x,ULONG n){UBYTE *p=(UBYTE*)x;while(n--)*p++=0;}
/* Private libc-free copies required by cow.h (resident never calls C runtime). */
void *memcpy(void *dst,const void *src,size_t n){
    UBYTE*d=(UBYTE*)dst;const UBYTE*s=(const UBYTE*)src;
    while(n--) *d++=*s++;
    return dst;
}
void *memset(void *dst,int c,size_t n){
    UBYTE*d=(UBYTE*)dst;while(n--)*d++=(UBYTE)c;return dst;
}
static int rc2_equal(const char*a,const char*b){if(!a||!b)return 0;
    while(*a&&*a==*b){a++;b++;}return *a==*b;}
static ULONG rc2_fingerprint(const UBYTE*x,ULONG n,ULONG h){
    ULONG i;for(i=0;i<n;i++){h^=x[i];h*=16777619UL;}return h;
}
static ULONG rc2_addr(const void *p){return (ULONG)(uintptr_t)p;}
static int rc2_valid_store(struct rc2_store *p){
    return p&&p->magic==RC2_MAGIC&&p->version==RC2_VERSION&&
      p->self==rc2_addr(p)&&p->bytes==sizeof(*p)&&p->integrity==RC2_CLEAN&&
      p->capacity&&!(p->capacity&(p->capacity-1))&&
      p->buckets==2*p->capacity&&
      p->cow.capacity==p->capacity&&p->cow.buckets==p->buckets&&
      p->cow.entries&&p->cow.slots&&p->cow.used<=p->capacity&&
      p->cow.disk_sectors&&
      p->busy==0&&p->busy_inverse==~(ULONG)0&&
      (p->disk_fingerprint==0||p->disk_fingerprint_inverse==~p->disk_fingerprint);
}
static int rc2_driver_ok(struct Device*d){
    /* Executed again by the worker at each warm boot; no known address,
     * version, git ID, internal struct offset or fingerprint dependency. */
    return g.patched&&d==g.dev&&
      rc2_driver_postpatch(d,rc2_addr(rc2_previous_begin),
                             rc2_addr(rc2_previous_abort),
                             rc2_addr((APTR)rc2_begin_entry),
                             rc2_addr((APTR)rc2_abort_entry));
}
static void rc2_port_init(struct MsgPort *p,struct Task *task,UBYTE sig){
    rc2_zero(p,sizeof(*p));
    p->mp_Node.ln_Type=NT_MSGPORT;p->mp_Flags=PA_SIGNAL;
    p->mp_SigBit=sig;p->mp_SigTask=task;
    p->mp_MsgList.lh_Head=(struct Node*)&p->mp_MsgList.lh_Tail;
    p->mp_MsgList.lh_Tail=NULL;
    p->mp_MsgList.lh_TailPred=(struct Node*)&p->mp_MsgList.lh_Head;
}
static int rc2_index(struct IOStdReq *r){
    ULONG i;
    for(i=0;i<RC2_PENDING;i++){
        if(g.pending[i].req==r)return (int)i;
    }
    return -1;
}
static int rc2_reserve(struct IOStdReq *r){ULONG i;if(rc2_index(r)>=0)return -1;
    for(i=0;i<RC2_PENDING;i++)if(!g.pending[i].req){
        g.pending[i].req=r;g.pending[i].abort=0;g.pending[i].phase=1;g.pending_used++;return (int)i;}
    return -1;
}
static void rc2_reply(struct IOStdReq*r,int err,ULONG length){int i;
    Forbid();i=rc2_index(r);
    if(i>=0){if(g.pending[i].abort){err=RC2_ABORTED;length=0;
        if(g.store)g.store->cow.aborted++;}
        g.pending[i].req=NULL;g.pending[i].phase=0;g.pending[i].abort=0;g.pending_used--;}
    r->io_Error=(BYTE)err;r->io_Actual=err?0:length;
    if(err&&g.store)g.store->cow.errors++;
    g.completed++;ReplyMsg(&r->io_Message);Permit();
}
static int rc2_failfast(struct IOStdReq *r,int error){
    r->io_Error=(BYTE)error;r->io_Actual=0;
    if(!(r->io_Flags&IOF_QUICK))ReplyMsg(&r->io_Message);
    return 1;
}
#include "rc2_command_policy.h"

/* Called by resident patch, from arbitrary BeginIO context; must NEVER block. */
int rc2_dispatch(struct IOStdReq*r){int idx;UWORD c;
    if(!r||r==g.io0||r==g.io1)return 0;
    c=r->io_Command;
    /* A brcm unit that has not yet been observed must never expose an
     * unclassified write command to the physical medium. Allow only known
     * non-writing controls; refuse every other command with a proper I/O
     * completion, including unrecognized FORMAT/WRITE64/SCSI variants. */
    if((!g.unit0 || r->io_Unit!=g.unit0) &&
       (!g.unit1 || r->io_Unit!=g.unit1)){
        if(rc2_safe_control(c))return 0;
        if(g.store)g.store->cow.blocked++;
        return rc2_failfast(r,RC2_BUSY);
    }
    /* Preserve the native semantics of read-only control and disk-change
     * notifications; in particular the original driver owns the outstanding
     * TD_ADDCHANGEINT request until matching TD_REMCHANGEINT. */
    if(rc2_safe_control(c))return 0;
    if(!rc2_cow_command(c)){
        if(g.store)g.store->cow.blocked++;
        return rc2_failfast(r,RC_NOCMD);
    }
    /* During a normal live write busy=DIRTY temporarily: do not reject the
       concurrently arriving request; the worker serializes it safely. */
    if(!g.task||g.failed||!g.store||
       g.store->magic!=RC2_MAGIC||g.store->integrity!=RC2_CLEAN)
        return rc2_failfast(r,RC2_BUSY);
    Forbid();idx=rc2_reserve(r);
    if(idx<0){Permit();return rc2_failfast(r,RC2_BUSY);}
    r->io_Flags=(UBYTE)(r->io_Flags&~IOF_QUICK);
    r->io_Error=0;PutMsg(&g.work,&r->io_Message);g.queued++;
    Permit();return 1;
}
int rc2_abort(struct IOStdReq*r){int idx;if(!r)return -1;
    Forbid();idx=rc2_index(r);
    if(idx>=0){g.pending[idx].abort=1;Permit();return 0;}
    Permit();return -1;
}
/* Private I/O always falls through to original physical driver. */
static int rc2_read(struct IOStdReq*io,uint64_t off,ULONG len,UBYTE*data){
    int rc;io->io_Command=0xc000;io->io_Data=data;io->io_Length=len;
    io->io_Offset=(ULONG)off;io->io_Actual=(ULONG)(off>>32);io->io_Error=0;
    rc=DoIO((struct IORequest*)io);
    return !rc&&!io->io_Error&&io->io_Actual==len;
}
static int rc2_audit(void){
    struct DriveGeometry d0,d1;
    static const ULONG samples[]={0,1,4,8,32,128,4096,8192};
    UBYTE data[1536],*mbr=data,*a=data+512,*b=data+1024;
    ULONG i,h=RC2_FNV;uint64_t physical,total;
    struct am_partition part;
    struct rc2_store*p=g.store;
    if(!g.io0||!g.io1||g.io0->io_Device!=g.io1->io_Device||
       g.io0->io_Unit==g.io1->io_Unit)return 0;
    rc2_zero(&d0,sizeof(d0));rc2_zero(&d1,sizeof(d1));
    g.io0->io_Command=TD_GETGEOMETRY;g.io0->io_Data=&d0;g.io0->io_Length=sizeof(d0);
    if(DoIO((struct IORequest*)g.io0)||g.io0->io_Error||d0.dg_SectorSize!=512||!d0.dg_TotalSectors)return 0;
    g.io1->io_Command=TD_GETGEOMETRY;g.io1->io_Data=&d1;g.io1->io_Length=sizeof(d1);
    if(DoIO((struct IORequest*)g.io1)||g.io1->io_Error||d1.dg_SectorSize!=512||!d1.dg_TotalSectors)return 0;
    total=(uint64_t)d0.dg_TotalSectors;
    if(!rc2_read(g.io0,0,512,mbr)||!am_parse_single_76(mbr,total,&part)||
       d1.dg_TotalSectors!=part.count||d1.dg_TotalSectors<=8192)return 0;
    h=rc2_fingerprint(mbr,512,h);
    for(i=0;i<sizeof(samples)/sizeof(samples[0]);i++){
        ULONG j;
        if(!am_translate(&part,1,(uint64_t)samples[i]*512,512,total,&physical)||
           !rc2_read(g.io0,physical,512,a)||
           !rc2_read(g.io1,(uint64_t)samples[i]*512,512,b))return 0;
        for(j=0;j<512;j++)if(a[j]!=b[j])return 0;
        h=rc2_fingerprint(a,512,h);
    }
    if(p->disk_fingerprint){
        if(p->disk_fingerprint!=h||p->cow.disk_sectors!=total||
            p->part.start!=part.start||p->part.count!=part.count)return 0;
    }else{
        if(p->cow.used)return 0;
        p->disk_fingerprint=h;p->disk_fingerprint_inverse=~h;
        p->cow.disk_sectors=total;p->part=part;
    }
    return 1;
}
static int rc2_open_private(ULONG unit,struct IOStdReq **out){
    struct IOStdReq *io=(struct IOStdReq*)AllocMem(sizeof(*io),MEMF_PUBLIC|MEMF_CLEAR);
    if(!io)return 0;
    io->io_Message.mn_ReplyPort=&g.private_port;
    io->io_Message.mn_Node.ln_Type=NT_MESSAGE;
    io->io_Message.mn_Length=sizeof(*io);
    if(OpenDevice((STRPTR)RC2_DEVICE,unit,(struct IORequest*)io,0)){
        FreeMem(io,sizeof(*io));return 0;
    }
    *out=io;return 1;
}
static void rc2_worker_setup(void){
    if(g.ready||g.failed)return;
    if(!rc2_open_private(0,&g.io0)||!rc2_open_private(1,&g.io1)||
        !rc2_driver_ok((struct Device*)g.io0->io_Device)||!rc2_audit()){
        g.failed=1;g.store->worker_ready=RC2_FAULT;g.store->fail_code=3;return;
    }
    g.ready=1;g.store->worker_ready=RC2_READY;g.store->armed_count++;
}
static void rc2_process(struct IOStdReq*r){
    int idx,unit,error=0;UWORD c=r->io_Command;ULONG len=r->io_Length;
    uint64_t logical,physical;struct rc2_store*p=g.store;
    if(!g.ready||g.failed){rc2_reply(r,RC2_BUSY,0);return;}
    Forbid();idx=rc2_index(r);
    if(idx>=0)g.pending[idx].phase=2;
    if(idx<0||g.pending[idx].abort){Permit();rc2_reply(r,RC2_ABORTED,0);return;}
    Permit();
    if(c==4||c==5||c==8){rc2_reply(r,0,0);return;}
    unit=r->io_Unit==g.unit1?1:0;
    logical=rc_decode_offset(c,(uint32_t)r->io_Actual,(uint32_t)r->io_Offset);
    if(!am_translate(&p->part,(uint8_t)unit,logical,len,p->cow.disk_sectors,&physical)
       ||!r->io_Data){rc2_reply(r,RC_BADADDRESS,0);return;}
    if(c==3||c==0xc001){
        /* Reset interrupt mid-write => DIRTY at next COLDSTART => refuse recovery. */
        p->busy=RC2_DIRTY;p->busy_inverse=~RC2_DIRTY;
        __asm__ __volatile__("" ::: "memory");
        error=rc_write(&p->cow,physical,len,r->io_Data);
        __asm__ __volatile__("" ::: "memory");
        p->busy=0;p->busy_inverse=~(ULONG)0;
        if(!error)p->change_count++;
        rc2_reply(r,error,error?0:len);
    }else{
        if(!rc_all_ram(&p->cow,physical,len)){
            if(!rc2_read(g.io0,physical,len,(UBYTE*)r->io_Data)){
                rc2_reply(r,g.io0->io_Error?g.io0->io_Error:RC_BADADDRESS,0);return;}
            p->cow.physical_reads++;
        }
        if(!rc_overlay(&p->cow,physical,len,r->io_Data)){
            rc2_reply(r,RC_BADADDRESS,0);return;}
        p->cow.reads++;rc2_reply(r,0,len);
    }
}
static void rc2_worker(void){struct IOStdReq*r;
    g.worker_live=1;
    for(;;){Wait(1UL<<RC2_SIGNAL);
        while((r=(struct IOStdReq*)GetMsg(&g.work))){
            if(!g.ready&&!g.failed)rc2_worker_setup();
            rc2_process(r);
        }
    }
}
static int rc2_make_worker(void){
    UBYTE *block;struct Task*t;
    ULONG size=(ULONG)sizeof(*t)+RC2_STACK+16;
    block=(UBYTE*)AllocMem(size,MEMF_PUBLIC|MEMF_CLEAR);
    if(!block)return 0;
    t=(struct Task*)block;
    t->tc_Node.ln_Type=NT_TASK;t->tc_Node.ln_Pri=0;
    t->tc_Node.ln_Name=(STRPTR)"aminesiac.RC3.worker";
    t->tc_SPLower=(APTR)(block+sizeof(*t));
    t->tc_SPUpper=(APTR)(block+size);
    t->tc_SPReg=(APTR)((ULONG)(uintptr_t)t->tc_SPUpper & ~3UL);
    t->tc_SigAlloc=(1UL<<RC2_SIGNAL)|(1UL<<RC2_IO_SIGNAL);
    t->tc_MemEntry.lh_Head=(struct Node*)&t->tc_MemEntry.lh_Tail;
    t->tc_MemEntry.lh_Tail=NULL;
    t->tc_MemEntry.lh_TailPred=(struct Node*)&t->tc_MemEntry.lh_Head;
    g.task=t;
    rc2_port_init(&g.work,t,RC2_SIGNAL);
    rc2_port_init(&g.private_port,t,RC2_IO_SIGNAL);
    if(!AddTask(t,(APTR)rc2_worker,NULL)){g.task=NULL;FreeMem(block,size);return 0;}
    return 1;
}
/* Installed as soon as brcm-sdhc.device is added to Exec's DeviceList.
 * It must NOT depend on task scheduling, filesystem startup or DOS. */
static void rc2_patch(struct Device*d){
    APTR b,a;
    ULONG before_begin=0,before_abort=0;
    int verified;
    if(g.patched||!d)return;
    Forbid();
    if(g.patched){Permit();return;}
    /* Do not use any driver-private fields or assumed firmware addresses.
     * Preserve fail-closed interception even if a previously unknown build
     * has an unexpected but patchable vector layout at warm reboot. */
    verified=rc2_driver_prepatch(d,&before_begin,&before_abort);
    if(!rc2_driver_identity(d)){
        g.failed=1;
        if(g.store){g.store->fail_code=31;g.store->worker_ready=RC2_FAULT;}
        Permit();return;
    }
    g.dev=d;
    a=SetFunction((struct Library*)d,RC2_ABORT_LVO,(APTR)rc2_abort_entry);
    b=SetFunction((struct Library*)d,RC2_BEGIN_LVO,(APTR)rc2_begin_entry);
    rc2_previous_abort=a;rc2_previous_begin=b;
    SumLibrary((struct Library*)d);CacheClearU();
    g.patched=1;
    if(!verified||before_begin!=rc2_addr(b)||before_abort!=rc2_addr(a)||
       !rc2_driver_ok(d)){
        g.failed=1;
        if(g.store){g.store->fail_code=31;g.store->worker_ready=RC2_FAULT;}
    }
    Permit();
}
void rc2_add_observe(struct Device*d){
    if(!d||!d->dd_Library.lib_Node.ln_Name)return;
    if(!rc2_equal(d->dd_Library.lib_Node.ln_Name,RC2_DEVICE))return;
    rc2_patch(d);
}
void rc2_open_observe(const char*name,ULONG unit,struct IOStdReq*io,LONG result){
    if(result||!io||!io->io_Device)return;
    /* OpenDevice may be called through a path alias: verify the actual node. */
    if(!rc2_equal(((struct Device*)io->io_Device)->dd_Library.lib_Node.ln_Name,RC2_DEVICE))return;
    (void)name;
    if(g.task&&FindTask(NULL)==g.task)return; /* our internal physical opens */
    if(unit>1)return;
    Forbid();
    if(unit==0)g.unit0=io->io_Unit;
    if(unit==1)g.unit1=io->io_Unit;
    g.seen|=(1U<<unit);
    Permit();
    /* A normal boot should have patched at AddDevice. The OpenDevice
     * observation remains a fallback for preexisting/atypical drivers. */
    if(!g.patched)rc2_patch((struct Device*)io->io_Device);
    if(g.dev!=(struct Device*)io->io_Device){
        g.failed=1;
        if(g.store){g.store->fail_code=32;g.store->worker_ready=RC2_FAULT;}
        return;
    }
    if(!g.failed&&!g.task&&!rc2_make_worker()){
        g.failed=1;
        if(g.store){g.store->fail_code=20;g.store->worker_ready=RC2_FAULT;}
    }
}
/* Called by the ROMTag with A6=ExecBase, including the FIRST installer boot. */
void rc2_coldstart(struct ExecBase *base){
    struct rc2_store*p=rc2_store_pointer;
    SysBase=base;
    rc2_zero(&g,sizeof(g));
    g.store=p;rc2_last_boot_result=0;
    if(!base||base->LibNode.lib_Version<39){
        rc2_last_boot_result=10;return;
    }
    if(!p||!TypeOfMem(p)||p->magic!=RC2_MAGIC){
        /* No valid arena: still intercept OpenDevice to block SD writes. */
        g.failed=1;rc2_last_boot_result=10;
    }else{
        p->boot_count++;
        p->worker_ready=0;
        if(!rc2_valid_store(p)){
            /* Install a faulting I/O gate if persistent COW is inconsistent. */
            g.failed=1;p->fail_code=0x44495254UL;rc2_last_boot_result=40;
        }else p->fail_code=0;
    }
    /* Priority 0 must remain minimal. Create the worker on first successful
       OpenDevice instead: then DOS/normal task scheduling is available. */
    rc2_previous_open=SetFunction((struct Library*)base,RC2_OPEN_LVO,(APTR)rc2_open_entry);
    rc2_previous_add=SetFunction((struct Library*)base,RC2_ADD_LVO,(APTR)rc2_add_entry);
    SumLibrary((struct Library*)base);CacheClearU();
    if(!rc2_previous_open||!rc2_previous_add)rc2_last_boot_result=30;
}
