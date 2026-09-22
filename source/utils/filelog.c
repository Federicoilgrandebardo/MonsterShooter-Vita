#include "utils/filelog.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <stdarg.h>

#define FILELOG_PATH DATA_PATH "log.txt"

void filelog_reset(void) {
    SceUID fd = sceIoOpen(FILELOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0)
        sceIoClose(fd);
}

// apre e chiude a ogni riga. E' lento, ma garantisce che l'ULTIMA riga
//           prima di un crash sia su disco — che e' l'unica che conta davvero.
//           Se il boot diventasse troppo lento, tenere l'fd aperto e sceIoSyncByFd.
void filelog_write(const char * text) {
    if (!text || !text[0])
        return;

    SceUID fd = sceIoOpen(FILELOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0)
        return;

    unsigned int len = 0;
    while (text[len]) len++;

    sceIoWrite(fd, text, len);
    sceIoClose(fd);
}

void filelog_printf(const char * fmt, ...) {
    char buf[1024];
    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(buf, sizeof(buf), fmt, list);
    va_end(list);

    sceClibPrintf(buf);
    filelog_write(buf);
}
