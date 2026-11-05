/* SPDX-License-Identifier: MIT
 * Optional visual-only Exec task embedded in aminesiac.RC4.
 * No DOS, no disk access, no title updates and no Workbench patches.
 * The COW worker must remain fully functional if this component fails.
 */
#include <stdint.h>
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include "rc2_protocol.h"

#define RC4_VIS_STACK 8192UL
#define RC4_VIS_WIDTH 224UL

extern struct ExecBase *SysBase;
extern int rc4_visual_status(void);
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
static const char banner[] = "DISPOSABLE SESSION!";
static const char task_name[] = "aminesiac.RC4.visual";

static int same(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

/* Check the public-screen list BEFORE LockPubScreen: that function alone
 * can open Workbench when none exists (including early boot / CLI only). */
static int wb_present(void)
{
    struct List *list = LockPubScreenList();
    struct Node *n;
    int found = 0;
    if (!list) return 0;
    for (n = list->lh_Head; n && n->ln_Succ; n = n->ln_Succ)
        if (same(n->ln_Name, "Workbench")) { found = 1; break; }
    UnlockPubScreenList();
    return found;
}

static void visual_task(void)
{
    struct Window *w = NULL;
    struct Screen *s;
    ULONG width, height;
    unsigned i;

    /* No DOS calls: Exec Tasks have no AmigaDOS Process context. */
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39);
    if (!GfxBase || !IntuitionBase) goto stopped;

    for (;;) {
        if (!rc4_visual_status()) break;
        if (!w && wb_present()) {
            s = LockPubScreen((UBYTE *)"Workbench");
            if (s) {
                struct TagItem tags[] = {
                    {WA_PubScreen, (ULONG)(uintptr_t)s},
                    {WA_Left, 0}, {WA_Top, 0},
                    {WA_Width, 0}, {WA_Height, 0},
                    {WA_Title, (ULONG)(uintptr_t)banner},
                    {WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                               WFLG_NOCAREREFRESH},
                    {TAG_DONE, 0}
                };
                width = (ULONG)s->Width < RC4_VIS_WIDTH ?
                        (ULONG)s->Width : RC4_VIS_WIDTH;
                height = (ULONG)s->BarHeight + 2UL;
                if (height < 11UL) height = 11UL;
                tags[1].ti_Data = (ULONG)s->Width - width;
                tags[3].ti_Data = width;
                tags[4].ti_Data = height;
                w = OpenWindowTagList(NULL, tags);
                UnlockPubScreen(NULL, s);
            }
        }
        /* Only wait for Workbench or check safety state; title never updates. */
        for (i = 0; i < 50U; ++i) WaitTOF();
    }
    if (w) CloseWindow(w);
    CloseLibrary((struct Library *)IntuitionBase);
    CloseLibrary((struct Library *)GfxBase);
stopped:
    /* Never return into boot code or retry indefinitely on failures. */
    for (;;) Wait(SIGBREAKF_CTRL_C);
}

/* Called ONCE by the ready worker on each boot; best-effort only. */
void rc4_visual_start(void)
{
    UBYTE *mem;
    struct Task *t;
    ULONG size = (ULONG)sizeof(*t) + RC4_VIS_STACK + 16UL;
    mem = (UBYTE *)AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!mem) return;
    t = (struct Task *)mem;
    t->tc_Node.ln_Type = NT_TASK;
    t->tc_Node.ln_Pri = -5;
    t->tc_Node.ln_Name = (STRPTR)task_name;
    t->tc_SPLower = (APTR)(mem + sizeof(*t));
    t->tc_SPUpper = (APTR)(mem + size);
    t->tc_SPReg = (APTR)((ULONG)(uintptr_t)t->tc_SPUpper & ~3UL);
    t->tc_MemEntry.lh_Head = (struct Node *)&t->tc_MemEntry.lh_Tail;
    t->tc_MemEntry.lh_Tail = NULL;
    t->tc_MemEntry.lh_TailPred = (struct Node *)&t->tc_MemEntry.lh_Head;
    if (!AddTask(t, (APTR)visual_task, NULL)) FreeMem(mem, size);
}
