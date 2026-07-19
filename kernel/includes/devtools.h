/* =============================================================================
 * devtools.h — on-device developer tools (§M43).
 *
 * The compile+run engine behind the `tcc`/`exec` shell commands AND the Editor
 * "Compile & Run" button: it runs the embedded TinyCC binary to compile a C
 * source in the VFS into a runnable ELF, and loads+runs a VFS ELF — both under
 * the Linux personality.  Implemented in shell.c (where the embedded blobs
 * live); declared here so the GUI editor can reuse it.
 * ============================================================================= */
#ifndef DEVTOOLS_H
#define DEVTOOLS_H

/* Compile `src` → `out` with the on-device tcc.  Returns 0 if tcc produced the
 * output file, non-zero otherwise.  (tcc's diagnostics go to the console.) */
int dos_tcc_compile(const char* src, const char* out);

/* Load + run the ELF at `path` (Linux personality) as an excursion; returns
 * the program's result, or -1 if it couldn't be loaded. */
int dos_run_elf(const char* path);

/* True if the on-device tcc is embedded in this build. */
int dos_tcc_available(void);

#endif
