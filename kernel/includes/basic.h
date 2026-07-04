/* =============================================================================
 * basic.h — Tiny-BASIC interpreter (M22.5, PLAN §M22.5 stage 5).
 *
 * A classic line-numbered Tiny-BASIC dialect, small enough to read in
 * one sitting, big enough to write real toy programs on d-os itself:
 *
 *   variables    A..Z, 32-bit signed integers
 *   statements   PRINT items / LET v=e / v=e / INPUT v / IF e THEN s
 *                GOTO e / GOSUB e / RETURN / FOR v=a TO b [STEP c] /
 *                NEXT [v] / REM / CLS / END / STOP
 *   expressions  + - * / %  unary-  ( )  comparisons = <> < > <= >=
 *                RND(n) → 0..n-1
 *   REPL         type `10 PRINT "HI"` to store a line, a bare number
 *                deletes it; RUN / LIST / NEW / LOAD p / SAVE p / BYE
 *
 * INTERPRETER, not codegen — ring-0 native code generation is a
 * footgun and §M25 userland is where the real compile story lives.
 * The bytecode-compile extension point is bas_run(): it walks the
 * stored lines through one exec function, so a compiler would slot in
 * as an alternate implementation of that single function.
 *
 * Threading contract (kthread rule, task.h): bas_run polls
 * task_should_stop() and yields regularly, so the Task Manager can
 * stop a runaway program; INPUT blocks in vc_getchar which is a
 * standard kill point.  Output goes through kprintf (routed to the
 * caller's VC by the per-task console binding); input reads the VC
 * passed to basic_init.
 *
 * The struct is exposed (not opaque) so callers can place it wherever
 * fits their lifetime story: the GUI app uses one static instance
 * (singleton window), the shell command kmallocs one per run.  It is
 * large (~22 KiB) — NEVER put it on a 4 KiB kernel task stack.
 * ============================================================================= */

#ifndef BASIC_H
#define BASIC_H

#include <stdint.h>

struct vc;

#define BAS_MAX_LINES 256
#define BAS_TEXT_LEN  80
#define BAS_STACK     8                 /* FOR / GOSUB nesting depth */

struct bas_line {
    int  num;                           /* BASIC line number (1..99999) */
    char text[BAS_TEXT_LEN];            /* statement text, no number    */
};

struct basic {
    struct bas_line prog[BAS_MAX_LINES];
    int      nlines;
    int32_t  vars[26];                  /* A..Z */
    struct vc* vc;                      /* input source (INPUT, REPL)   */

    /* Run-time state (valid during bas_run). */
    int      pc;                        /* index into prog[]            */
    int      err;                       /* parse/exec error flag        */
    struct { int var; int32_t limit, step; int line_idx; }
             forstk[BAS_STACK];
    int      forsp;
    int      gosub[BAS_STACK];
    int      gsp;
    uint32_t rnd;                       /* LCG state for RND()          */
};

/* Reset everything (program + vars) and bind the input VC. */
void basic_init(struct basic* b, struct vc* vc);

/* Load `path` into the program store (replaces it).  Lines must carry
 * numbers ("10 PRINT ...").  Returns 0, or -1 (open/parse failure). */
int  basic_load(struct basic* b, const char* path);

/* Save the program store to `path` (numbered listing).  0 / -1. */
int  basic_save(struct basic* b, const char* path);

/* Run the stored program from its first line.  Returns when the
 * program ENDs, errors out, or the task is asked to stop. */
void basic_run(struct basic* b);

/* Interactive REPL (reads lines from b->vc until BYE or task stop).
 * If `autoload` is non-NULL, LOAD + RUN it first, then drop to the
 * prompt. */
void basic_repl(struct basic* b, const char* autoload);

#endif
