/* gzio.c -- IO on .gz files
 * Copyright (C) 1995-2002 Jean-loup Gailly.
 * For conditions of distribution and use, see copyright notice in zlib.h
 *
 * Compile this file with -DNO_DEFLATE to avoid the compression code.
 */

/* memgzio.c - IO on .gz files in memory
 * Adapted from original gzio.c from zlib library by Forgotten
 */
#include <zlib.h>
#ifndef local
#define local static
#endif
#ifndef OF
#define OF(args) args
#endif
#ifndef DEF_MEM_LEVEL
#  if MAX_MEM_LEVEL >= 8
#    define DEF_MEM_LEVEL 8
#  else
#    define DEF_MEM_LEVEL MAX_MEM_LEVEL
#  endif
#endif
#ifndef OS_CODE
#  if defined(_WIN32) && !defined(__CYGWIN__)
#    define OS_CODE 10
#  else
#    define OS_CODE 3
#  endif
#endif

gzFile ZEXPORT memgzopen(char *memory, int, const char *);
int ZEXPORT memgzread(gzFile, voidp, unsigned);
int ZEXPORT memgzwrite(gzFile, voidpc, unsigned);
int ZEXPORT memgzclose(gzFile);
long ZEXPORT memtell(gzFile);
