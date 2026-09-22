/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <unistd.h>
#include "reimpl/mem.h"
#include "utils/logger.h"

#include <string.h>
#include <malloc.h>
#include <psp2/kernel/clib.h>

void *sceClibMemclr(void *dst, size_t len) {
    return sceClibMemset(dst, 0, len);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offs) {
    l_warn("mmap(%p, %i, %i, %i, %i, %li)", addr, length, prot, flags, fd, offs);

    if (length <= 0) {
        return MAP_FAILED;
    }

    void* ret = malloc(length);
    if (!ret)
        return MAP_FAILED;
    memset(ret, 0, length);

    // Il motore mappa i .pak con mmap file-backed: senza leggere davvero il file
    // si ottiene un blocco di zeri e ogni parsing a valle fallisce.
    // MAP_ANONYMOUS (0x20 su bionic) resta memoria e basta.
    if (fd > 0 && !(flags & 0x20)) {
        if (lseek(fd, offs, SEEK_SET) == (off_t)-1) {
            l_warn("mmap: lseek(fd %i, %li) fallita", fd, (long)offs);
            free(ret);
            return MAP_FAILED;
        }
        size_t done = 0;
        while (done < length) {
            int n = read(fd, (char *)ret + done, length - done);
            if (n <= 0)
                break;
            done += n;
        }
        if (done != length)
            l_warn("mmap: letti %u byte su %u richiesti (fd %i)", (unsigned)done, (unsigned)length, fd);
        else
            l_debug("mmap: mappati %u byte dal fd %i", (unsigned)length, fd);
    }

    return ret;
}

int munmap(void *addr, size_t length) {
    if (addr) free(addr);
    return 0;
}
