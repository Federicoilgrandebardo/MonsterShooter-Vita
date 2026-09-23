/*
 * Filmati con sceAvPlayer.
 *
 * Il gioco chiama PlayMovie() e poi sonda IsMoviePlaying() due volte per frame,
 * continuando a chiamare eglSwapBuffers dallo stesso thread: il video si
 * disegna sopra il suo frame, subito prima dello swap. Niente thread GL nostri.
 *
 * Il frame decodificato (YUV420) diventa una texture vitaGL puntando la
 * texture GXM direttamente sul buffer del player: nessuna copia.
 * L'audio va su una porta VOICE, perche' l'unica BGM e' gia' del gioco.
 */

#include "reimpl/movie.h"

#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/avplayer.h>
#include <psp2/gxm.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/fcntl.h>
#include <psp2/sysmodule.h>
#include <vitaGL.h>

#include "utils/logger.h"

#define FRAMES 5

static SceAvPlayerHandle player;
static int playing;
static int audio_port = -1;
static SceUID audio_thid = -1;
static volatile int audio_run;

static GLuint tex[FRAMES];
static SceGxmTexture *gxm_tex[FRAMES];
static int tex_ready, cur, have_frame;
static float vw, vh;
static unsigned skip_armed, skip_prev; // tasti premuti durante il film

#define SKIP_BUTTONS (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_START)
#define SKIP_TOUCH   0x80000000u // bit finto: dito sul touch frontale

// Tasti di skip premuti ora, touch frontale compreso.
static unsigned skip_input(void) {
    SceCtrlData pad;
    SceTouchData touch;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    return (pad.buttons & SKIP_BUTTONS) | (touch.reportNum > 0 ? SKIP_TOUCH : 0);
}

static void *mem_alloc(void *p, uint32_t align, uint32_t size) {
    (void)p;
    return memalign(align, size);
}

static void mem_free(void *p, void *addr) {
    (void)p;
    free(addr);
}

