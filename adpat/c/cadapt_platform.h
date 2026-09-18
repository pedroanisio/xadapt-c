/*
 * cadapt_platform.h — the portability surface the command line and its
 * filesystem layer share: the feature macros that must be set before any
 * system header, the headers themselves, and the three calls whose names
 * differ between POSIX and Windows.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
 *
 * Include this before anything else: _POSIX_C_SOURCE and _GNU_SOURCE decide
 * what the system headers declare, so a header that arrives first wins.
 */
#ifndef CADAPT_PLATFORM_H
#define CADAPT_PLATFORM_H

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
/* Linux keeps SEEK_DATA and SEEK_HOLE, the sparse map, behind _GNU_SOURCE. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif

#include "adapt.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/utime.h>
#include <windows.h>
#define cadapt_getpid _getpid
#define cadapt_fseek _fseeki64
#define cadapt_ftell _ftelli64
typedef long long cadapt_off;
#else
#include <dirent.h>
#include <poll.h>
#include <unistd.h>
#define cadapt_getpid getpid
#define cadapt_fseek fseeko
#define cadapt_ftell ftello
typedef off_t cadapt_off;
/* Extended attributes: Linux and macOS list, read and set them by name;
 * the calls differ in shape, so each is wrapped below. Elsewhere on POSIX
 * a folder is packed without them. */
#if defined(__linux__) || defined(__APPLE__)
#include <sys/xattr.h>
#define CADAPT_XATTR 1
#endif
/* A symbolic link's own mode: Darwin and the BSDs set it (lchmod, or
 * fchmodat with AT_SYMLINK_NOFOLLOW) and their umask decides it at
 * creation, so a restored link gets the recorded mode and is checked
 * against it. Linux fixes every link at 0777 and refuses to change it,
 * so there a link's mode is the platform's and only its times are held. */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define CADAPT_LINK_MODE 1
#else
#define CADAPT_LINK_MODE 0
#endif
#endif

/* The feature macros above decide what the system headers declare, so they
 * must precede every one of them. When they do not, the declarations simply
 * go missing and the code below compiles to a working but wrong fallback —
 * a restored file that is dense rather than sparse, and a test far away.
 * Fail here instead, where the cause is. */
#if defined(__linux__) && !defined(_WIN32)
#if !defined(SEEK_HOLE) || !defined(SEEK_DATA)
#error "cadapt_platform.h must be included before any system header: SEEK_HOLE/SEEK_DATA are missing, so _GNU_SOURCE arrived too late"
#endif
#endif
/* Darwin keeps the same two behind _DARWIN_C_SOURCE. */
#if defined(__APPLE__)
#if !defined(SEEK_HOLE) || !defined(SEEK_DATA)
#error "cadapt_platform.h must be included before any system header: SEEK_HOLE/SEEK_DATA are missing, so _DARWIN_C_SOURCE arrived too late"
#endif
#endif

#endif /* CADAPT_PLATFORM_H */
