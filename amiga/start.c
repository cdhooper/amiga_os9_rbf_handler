/*
 * Handler entry point
 *
 * DOS starts the handler at the first byte of its first code hunk, so
 * this file is linked first and holds only this function (built with
 * -fno-toplevel-reorder; the Makefile checks the symbol is at offset 0).
 * No C library startup code is used.
 */

#include <exec/types.h>
#include "handler.h"

void
handler_entry(void)
{
    SysBase = *(struct ExecBase **) 4;
    handler_main();
}