// I frame li scrive il decoder hardware e li legge la GPU: vanno in CDRAM,
// mappata su GXM. Dai pool di vitaGL (RAM) l'allocazione riesce ma il
// decoder si ferma a 0 frame (VERIFICATO sul ferro): serve memoria video.
// La CDRAM libera la garantisce la soglia in gl_init (glutil.c).
static void *gpu_alloc(void *p, uint32_t align, uint32_t size) {
    (void)p;
    void *res = NULL;
    if (align < 0x40000) align = 0x40000;
    size = (size + align - 1) & ~(align - 1);
    SceUID blk = sceKernelAllocMemBlock("movie_frame", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
    if (blk < 0) {
        l_error("[MOVIE] CDRAM negata per %u byte (0x%08X)", size, blk);
        return NULL;
    }
    sceKernelGetMemBlockBase(blk, &res);
    sceGxmMapMemory(res, size, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    return res;
}

static void gpu_free(void *p, void *addr) {
    (void)p;
    glFinish(); // la GPU potrebbe ancora leggere il frame
    SceUID blk = sceKernelFindMemBlockByAddr(addr, 0);
    sceGxmUnmapMemory(addr);
    sceKernelFreeMemBlock(blk);
}

// I/O del player: la configurazione verificata sul ferro passa da qui.
static SceUID movie_fd = -1;

static int file_open(void *p, const char *name) {
    (void)p;
    movie_fd = sceIoOpen(name, SCE_O_RDONLY, 0);
    return movie_fd < 0 ? -1 : 0;
}

static int file_close(void *p) {
    (void)p;
    if (movie_fd >= 0) sceIoClose(movie_fd);
    movie_fd = -1;
    return 0;
}

static int file_read(void *p, uint8_t *buf, uint64_t pos, uint32_t len) {
    (void)p;
    return sceIoPread(movie_fd, buf, len, pos);
}

static uint64_t file_size(void *p) {
    (void)p;
    return sceIoLseek(movie_fd, 0, SCE_SEEK_END);
}

static int audio_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    SceAvPlayerFrameInfo f;
    int grain = 0, rate = 0, ch = 0;
    while (audio_run && sceAvPlayerIsActive(player)) {
        if (!sceAvPlayerGetAudioData(player, &f)) {
            sceKernelDelayThread(1000);
            continue;
        }
        int c = f.details.audio.channelCount;
        int g = f.details.audio.size / (c * 2);
        if (g != grain || (int)f.details.audio.sampleRate != rate || c != ch) {
            grain = g; rate = f.details.audio.sampleRate; ch = c;
            sceAudioOutSetConfig(audio_port, grain, rate,
                    ch == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
        }
        sceAudioOutOutput(audio_port, f.pData);
    }
    return sceKernelExitThread(0);
}

int movie_start(const char *name) {
    static int module_loaded;
    char path[256];

    if (playing || !name) return 0;
    snprintf(path, sizeof(path), DATA_PATH "movie/%s.mp4", name);
    struct stat st;
    if (stat(path, &st) != 0) {
        l_warn("[MOVIE] %s mancante, filmato saltato", path);
        return 0;
    }

    if (!module_loaded) {
        sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
        module_loaded = 1;
    }
    // Le texture restano vive per tutti e quattro i filmati: nessuna la
    // cancella, quindi vitaGL non libera mai il buffer del player.
    if (!tex_ready) {
        glGenTextures(FRAMES, tex);
        for (int i = 0; i < FRAMES; i++) {
            glBindTexture(GL_TEXTURE_2D, tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            gxm_tex[i] = vglGetGxmTexture(GL_TEXTURE_2D);
            vglFree(vglGetTexDataPointer(GL_TEXTURE_2D));
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        tex_ready = 1;
    }

    SceAvPlayerInitData init;
    memset(&init, 0, sizeof(init));
    init.memoryReplacement.allocate = mem_alloc;
    init.memoryReplacement.deallocate = mem_free;
    init.memoryReplacement.allocateTexture = gpu_alloc;
    init.memoryReplacement.deallocateTexture = gpu_free;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = FRAMES;
    init.autoStart = 1;
    init.fileReplacement.open = file_open;
    init.fileReplacement.close = file_close;
    init.fileReplacement.readOffset = file_read;
    init.fileReplacement.size = file_size;
    // L'handle e' un puntatore (0x84......): come int e' negativo, quindi il
    // fallimento si riconosce solo dallo zero.
    player = sceAvPlayerInit(&init);
    if (!player) {
        l_error("[MOVIE] sceAvPlayerInit ha risposto 0x%08X", player);
        return 0;
    }
    int ret = sceAvPlayerAddSource(player, path);
    if (ret < 0) {
        l_error("[MOVIE] sceAvPlayerAddSource(%s) ha risposto 0x%08X", path, ret);
        sceAvPlayerClose(player);
        return 0;
    }

    audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_VOICE, 1024, 48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (audio_port < 0) {
        l_warn("[MOVIE] porta audio negata (0x%08X), filmato muto", audio_port);
    } else {
        audio_run = 1;
        audio_thid = sceKernelCreateThread("movie_audio", audio_thread, 0x40, 0x10000, 0, 0, NULL);
        sceKernelStartThread(audio_thid, 0, NULL);
    }

    // Solo tasti premuti DOPO l'avvio: quello che ha aperto il livello puo'
    // essere ancora giu'.
    skip_prev = skip_input();
    skip_armed = 0;
    have_frame = 0;
    playing = 1;
    l_info("[MOVIE] %s avviato", path);
    return 1;
}

static void movie_stop(void) {
    audio_run = 0;
    if (audio_thid >= 0) {
        sceKernelWaitThreadEnd(audio_thid, NULL, NULL);
        sceKernelDeleteThread(audio_thid);
        audio_thid = -1;
    }
    if (audio_port >= 0) {
        sceAudioOutReleasePort(audio_port);
        audio_port = -1;
    }
    sceAvPlayerStop(player);
    sceAvPlayerClose(player); // libera i frame: da qui le texture puntano al nulla
    have_frame = 0;
    playing = 0;
    l_info("[MOVIE] fine filmato");
}

int movie_active(void) {
    return playing;
}

int movie_is_playing(void) {
    if (playing && !sceAvPlayerIsActive(player))
        movie_stop();
    return playing;
}

// Salto al RILASCIO di un tasto premuto durante il film: pressione e rilascio
// cadono entrambi mentre l'input e' scartato (dynlib.c), cosi' il gioco non
// riceve un rilascio orfano prima che il livello esista.
static void check_skip(void) {
    unsigned now = skip_input();
    skip_armed |= now & ~skip_prev;
    if (skip_armed & ~now) {
        l_info("[MOVIE] saltato");
        sceAvPlayerStop(player); // IsMoviePlaying chiude il resto
        skip_armed = 0;
    }
    skip_prev = now;
}

void movie_draw(void) {
    if (!playing) return;
    check_skip();

    SceAvPlayerFrameInfo f;
    if (sceAvPlayerGetVideoData(player, &f)) {
        cur = (cur + 1) % FRAMES;
        vw = f.details.video.width;
        vh = f.details.video.height;
        sceGxmTextureInitLinear(gxm_tex[cur], f.pData, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1, vw, vh, 0);
        sceGxmTextureSetMinFilter(gxm_tex[cur], SCE_GXM_TEXTURE_FILTER_LINEAR);
        sceGxmTextureSetMagFilter(gxm_tex[cur], SCE_GXM_TEXTURE_FILTER_LINEAR);
        have_frame = 1;
    }

    // Lo stato GL e' del gioco (combiner DOT3 compreso): si salva tutto quello
    // che si tocca e si rimette com'era, altrimenti il primo frame dopo il
    // filmato eredita il nostro.
    GLint unit, bind0, env0;
    GLboolean on0, on1;
    GLfloat clear[4];
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    glActiveTexture(GL_TEXTURE1);
    on1 = glIsEnabled(GL_TEXTURE_2D);
    glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    on0 = glIsEnabled(GL_TEXTURE_2D);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bind0);
    glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &env0);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);

    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                 GL_TRANSFORM_BIT | GL_VIEWPORT_BIT | GL_SCISSOR_BIT);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_FOG);
    glDisable(GL_LIGHTING);
    glViewport(0, 0, 960, 544);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();

    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (have_frame) {
        // Adattato in altezza, bande nere ai lati (640x432 su 960x544).
        float w = vw * 544.0f / vh, h = 544.0f;
        if (w > 960.0f) { h = vh * 960.0f / vw; w = 960.0f; }
        float x = (960.0f - w) / 2, y = (544.0f - h) / 2;
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, tex[cur]);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0, 0); glVertex2f(x, y);
        glTexCoord2f(1, 0); glVertex2f(x + w, y);
        glTexCoord2f(0, 1); glVertex2f(x, y + h);
        glTexCoord2f(1, 1); glVertex2f(x + w, y + h);
        glEnd();
    }

    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib(); // rimette anche la matrix mode e il viewport
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, env0);
    glBindTexture(GL_TEXTURE_2D, bind0);
    if (on0) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE1);
    if (on1) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
    glActiveTexture(unit);
}
