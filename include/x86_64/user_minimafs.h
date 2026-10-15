#ifndef MINIMALOS_X86_64_USER_MINIMAFS_H
#define MINIMALOS_X86_64_USER_MINIMAFS_H

// Mos_fwrite, mos_listdir, mos_delete, mos_rmdir, mos_get_metadata, mos_tell and mos_feof are defined once in user_common.h,
// which minimalos.h includes too, so the two can be mixed freely.
#include "x86_64/user_syscalls.h"

#endif
