#ifndef SHADOWSD_RC2_COMMAND_POLICY_H
#define SHADOWSD_RC2_COMMAND_POLICY_H
/* Requires <exec/types.h>, <exec/io.h>, <devices/trackdisk.h>. */
/* Forward ONLY control commands which cannot carry sector writes. This is
 * essential for early filesystem startup: the initial RC2 allowlist
 * inadvertently rejected TD_GETGEOMETRY and TD_ADDCHANGEINT. In particular,
 * TD_ADDCHANGEINT must retain its native asynchronous lifetime; never
 * synthesize a completion or queue it to our COW worker.
 * Numeric values are NDK CMD_NONSTD-based, NSCMD_DEVICEQUERY=0x4000.
 */
static inline int rc2_safe_control(UWORD c){
    switch(c){
    case TD_MOTOR:          /* 9  */
    case TD_REMOVE:         /* 12 */
    case TD_CHANGENUM:      /* 13 */
    case TD_CHANGESTATE:    /* 14 */
    case TD_PROTSTATUS:     /* 15 */
    case TD_GETDRIVETYPE:  /* 18 */
    case TD_GETNUMTRACKS:  /* 19 */
    case TD_ADDCHANGEINT:  /* 20 */
    case TD_REMCHANGEINT:  /* 21 */
    case TD_GETGEOMETRY:   /* 22 */
    case 0x4000:           /* NSCMD_DEVICEQUERY */
        return 1;
    default: return 0;
    }
}
/* Strict data path accepted by RC1, still virtualized by COW worker. */
static inline int rc2_cow_command(UWORD c){
    return c==CMD_READ||c==CMD_WRITE||c==0xc000||c==0xc001||
           c==CMD_UPDATE||c==CMD_CLEAR||c==CMD_FLUSH;
}
#endif
