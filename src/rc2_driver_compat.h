#ifndef SHADOWSD_RC3_DRIVER_COMPAT_H
#define SHADOWSD_RC3_DRIVER_COMPAT_H
/* Portable brcm-sdhc.device gate ABI. No driver release, git revision,
 * firmware address or private struct offset is part of compatibility.
 * Only standard Exec device vectors are inspected; unrecognised layouts
 * fail closed. Must be safe to use from the coldstart resident (no libc).
 */
#include <stdint.h>
#include <exec/types.h>
#include <exec/devices.h>
#define RC2_BEGIN_LVO (-30L)
#define RC2_ABORT_LVO (-36L)
#define RC2_JMP_ABS 0x4ef9U

static int rc2_device_name_equal(const char *a,const char *b){
    if(!a||!b)return 0;
    while(*a&&*a==*b){++a;++b;}
    return *a==*b;
}
static ULONG rc2_driver_jump_target(const struct Library *lib,LONG lvo){
    const volatile UBYTE *v;
    ULONG target;
    if(!lib||lvo>=0||lib->lib_NegSize<(UWORD)(-lvo))return 0;
    v=(const volatile UBYTE *)lib+lvo;
    if(v[0]!=0x4eU||v[1]!=0xf9U)return 0;
    /* Decode the 68k absolute JMP in big-endian order. On a host test,
     * native pointers need not have the target's byte order. */
    target=((ULONG)v[2]<<24)|((ULONG)v[3]<<16)|((ULONG)v[4]<<8)|v[5];
    return target;
}
static int rc2_driver_target_valid(ULONG target){
    return target>=0x100UL && !(target&1UL);
}
static int rc2_driver_identity(const struct Device *d){
    const struct Library *lib=d?&d->dd_Library:0;
    return lib&&rc2_device_name_equal(lib->lib_Node.ln_Name,RC2_DEVICE)&&
           lib->lib_NegSize>=36&&lib->lib_PosSize>=sizeof(struct Device);
}
/* PREPATCH is useful both for installer PROBE and during AddDevice.
 * A device calling itself brcm-sdhc but not exposing two normal Exec
 * vectors is NOT considered compatible merely because it can be opened. */
static int rc2_driver_prepatch(const struct Device *d,ULONG *begin,ULONG *abort){
    ULONG b,a;
    if(!rc2_driver_identity(d))return 0;
    b=rc2_driver_jump_target(&d->dd_Library,RC2_BEGIN_LVO);
    a=rc2_driver_jump_target(&d->dd_Library,RC2_ABORT_LVO);
    if(!rc2_driver_target_valid(b)||!rc2_driver_target_valid(a))return 0;
    if(begin)*begin=b;
    if(abort)*abort=a;
    return 1;
}
/* Check that the hooks are active and SetFunction returned sensible
 * original handlers. Their addresses are intentionally not hard-coded. */
static int rc2_driver_postpatch(const struct Device *d,
                                ULONG old_begin,ULONG old_abort,
                                ULONG gate_begin,ULONG gate_abort){
    return rc2_driver_identity(d)&&
      rc2_driver_target_valid(old_begin)&&rc2_driver_target_valid(old_abort)&&
      old_begin!=gate_begin&&old_abort!=gate_abort&&
      rc2_driver_jump_target(&d->dd_Library,RC2_BEGIN_LVO)==gate_begin&&
      rc2_driver_jump_target(&d->dd_Library,RC2_ABORT_LVO)==gate_abort;
}
#endif
