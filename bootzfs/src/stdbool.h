/*	$NetBSD$	*/

/*
 * zfsimpl.c asks for <stdbool.h>, which the compiler's own copy of would
 * supply were the boot loader not built with -nostdinc.  Naming NetBSD's
 * here rather than editing that file keeps it re-importable.
 */
#include <sys/stdbool.h>
