/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 * Copyright (C) 2026      Ellie J Turner
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dynlib.c
 * @brief Resolving dynamic imports of the .so.
 */

#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include <malloc.h>
#include <math.h>
#include <netdb.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <zlib.h>
#include <locale.h>
#include <poll.h>

#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <so_util/so_util.h>
#include <utime.h>

#include "utils/glutil.h"
#include "utils/utils.h"
#include "utils/logger.h"

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif


// OpenSLES rimosso, il gioco non lo importa (audio via JNI ClawAudio).
//           Se servisse: reinstallare vita-opensles e ripristinare le entry SL_IID_*.
#include "reimpl/errno.h"
#include "reimpl/io.h"
#include "reimpl/log.h"
#include "reimpl/mem.h"
#include "reimpl/pthr.h"
#include "reimpl/sys.h"
#include "reimpl/egl.h"
#include "reimpl/time64.h"
#include "reimpl/asset_manager.h"

#ifdef NDK_PORT
#include <falso_ndk/FalsoNDK.h>
#endif

const unsigned int __page_size = PAGE_SIZE;

extern void * _ZNSt9exceptionD2Ev;
extern void * _ZSt17__throw_bad_allocv;
extern void * _ZSt9terminatev;
extern void * _ZdaPv;
extern void * _ZdlPv;
extern void * _Znaj;
extern void * __cxa_allocate_exception;
extern void * __cxa_begin_catch;
extern void * __cxa_end_catch;
extern void * __cxa_free_exception;
extern void * __cxa_rethrow;
extern void * __cxa_throw;
extern void * __gxx_personality_v0;
extern void *_ZNSt8bad_castD1Ev;
extern void *_ZTISt8bad_cast;
extern void *_ZTISt9exception;
extern void *_ZTVN10__cxxabiv117__class_type_infoE;
extern void *_ZTVN10__cxxabiv120__si_class_type_infoE;
extern void *_ZTVN10__cxxabiv121__vmi_class_type_infoE;
extern void *_Znwj;
extern void *__aeabi_atexit;
extern void *__aeabi_d2lz;
extern void *__aeabi_d2ulz;
extern void *__aeabi_dadd;
extern void *__aeabi_dcmpgt;
extern void *__aeabi_dcmplt;
extern void *__aeabi_ddiv;
extern void *__aeabi_dmul;
extern void *__aeabi_f2lz;
extern void *__aeabi_f2ulz;
extern void *__aeabi_i2d;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_l2d;
extern void *__aeabi_l2f;
extern void *__aeabi_ldivmod;
extern void *__aeabi_memclr;
extern void *__aeabi_memcpy;
extern void *__aeabi_memmove;
extern void *__aeabi_memset4;
extern void *__aeabi_memset8;
extern void *__aeabi_memset;
extern void *__aeabi_ui2d;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_ul2d;
extern void *__aeabi_ul2f;
extern void *__aeabi_uldivmod;
extern void *__aeabi_unwind_cpp_pr0;
extern void *__aeabi_unwind_cpp_pr1;
extern void *__cxa_atexit;
extern void *__cxa_call_unexpected;
extern void *__cxa_finalize;
extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;
extern void *__cxa_pure_virtual;
extern void *__gnu_ldivmod_helper;
extern void *__gnu_unwind_frame;
extern void *__srget;
extern void *__stack_chk_guard;
extern void *__swbuf;

extern const char *BIONIC_ctype_;
extern const short *BIONIC_tolower_tab_;
extern const short *BIONIC_toupper_tab_;

static FILE __sF_fake[3];

void *dlsym_soloader(void * handle, const char * symbol);


// Su Vita la protezione delle pagine non e' gestibile dallo user space e non
// esiste remap: il motore li chiama in percorsi opzionali, quindi stub onesti.
// mprotect torna 0 (successo) perche' la memoria del loader e' gia' RWX via
// kubridge; mremap fallisce e chi chiama ricade su malloc+memcpy.
// L'unico simbolo NDK che FalsoNDK non dichiara. La Vita non ha tasti
// modificatori (shift/alt/ctrl), quindi nessun meta-stato attivo.
static int32_t AKeyEvent_getMetaState_stub(const AInputEvent *event) { return 0; }

static int mprotect_stub(void *addr, size_t len, int prot) { return 0; }
static void *mremap_stub(void *old, size_t oldsz, size_t newsz, int flags) { return (void *)-1; }
static struct mallinfo mallinfo_stub(void) { struct mallinfo mi; memset(&mi, 0, sizeof(mi)); return mi; }


// Stato dell'array colori, condiviso fra le sonde.
static GLuint g_bound_tex[4] = {0,0,0,0};
static GLsizei g_tex_w[64] = {0}, g_tex_h[64] = {0};
static GLenum g_active_unit = 0x84C0;
static int g_tex2d_on[4] = {0,0,0,0};
static const unsigned char *g_col_ptr = NULL;
static GLsizei g_col_stride = 0;
static GLenum g_col_type = 0;
static GLint g_col_size = 0;
static int g_col_enabled = 0;

// --- sonde diagnostiche texture (rimuovere quando la UI funziona) ---
// La UI appare come rettangoli bianchi/grigi: geometria giusta, texture assente.
// Serve sapere quale formato il gioco sceglie e con quali parametri carica.
static void glCompressedTexImage2D_probe(GLenum target, GLint level, GLenum fmt,
                                         GLsizei w, GLsizei h, GLint border,
                                         GLsizei imgsize, const void *data) {
    { unsigned id = g_bound_tex[(g_active_unit - 0x84C0u) < 4 ? (g_active_unit - 0x84C0u) : 0];
      if (id < 64) { g_tex_w[id] = w; g_tex_h[id] = h; } }
    l_debug("upload ETC1 su tex id %u (unit %u): %ix%i size=%i",
            g_bound_tex[(g_active_unit - 0x84C0u) < 4 ? (g_active_unit - 0x84C0u) : 0],
            (unsigned)(g_active_unit - 0x84C0u), w, h, imgsize);
    glCompressedTexImage2D(target, level, fmt, w, h, border, imgsize, data);
    GLenum e = glGetError();
    if (e) l_warn("  -> glGetError 0x%x", e);
}

static void glTexImage2D_probe(GLenum target, GLint level, GLint internalfmt,
                               GLsizei w, GLsizei h, GLint border,
                               GLenum fmt, GLenum type, const void *data) {
    { unsigned id = g_bound_tex[(g_active_unit - 0x84C0u) < 4 ? (g_active_unit - 0x84C0u) : 0];
      if (id < 64) { g_tex_w[id] = w; g_tex_h[id] = h; } }
    l_debug("upload RGBA su tex id %u (unit %u): %ix%i ifmt=0x%x",
            g_bound_tex[(g_active_unit - 0x84C0u) < 4 ? (g_active_unit - 0x84C0u) : 0],
            (unsigned)(g_active_unit - 0x84C0u), w, h, internalfmt);
    glTexImage2D(target, level, internalfmt, w, h, border, fmt, type, data);
    GLenum e = glGetError();
    if (e) l_warn("  -> glGetError 0x%x", e);
}

static const GLubyte * glGetString_probe(GLenum name) {
    const GLubyte *r = glGetString(name);
    l_debug("glGetString(0x%x) = %s", name, r ? (const char *)r : "(null)");
    return r;
}


// Sonda combiner: il gioco carica coppie di atlas ETC1 (RGB + alpha) e li fonde
// con il texture combiner. Logga solo i cambi di stato, non ogni frame.
// sonde sulla coda di input - rimuovere a bug chiuso.
// Domanda: durante la schermata di caricamento il gioco LEGGE ancora l'input?
// Se smette di interrogare la coda, non sta aspettando un tocco.
static int32_t AInputQueue_hasEvents_probe(AInputQueue *q) {
    static unsigned n = 0, got = 0;
    int32_t r = AInputQueue_hasEvents(q);
    n++;
    if (r > 0) got++;
    if ((n % 300) == 0)
        l_debug("[IN] hasEvents interrogata %u volte, eventi disponibili %u", n, got);
    return r;
}

static int32_t AInputQueue_getEvent_probe(AInputQueue *q, AInputEvent **out) {
    int32_t r = AInputQueue_getEvent(q, out);
    static int budget = 40;
    if (budget > 0 && r >= 0) {
        budget--;
        l_debug("[IN] getEvent -> %d (il gioco ha PRESO un evento)", (int)r);
    }
    return r;
}

// sonda sul ciclo di vita dei thread - rimuovere a bug chiuso.
// Il thread di caricamento si ferma dopo il fallimento del sync: qui si vede
// se ESCE (la sua funzione ritorna) o se resta appeso dentro.
typedef struct { void *(*start)(void *); void *arg; int id; } thr_probe_t;

static void *thread_trampoline(void *p) {
    thr_probe_t *t = (thr_probe_t *)p;
    int id = t->id;
    void *(*start)(void *) = t->start;
    void *arg = t->arg;
    free(t);
    l_debug("[THR] #%d entra (start=%p)", id, (void *)start);
    void *r = start(arg);
    l_debug("[THR] #%d ESCE (start=%p, ret=%p)", id, (void *)start, r);
    return r;
}

static int pthread_create_probe(pthread_t *thread, const pthread_attr_t_bionic *attr,
                                void *(*start)(void *), void *param) {
    static int seq = 0;
    thr_probe_t *t = (thr_probe_t *)malloc(sizeof(thr_probe_t));
    if (!t)
        return pthread_create_soloader(thread, attr, start, param);
    t->start = start; t->arg = param; t->id = ++seq;
    l_debug("[THR] #%d creato (start=%p)", t->id, (void *)start);
    int r = pthread_create_soloader(thread, attr, thread_trampoline, t);
    if (r != 0) { l_debug("[THR] #%d CREAZIONE FALLITA (%d)", t->id, r); free(t); }
    return r;
}

// sonde di rete. I server Gamelion sono spenti da anni; se il
// caricamento aspetta una risposta che non arriva, si vede qui.
static struct hostent *gethostbyname_probe(const char *name) {
    l_debug("[NET] gethostbyname(\"%s\")", name ? name : "(null)");
    struct hostent *r = gethostbyname(name);
    l_debug("[NET] gethostbyname(\"%s\") -> %s", name ? name : "(null)", r ? "ok" : "NULL");
    return r;
}

static int connect_probe(int fd, const struct sockaddr *addr, socklen_t len) {
    l_debug("[NET] connect(fd=%d) inizio", fd);
    int r = connect(fd, addr, len);
    l_debug("[NET] connect(fd=%d) -> %d (errno %d)", fd, r, errno);
    return r;
}

static void glTexEnvi_probe(GLenum target, GLenum pname, GLint param) {
    static GLenum lt = 0xffff; static GLenum lp = 0xffff; static GLint lv = -12345;
    static int budget = 60;  // il combiner si riconfigura a ogni frame
    if (budget > 0 && (target != lt || pname != lp || param != lv)) {
        budget--;
        l_debug("glTexEnvi(target=0x%x pname=0x%x param=0x%x)", target, pname, param);
        lt = target; lp = pname; lv = param;
    }
    glTexEnvi(target, pname, param);
}

static void glClientActiveTexture_probe(GLenum unit) {
    static GLenum last = 0xffff;
    if (unit != last) { l_debug("glClientActiveTexture(0x%x)", unit); last = unit; }
    glClientActiveTexture(unit);
}

static void glBlendFunc_probe(GLenum src, GLenum dst) {
    static GLenum ls = 0xffff; static GLenum ld = 0xffff;
    if (src != ls || dst != ld) { l_debug("glBlendFunc(0x%x, 0x%x)", src, dst); ls = src; ld = dst; }
    glBlendFunc(src, dst);
}


// Sonda FBO: il gioco usa render-to-texture. Se la UI viene disegnata su un
// framebuffer incompleto, il risultato e' un quad bianco senza errori GL.
static GLenum glCheckFramebufferStatusOES_probe(GLenum target) {
    GLenum st = glCheckFramebufferStatus(target);
    static GLenum last = 0;
    if (st != last) {
        l_debug("glCheckFramebufferStatus(0x%x) = 0x%x %s", target, st,
                st == 0x8CD5 ? "(COMPLETE)" : "(INCOMPLETO!)");
        last = st;
    }
    return st;
}

static void glFramebufferTexture2DOES_probe(GLenum target, GLenum att, GLenum textarget,
                                            GLuint tex, GLint level) {
    static int budget = 20;
    if (budget > 0) {
        budget--;
        l_debug("glFramebufferTexture2D(t=0x%x att=0x%x tex=%u lvl=%i)", target, att, tex, level);
    }
    glFramebufferTexture2D(target, att, textarget, tex, level);
    GLenum e = glGetError();
    if (e) l_warn("  -> glGetError 0x%x", e);
}

static void glBindFramebufferOES_probe(GLenum target, GLuint fb) {
    static int budget = 20;
    if (budget > 0) { budget--; l_debug("glBindFramebuffer(0x%x, %u)", target, fb); }
    glBindFramebuffer(target, fb);
}

static void glRenderbufferStorageOES_probe(GLenum target, GLenum fmt, GLsizei w, GLsizei h) {
    static int budget = 20;
    if (budget > 0) { budget--; l_debug("glRenderbufferStorage(fmt=0x%x %ix%i)", fmt, w, h); }
    glRenderbufferStorage(target, fmt, w, h);
    GLenum e = glGetError();
    if (e) l_warn("  -> glGetError 0x%x", e);
}


static void glEnableClientState_probe(GLenum a) {
    if (a == 0x8076) g_col_enabled = 1;  // GL_COLOR_ARRAY
    static int budget = 24;
    if (budget > 0) { budget--; l_debug("glEnableClientState(0x%x)", a); }
    glEnableClientState(a);
}

static void glDisableClientState_probe(GLenum a) {
    if (a == 0x8076) g_col_enabled = 0;
    static int budget = 24;
    if (budget > 0) { budget--; l_debug("glDisableClientState(0x%x)", a); }
    glDisableClientState(a);
}

static void glEnable_probe(GLenum c) {
    if (c == 0xDE1) { unsigned u = g_active_unit - 0x84C0u; if (u < 4) g_tex2d_on[u] = 1; }
    static int budget = 30;
    if (budget > 0) { budget--; l_debug("glEnable(0x%x)", c); }
    glEnable(c);
}

static void glDisable_probe(GLenum c) {
    if (c == 0xDE1) { unsigned u = g_active_unit - 0x84C0u; if (u < 4) g_tex2d_on[u] = 0; }
    static int budget = 30;
    if (budget > 0) { budget--; l_debug("glDisable(0x%x)", c); }
    glDisable(c);
}


// Clipping: il gioco importa glScissor. Se lo scissor rect e' calcolato su una
// risoluzione diversa da 960x544, o con l'origine Y invertita, la UI viene
// tagliata via senza alcun errore GL. Mai sondato finora.
static void glScissor_probe(GLint x, GLint y, GLsizei w, GLsizei h) {
    static GLint lx = -1, ly = -1; static GLsizei lw = -1, lh = -1;
    if (x != lx || y != ly || w != lw || h != lh) {
        l_debug("glScissor(x=%i y=%i w=%i h=%i)", x, y, w, h);
        lx = x; ly = y; lw = w; lh = h;
    }
    glScissor(x, y, w, h);
}

static void glViewport_probe(GLint x, GLint y, GLsizei w, GLsizei h) {
    static GLint lx = -1, ly = -1; static GLsizei lw = -1, lh = -1;
    if (x != lx || y != ly || w != lw || h != lh) {
        l_debug("glViewport(x=%i y=%i w=%i h=%i)", x, y, w, h);
        lx = x; ly = y; lw = w; lh = h;
    }
    glViewport(x, y, w, h);
}

static void glOrthof_probe(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
    static int budget = 12;
    if (budget > 0) {
        budget--;
        l_debug("glOrthof(l=%i r=%i b=%i t=%i n=%i f=%i)",
                (int)l, (int)r, (int)b, (int)t, (int)n, (int)f);
    }
    glOrthof(l, r, b, t, n, f);
}


// Conta glClear ed eglSwapBuffers: se il gioco pulisce meno spesso di quanto
// scambia, sta assumendo un framebuffer preservato fra i frame.
static unsigned g_clears = 0, g_swaps = 0;

// Due glClear per ogni swap: il gioco fa due passate di rendering per frame.
// Il mask dice quale delle due tocca il colore — se la seconda pulisce il
// COLOR_BUFFER dopo aver disegnato, cancella quello appena messo a schermo.
// VERIFICATO: il gioco fa due glClear(GL_COLOR_BUFFER_BIT) per ogni
// eglSwapBuffers, quindi il primo disegno del frame viene cancellato dal
// secondo clear e non arriva mai a schermo.
// TEST: eseguo solo il primo clear di ogni frame, ignoro i successivi finche'
// non arriva lo swap. Se la UI compare, la causa e' confermata.
static int g_cleared_this_frame = 0;

static unsigned g_draws_since_clear = 0;
static unsigned g_ui_since_clear = 0;
static unsigned g_group = 0;

static void glClear_probe(GLbitfield mask) {
    g_clears++;
    // Registro cosa conteneva il gruppo di draw appena concluso.
    if (mask & 0x4000) {
        static int budget = 20;
        if (budget > 0 && g_group < 200) {
            budget--;
            l_debug("[CLEAR] gruppo chiuso: %u draw, di cui %u con texture UI",
                    g_draws_since_clear, g_ui_since_clear);
        }
        g_group++;
        g_draws_since_clear = 0;
        g_ui_since_clear = 0;
    }
    // TESTATO E SMENTITO: sopprimere il secondo clear del colore non fa
    // ricomparire la UI, quindi il primo disegno non e' quello perso.
    (void)g_cleared_this_frame;
    glClear(mask);
}

static unsigned g_draws = 0;

static void glDrawElements_probe(GLenum mode, GLsizei count, GLenum type, const void *idx) {
    g_draws++;
    g_draws_since_clear++;
    // "draw UI" = quad da 6 indici con texture attiva: sono le icone.
    if (count == 6 && g_tex2d_on[0] && g_bound_tex[0] != 0)
        g_ui_since_clear++;
    // Campiono solo le draw con array colori attivo e formato byte: sono quelle
    // che portano l'alpha al combiner.
    if (g_col_enabled && g_col_ptr && g_col_type == 0x1401 && g_col_size == 4) {
        static int budget = 25;
        if (budget > 0) {
            budget--;
            const unsigned char *c = g_col_ptr;
            unsigned t0 = g_bound_tex[0];
            l_debug("draw count=%i rgba=%u,%u,%u,%u | tex0=%u(%ix%i on=%i) tex1=%u(on=%i)",
                    count, c[0], c[1], c[2], c[3],
                    t0, t0 < 64 ? g_tex_w[t0] : -1, t0 < 64 ? g_tex_h[t0] : -1,
                    g_tex2d_on[0], g_bound_tex[1], g_tex2d_on[1]);
        }
    }
    glDrawElements(mode, count, type, idx);
}

static unsigned int eglSwapBuffers_probe(void *dpy, void *surf) {
    g_swaps++;
    g_cleared_this_frame = 0;
    {
        static int budget = 20;
        if (budget > 0 && g_group < 200) {
            budget--;
            l_debug("[SWAP ] ultimo gruppo: %u draw, di cui %u con texture UI  <-- QUESTO SI VEDE",
                    g_draws_since_clear, g_ui_since_clear);
        }
    }
    g_draws_since_clear = 0;
    g_ui_since_clear = 0;
    if (g_swaps == 30 || (g_swaps % 300) == 0)
        l_debug("frame %u: clear=%u swap=%u draw=%u (clear/frame %.2f, draw/frame %.1f)",
                g_swaps, g_clears, g_swaps, g_draws,
                (double)g_clears / (double)g_swaps, (double)g_draws / (double)g_swaps);
    return eglSwapBuffers(dpy, surf);
}


// TEST DECISIVO: leggere l'alpha reale nei vertici della UI.
// Il combiner prende l'alpha da GL_PRIMARY_COLOR, cioe' da questo array.
// alpha 0   -> e' il gioco a volere la UI invisibile (causa nella sua logica)
// alpha 255 -> il gioco la vuole opaca, qualcosa a valle la annulla (causa nel rendering)
static void glColorPointer_probe(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    g_col_ptr = (const unsigned char *)ptr;
    g_col_stride = stride ? stride : (GLsizei)(size * (type == 0x1401 ? 1 : 4));
    g_col_type = type;
    g_col_size = size;
    static GLint ls = -1; static GLenum lt = 0; static GLsizei lst = -1;
    if (size != ls || type != lt || stride != lst) {
        l_debug("glColorPointer(size=%i type=0x%x stride=%i)", size, type, stride);
        ls = size; lt = type; lst = stride;
    }
    glColorPointer(size, type, stride, ptr);
}

static void glBindTexture_probe(GLenum target, GLuint tex) {
    unsigned u = (g_active_unit - 0x84C0u);
    if (u < 4) g_bound_tex[u] = tex;
    glBindTexture(target, tex);
}

static void glActiveTexture_probe(GLenum unit) {
    g_active_unit = unit;
    glActiveTexture(unit);
}

static void glGenTextures_probe(GLsizei n, GLuint *out) {
    glGenTextures(n, out);
    static int budget = 14;
    if (budget > 0 && n > 0) {
        budget--;
        l_debug("glGenTextures(n=%i) -> primo id %u", n, out[0]);
    }
}

static void glDeleteTextures_probe(GLsizei n, const GLuint *ids) {
    static int budget = 14;
    if (budget > 0 && n > 0) {
        budget--;
        l_debug("glDeleteTextures(n=%i) primo id %u", n, ids[0]);
    }
    glDeleteTextures(n, ids);
}

static void glTexEnvfv_probe(GLenum target, GLenum pname, GLfloat *param) {
    static int budget = 10;
    if (budget > 0 && pname == 0x2201) {  // GL_TEXTURE_ENV_COLOR
        budget--;
        l_debug("costante combiner (unit %u): %i,%i,%i,%i (millesimi)",
                (unsigned)(g_active_unit - 0x84C0u),
                (int)(param[0]*1000), (int)(param[1]*1000),
                (int)(param[2]*1000), (int)(param[3]*1000));
    }
    glTexEnvfv(target, pname, param);
}

so_default_dynlib default_dynlib[] = {
        // Common C/C++ internals
        { "_ZNSt8bad_castD1Ev", (uintptr_t)&_ZNSt8bad_castD1Ev },
        { "_ZNSt9exceptionD2Ev", (uintptr_t)&_ZNSt9exceptionD2Ev },
        { "_ZSt17__throw_bad_allocv", (uintptr_t)&_ZSt17__throw_bad_allocv },
        { "_ZSt9terminatev", (uintptr_t)&_ZSt9terminatev },
        { "_ZTISt8bad_cast", (uintptr_t)&_ZTISt8bad_cast },
        { "_ZTISt9exception", (uintptr_t)&_ZTISt9exception },
        { "_ZTVN10__cxxabiv117__class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv117__class_type_infoE },
        { "_ZTVN10__cxxabiv120__si_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv120__si_class_type_infoE },
        { "_ZTVN10__cxxabiv121__vmi_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv121__vmi_class_type_infoE },
        { "_ZdaPv", (uintptr_t)&_ZdaPv },
        { "_ZdlPv", (uintptr_t)&_ZdlPv },
        { "_Znaj", (uintptr_t)&_Znaj },
        { "_Znwj", (uintptr_t)&_Znwj },
        { "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
        { "__aeabi_d2lz", (uintptr_t)&__aeabi_d2lz },
        { "__aeabi_d2ulz", (uintptr_t)&__aeabi_d2ulz },
        { "__aeabi_dadd", (uintptr_t)&__aeabi_dadd },
        { "__aeabi_dcmpgt", (uintptr_t)&__aeabi_dcmpgt },
        { "__aeabi_dcmplt", (uintptr_t)&__aeabi_dcmplt },
        { "__aeabi_ddiv", (uintptr_t)&__aeabi_ddiv },
        { "__aeabi_dmul", (uintptr_t)&__aeabi_dmul },
        { "__aeabi_f2lz", (uintptr_t)&__aeabi_f2lz },
        { "__aeabi_f2ulz", (uintptr_t)&__aeabi_f2ulz },
        { "__aeabi_i2d", (uintptr_t)&__aeabi_i2d },
        { "__aeabi_idiv", (uintptr_t)&__aeabi_idiv },
        { "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
        { "__aeabi_l2d", (uintptr_t)&__aeabi_l2d },
        { "__aeabi_l2f", (uintptr_t)&__aeabi_l2f },
        { "__aeabi_ldivmod", (uintptr_t)&__aeabi_ldivmod },
        { "__aeabi_memclr", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memclr4", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memclr8", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memcpy", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memcpy4", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memcpy8", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memmove", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memmove4", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memmove8", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memset", (uintptr_t)&__aeabi_memset },
        { "__aeabi_memset4",  (uintptr_t)&__aeabi_memset4 },
        { "__aeabi_memset8", (uintptr_t)&__aeabi_memset8 },
        { "__aeabi_ui2d", (uintptr_t)&__aeabi_ui2d },
        { "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv },
        { "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
        { "__aeabi_ul2d", (uintptr_t)&__aeabi_ul2d },
        { "__aeabi_ul2f", (uintptr_t)&__aeabi_ul2f },
        { "__aeabi_uldivmod", (uintptr_t)&__aeabi_uldivmod },
        { "__aeabi_unwind_cpp_pr0", (uintptr_t)&__aeabi_unwind_cpp_pr0 },
        { "__aeabi_unwind_cpp_pr1", (uintptr_t)&__aeabi_unwind_cpp_pr1 },
        { "__atomic_cmpxchg", (uintptr_t)&__atomic_cmpxchg },
        { "__atomic_dec", (uintptr_t)&__atomic_dec },
        { "__atomic_inc", (uintptr_t)&__atomic_inc },
        { "__atomic_swap", (uintptr_t)&__atomic_swap },
        { "__cxa_allocate_exception", (uintptr_t)&__cxa_allocate_exception },
        { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
        { "__cxa_begin_catch", (uintptr_t)&__cxa_begin_catch },
        { "__cxa_begin_cleanup", (uintptr_t)&ret0 },
        { "__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected },
        { "__cxa_end_catch", (uintptr_t)&__cxa_end_catch },
        { "__cxa_finalize", (uintptr_t)&__cxa_finalize },
        { "__cxa_free_exception", (uintptr_t)&__cxa_free_exception },
        { "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
        { "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
        { "__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual },
        { "__cxa_rethrow", (uintptr_t)&__cxa_rethrow },
        { "__cxa_throw", (uintptr_t)&__cxa_throw },
        { "__cxa_type_match", (uintptr_t)&ret0 },
        { "__gnu_Unwind_Find_exidx", (uintptr_t)&ret0 },
        { "__gnu_ldivmod_helper", (uintptr_t)&__gnu_ldivmod_helper },
        { "__gnu_unwind_frame", (uintptr_t)&__gnu_unwind_frame },
        { "__google_potentially_blocking_region_begin", (uintptr_t)&ret0 },
        { "__google_potentially_blocking_region_end", (uintptr_t)&ret0 },
        { "__gxx_personality_v0", (uintptr_t)&__gxx_personality_v0 },
        { "__isinf", (uintptr_t)&ret0 },
        { "__page_size", (uintptr_t)&__page_size },
        { "__sF", (uintptr_t)&__sF_fake },
        { "__srget", (uintptr_t)&__srget },
        { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_soloader },
        { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard },
        { "__swbuf", (uintptr_t)&__swbuf },
        { "__system_property_get", (uintptr_t)&__system_property_get_soloader },
        { "__assert2", (uintptr_t)&ret0 }, // TODO: stub/impl
        { "dl_unwind_find_exidx", (uintptr_t)&ret0 }, // TODO: stub/impl


        // ctype
        { "_ctype_", (uintptr_t)&BIONIC_ctype_ },
        { "_tolower_tab_", (uintptr_t)&BIONIC_tolower_tab_ },
        { "_toupper_tab_", (uintptr_t)&BIONIC_toupper_tab_ },
        { "isalnum", (uintptr_t)&isalnum },
        { "isalpha", (uintptr_t)&isalpha },
        { "isblank", (uintptr_t)&isblank },
        { "iscntrl", (uintptr_t)&iscntrl },
        { "isgraph", (uintptr_t)&isgraph },
        { "islower", (uintptr_t)&islower },
        { "isprint", (uintptr_t)&isprint },
        { "ispunct", (uintptr_t)&ispunct },
        { "isspace", (uintptr_t)&isspace },
        { "isupper", (uintptr_t)&isupper },
        { "isxdigit", (uintptr_t)&isxdigit },
        { "tolower", (uintptr_t)&tolower },
        { "toupper", (uintptr_t)&toupper },


        // Android SDK standard logging
        { "__android_log_assert", (uintptr_t)&__android_log_assert },
        { "__android_log_print", (uintptr_t)&__android_log_print },
        { "__android_log_vprint", (uintptr_t)&__android_log_vprint },
        { "__android_log_write", (uintptr_t)&__android_log_write },


        // AAssetManager
        { "AAsset_close", (uintptr_t)&AAsset_close },
        { "AAsset_getLength", (uintptr_t)&AAsset_getLength },
        { "AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength },
        { "AAsset_read", (uintptr_t)&AAsset_read },
        { "AAsset_seek", (uintptr_t)&AAsset_seek },
        { "AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor },
        { "AAssetDir_close", (uintptr_t)&AAssetDir_close },
        { "AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName },
        { "AAssetManager_fromJava", (uintptr_t)&ret1 },
        { "AAssetManager_open", (uintptr_t)&AAssetManager_open },
        { "AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir },

        
#ifdef NDK_PORT
        // FalsoNDK
        {"AConfiguration_delete", (uintptr_t)&AConfiguration_delete},
        {"AConfiguration_fromAssetManager", (uintptr_t)&AConfiguration_fromAssetManager},
        {"AConfiguration_getCountry", (uintptr_t)&AConfiguration_getCountry},
        {"AConfiguration_getDensity", (uintptr_t)&AConfiguration_getDensity},
        {"AConfiguration_getKeyboard", (uintptr_t)&AConfiguration_getKeyboard},
        {"AConfiguration_getKeysHidden", (uintptr_t)&AConfiguration_getKeysHidden},
        {"AConfiguration_getMcc", (uintptr_t)&AConfiguration_getMcc},
        {"AConfiguration_getMnc", (uintptr_t)&AConfiguration_getMnc},
        {"AConfiguration_getNavHidden", (uintptr_t)&AConfiguration_getNavHidden},
        {"AConfiguration_getNavigation", (uintptr_t)&AConfiguration_getNavigation},
        {"AConfiguration_getOrientation", (uintptr_t)&AConfiguration_getOrientation},
        {"AConfiguration_getScreenLong", (uintptr_t)&AConfiguration_getScreenLong},
        {"AConfiguration_getScreenSize", (uintptr_t)&AConfiguration_getScreenSize},
        {"AConfiguration_getSdkVersion", (uintptr_t)&AConfiguration_getSdkVersion},
        {"AConfiguration_getTouchscreen", (uintptr_t)&AConfiguration_getTouchscreen},
        {"AConfiguration_getUiModeNight", (uintptr_t)&AConfiguration_getUiModeNight},
        {"AConfiguration_getUiModeType", (uintptr_t)&AConfiguration_getUiModeType},
        {"AKeyEvent_getMetaState", (uintptr_t)&AKeyEvent_getMetaState_stub},
        {"AMotionEvent_getPointerId", (uintptr_t)&AMotionEvent_getPointerId},
        {"AConfiguration_getLanguage", (uintptr_t)&AConfiguration_getLanguage},
        {"AConfiguration_new", (uintptr_t)&AConfiguration_new},
        {"AInputEvent_getDeviceId", (uintptr_t)&AInputEvent_getDeviceId},
        {"AInputEvent_getSource", (uintptr_t)&AInputEvent_getSource},
        {"AInputEvent_getType", (uintptr_t)&AInputEvent_getType},
        {"AInputQueue_attachLooper", (uintptr_t)&AInputQueue_attachLooper},
        {"AInputQueue_detachLooper", (uintptr_t)&AInputQueue_detachLooper},
        {"AInputQueue_finishEvent", (uintptr_t)&AInputQueue_finishEvent},
        {"AInputQueue_getEvent", (uintptr_t)&AInputQueue_getEvent_probe},
        {"AInputQueue_hasEvents", (uintptr_t)&AInputQueue_hasEvents_probe},
        {"AInputQueue_preDispatchEvent", (uintptr_t)&AInputQueue_preDispatchEvent},
        {"AKeyEvent_getAction", (uintptr_t)&AKeyEvent_getAction},
        {"AKeyEvent_getKeyCode", (uintptr_t)&AKeyEvent_getKeyCode},
        {"ALooper_addFd", (uintptr_t)&ALooper_addFd},
        {"ALooper_pollAll", (uintptr_t)&ALooper_pollAll},
        {"ALooper_prepare", (uintptr_t)&ALooper_prepare},
        {"AMotionEvent_getAction", (uintptr_t)&AMotionEvent_getAction},
        {"AMotionEvent_getAxisValue", (uintptr_t)&AMotionEvent_getAxisValue},
        {"AMotionEvent_getPointerCount", (uintptr_t)&AMotionEvent_getPointerCount},
        {"AMotionEvent_getX", (uintptr_t)&AMotionEvent_getX},
        {"AMotionEvent_getY", (uintptr_t)&AMotionEvent_getY},
        {"ANativeActivity_finish", (uintptr_t)&ANativeActivity_finish},
        {"ANativeActivity_setWindowFlags", (uintptr_t)&ANativeActivity_setWindowFlags},
        {"ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight},
        {"ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth},
        {"ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry},
#endif


        // Math
        { "acos", (uintptr_t)&acos },
        { "acosf", (uintptr_t)&acosf },
        { "arc4random", (uintptr_t)&arc4random },
        { "asin", (uintptr_t)&asin },
        { "asinf", (uintptr_t)&asinf },
        { "atan", (uintptr_t)&atan },
        { "atan2", (uintptr_t)&atan2 },
        { "atan2f", (uintptr_t)&atan2f },
        { "atanf", (uintptr_t)&atanf },
        { "ceil", (uintptr_t)&ceil },
        { "ceilf", (uintptr_t)&ceilf },
        { "cos", (uintptr_t)&cos },
        { "cosf", (uintptr_t)&cosf },
        { "cosh", (uintptr_t)&cosh },
        { "exp", (uintptr_t)&exp },
        { "exp2", (uintptr_t)&exp2 },
        { "exp2f", (uintptr_t)&exp2f },
        { "expf", (uintptr_t)&expf },
        { "floor", (uintptr_t)&floor },
        { "floorf", (uintptr_t)&floorf },
        { "fmax", (uintptr_t)&fmax },
        { "fmaxf", (uintptr_t)&fmaxf },
        { "fmin", (uintptr_t)&fmin },
        { "fminf", (uintptr_t)&fminf },
        { "frexpf", (uintptr_t)&frexpf },
        { "fmod", (uintptr_t)&fmod },
        { "fmodf", (uintptr_t)&fmodf },
        { "frexp", (uintptr_t)&frexp },
        { "ldexp", (uintptr_t)&ldexp },
        { "ldexpf", (uintptr_t)&ldexpf },
        { "log", (uintptr_t)&log },
        { "log10", (uintptr_t)&log10 },
        { "log10f", (uintptr_t)&log10f },
        { "logf", (uintptr_t)&logf },
        { "lrint", (uintptr_t)&lrint },
        { "lrintf", (uintptr_t)&lrintf },
        { "lround", (uintptr_t)&lround },
        { "lroundf", (uintptr_t)&lroundf },
        { "modf", (uintptr_t)&modf },
        { "pow", (uintptr_t)&pow },
        { "powf", (uintptr_t)&powf },
        { "rint", (uintptr_t)&rint },
        { "rintf", (uintptr_t)&rintf },
        { "round", (uintptr_t)&round },
        { "roundf", (uintptr_t)&roundf },
        { "scalbn", (uintptr_t)&scalbn },
        { "scalbnf", (uintptr_t)&scalbnf },
        { "sin", (uintptr_t)&sin },
        { "sincos", (uintptr_t)&sincos },
        { "sincosf", (uintptr_t)&sincosf },
        { "sinf", (uintptr_t)&sinf },
        { "sinh", (uintptr_t)&sinh },
        { "sinhf", (uintptr_t)&sinhf },
        { "sqrt", (uintptr_t)&sqrt },
        { "sqrtf", (uintptr_t)&sqrtf },
        { "tan", (uintptr_t)&tan },
        { "tanf", (uintptr_t)&tanf },
        { "tanh", (uintptr_t)&tanh },
        { "trunc", (uintptr_t)&trunc },
        { "truncf", (uintptr_t)&truncf },


        // Sockets
        { "accept", (uintptr_t)&accept },
        { "bind", (uintptr_t)&bind },
        { "connect", (uintptr_t)&connect_probe },
        { "freeaddrinfo", (uintptr_t)&freeaddrinfo },
        { "gai_strerror", (uintptr_t)&ret0 },
        { "getaddrinfo", (uintptr_t)&getaddrinfo },
        { "gethostbyaddr", (uintptr_t)&gethostbyaddr },
        { "gethostbyname", (uintptr_t)&gethostbyname_probe },
        { "gethostname", (uintptr_t)&gethostname },
        { "getpeername", (uintptr_t)&getpeername },
        { "getservbyname", (uintptr_t)&getservbyname },
        { "getsockname", (uintptr_t)&getsockname },
        { "getsockopt", (uintptr_t)&getsockopt },
        { "inet_aton", (uintptr_t)&inet_aton },
        { "inet_pton", (uintptr_t)&inet_pton },
        { "inet_ntoa", (uintptr_t)&inet_ntoa },
        { "inet_ntop", (uintptr_t)&inet_ntop },
        { "listen", (uintptr_t)&listen },
        { "poll", (uintptr_t)&poll },
        { "recv", (uintptr_t)&recv },
        { "recvfrom", (uintptr_t)&recvfrom },
        { "recvmsg", (uintptr_t)&recvmsg },
        { "select", (uintptr_t)&select },
        { "send", (uintptr_t)&send },
        { "sendmsg", (uintptr_t)&sendmsg },
        { "sendto", (uintptr_t)&sendto },
        { "setsockopt", (uintptr_t)&setsockopt },
        { "shutdown", (uintptr_t)&shutdown },
        { "socket", (uintptr_t)&socket },


        // Memory
        { "calloc", (uintptr_t)&calloc },
        { "free", (uintptr_t)&free },
        { "malloc", (uintptr_t)&malloc },
        { "memalign", (uintptr_t)&memalign },
        { "memcmp", (uintptr_t)&memcmp },
        { "memcpy", (uintptr_t)&sceClibMemcpy },
        { "memmem", (uintptr_t)&memmem },
        { "memmove", (uintptr_t)&memmove },
        { "memset", (uintptr_t)&memset },
        { "mmap", (uintptr_t)&mmap },
        { "__mmap2", (uintptr_t)&mmap },
        { "munmap", (uintptr_t)&munmap },
        { "realloc", (uintptr_t)&realloc },
        { "valloc", (uintptr_t)&valloc },


        // IO
#ifndef NDK_PORT
        { "close", (uintptr_t)&close_soloader },
#else   
        { "close", (uintptr_t)&fndk_close },
#endif
        { "closedir", (uintptr_t)&closedir_soloader },
        { "execv", (uintptr_t)&ret0 },
        { "fclose", (uintptr_t)&fclose_soloader },
        { "fcntl", (uintptr_t)&fcntl_soloader },
        { "fopen", (uintptr_t)&fopen_soloader },
        { "fstat", (uintptr_t)&fstat_soloader },
        { "fsync", (uintptr_t)&fsync_soloader },
        { "ioctl", (uintptr_t)&ioctl_soloader },
        { "__open_2", (uintptr_t)&open_soloader },
        { "open", (uintptr_t)&open_soloader },
        { "opendir", (uintptr_t)&opendir_soloader },
        { "readdir", (uintptr_t)&readdir_soloader },
        { "readdir_r", (uintptr_t)&readdir_r_soloader },
        { "stat", (uintptr_t)&stat_soloader },
        { "utime", (uintptr_t)&utime },

        #ifdef USE_SCELIBC_IO
            { "fdopen", (uintptr_t)&sceLibcBridge_fdopen },
            { "feof", (uintptr_t)&sceLibcBridge_feof },
            { "ferror", (uintptr_t)&sceLibcBridge_ferror },
            { "fflush", (uintptr_t)&sceLibcBridge_fflush },
            { "fgetc", (uintptr_t)&sceLibcBridge_fgetc },
            { "fgetpos", (uintptr_t)&sceLibcBridge_fgetpos },
            { "fgets", (uintptr_t)&sceLibcBridge_fgets },
            { "fileno", (uintptr_t)&sceLibcBridge_fileno },
            { "fputc", (uintptr_t)&sceLibcBridge_fputc },
            { "fputs", (uintptr_t)&sceLibcBridge_fputs },
            { "fread", (uintptr_t)&sceLibcBridge_fread },
            { "freopen", (uintptr_t)&sceLibcBridge_freopen },
            { "fseek", (uintptr_t)&sceLibcBridge_fseek },
            { "fsetpos", (uintptr_t)&sceLibcBridge_fsetpos },
            { "ftell", (uintptr_t)&sceLibcBridge_ftell },
            { "fwide", (uintptr_t)&sceLibcBridge_fwide },
            { "fwrite", (uintptr_t)&sceLibcBridge_fwrite },
            { "getc", (uintptr_t)&sceLibcBridge_getc },
            { "getwc", (uintptr_t)&sceLibcBridge_getwc },
            { "putc", (uintptr_t)&sceLibcBridge_putc },
            { "putchar", (uintptr_t)&sceLibcBridge_putchar },
            { "puts", (uintptr_t)&sceLibcBridge_puts },
            { "putwc", (uintptr_t)&sceLibcBridge_putwc },
            { "setvbuf", (uintptr_t)&sceLibcBridge_setvbuf },
            { "ungetc", (uintptr_t)&sceLibcBridge_ungetc },
            { "ungetwc", (uintptr_t)&sceLibcBridge_ungetwc },
        #else
            { "fdopen", (uintptr_t)&fdopen },
            { "feof", (uintptr_t)&feof },
            { "ferror", (uintptr_t)&ferror },
            { "fflush", (uintptr_t)&fflush },
            { "fgetc", (uintptr_t)&fgetc },
            { "fgetpos", (uintptr_t)&fgetpos },
            { "fgets", (uintptr_t)&fgets },
            { "fileno", (uintptr_t)&fileno },
            { "fputc", (uintptr_t)&fputc },
            { "fputs", (uintptr_t)&fputs },
            { "fread", (uintptr_t)&fread },
            { "freopen", (uintptr_t)&freopen },
            { "fseek", (uintptr_t)&fseek },
            { "fsetpos", (uintptr_t)&fsetpos },
            { "ftell", (uintptr_t)&ftell },
            { "fwide", (uintptr_t)&fwide },
            { "fwrite", (uintptr_t)&fwrite },
            { "getc", (uintptr_t)&getc },
            { "getwc", (uintptr_t)&getwc },
            { "putc", (uintptr_t)&putc },
            { "putchar", (uintptr_t)&putchar },
            { "puts", (uintptr_t)&puts },
            { "putwc", (uintptr_t)&putwc },
            { "setvbuf", (uintptr_t)&setvbuf },
            { "ungetc", (uintptr_t)&ungetc },
            { "ungetwc", (uintptr_t)&ungetwc },
        #endif

        { "access", (uintptr_t)&access },
        { "basename", (uintptr_t)&basename },
        { "chdir", (uintptr_t)&chdir },
        { "chmod", (uintptr_t)&chmod },
        { "dup", (uintptr_t)&dup },
        { "fseeko", (uintptr_t)&fseeko }, // TODO: wrap normal fseek for SceLibc version?
        { "ftello", (uintptr_t)&ftello },
        { "ftruncate", (uintptr_t)&ftruncate },
        { "getcwd", (uintptr_t)&getcwd },
        { "lseek", (uintptr_t)&lseek },
        { "lseek64", (uintptr_t)&ret0 }, // TODO: implement or stub with warning
        { "lstat", (uintptr_t)&lstat },
        { "mkdir", (uintptr_t)&mkdir_soloader },
#ifndef NDK_PORT
        { "pipe", (uintptr_t)&pipe },
        { "read", (uintptr_t)&read },
#else
        { "pipe", (uintptr_t)&fndk_pipe },
        { "read", (uintptr_t)&fndk_read },
#endif
        { "realpath", (uintptr_t)&realpath },
        { "remove", (uintptr_t)&remove },
        { "rename", (uintptr_t)&rename },
        { "rewind", (uintptr_t)&rewind },
        { "rmdir", (uintptr_t)&rmdir },
        { "truncate", (uintptr_t)&truncate },
        { "unlink", (uintptr_t)&unlink },
#ifndef NDK_PORT
        { "write", (uintptr_t)&write },
#else
        { "write", (uintptr_t)&fndk_write },
#endif


        // *printf, *scanf
        { "snprintf", (uintptr_t)&snprintf },
        { "sprintf", (uintptr_t)&sprintf },
        { "vasprintf", (uintptr_t)&vasprintf },
        { "vprintf", (uintptr_t)&vprintf },
        { "vsnprintf", (uintptr_t)&vsnprintf },
        { "vsprintf", (uintptr_t)&vsprintf },
        { "vsscanf", (uintptr_t)&vsscanf },
        { "vswprintf", (uintptr_t)&vswprintf },
        { "printf", (uintptr_t)&sceClibPrintf },
        { "swprintf", (uintptr_t)&swprintf },

        #ifdef USE_SCELIBC_IO
            { "fprintf", (uintptr_t)&sceLibcBridge_fprintf },
            { "fscanf", (uintptr_t)&sceLibcBridge_fscanf },
            { "sscanf", (uintptr_t)&sceLibcBridge_sscanf },
            { "vfprintf", (uintptr_t)&sceLibcBridge_vfprintf },
        #else
            { "fprintf", (uintptr_t)&fprintf },
            { "fscanf", (uintptr_t)&fscanf },
            { "sscanf", (uintptr_t)&sscanf },
            { "vfprintf", (uintptr_t)&vfprintf },
        #endif


        // EGL
        { "eglBindAPI", (uintptr_t)&eglBindAPI },
        { "eglChooseConfig", (uintptr_t)&eglChooseConfig },
        { "eglCreateContext", (uintptr_t)&eglCreateContext },
        { "eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface },
        { "eglDestroyContext", (uintptr_t)&eglDestroyContext },
        { "eglDestroySurface", (uintptr_t)&eglDestroySurface },
        { "eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib },
        { "eglGetConfigs", (uintptr_t)&eglGetConfigs },
        { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
        { "eglGetDisplay", (uintptr_t)&eglGetDisplay },
        { "eglGetError", (uintptr_t)&eglGetError },
        { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },
        { "eglInitialize", (uintptr_t)&eglInitialize },
        { "eglMakeCurrent", (uintptr_t)&eglMakeCurrent },
        { "eglQueryContext", (uintptr_t)&eglQueryContext },
        { "eglQueryString", (uintptr_t)&eglQueryString },
        { "eglQuerySurface", (uintptr_t)&eglQuerySurface },
        { "eglSwapBuffers", (uintptr_t)&eglSwapBuffers_probe },
        { "eglTerminate", (uintptr_t)&eglTerminate },


        // OpenGL
        { "glActiveTexture", (uintptr_t)&glActiveTexture_probe },
        { "glAlphaFunc", (uintptr_t)&glAlphaFunc },
        { "glAlphaFuncx", (uintptr_t)&glAlphaFuncx },
        { "glAttachShader", (uintptr_t)&glAttachShader },
        { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
        { "glBindBuffer", (uintptr_t)&glBindBuffer },
        { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
        { "glBindFramebufferOES", (uintptr_t)&glBindFramebufferOES_probe },
        { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer },
        { "glBindRenderbufferOES", (uintptr_t)&glBindRenderbuffer },
        { "glBindTexture", (uintptr_t)&glBindTexture_probe },
        { "glBlendColor", (uintptr_t)&ret0 },
        { "glBlendEquation", (uintptr_t)&glBlendEquation },
        { "glBlendEquationOES", (uintptr_t)&glBlendEquation },
        { "glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate },
        { "glBlendEquationSeparateOES", (uintptr_t)&glBlendEquationSeparate },
        { "glBlendFunc", (uintptr_t)&glBlendFunc },
        { "glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate },
        { "glBlendFuncSeparateOES", (uintptr_t)&glBlendFuncSeparate },
        { "glBufferData", (uintptr_t)&glBufferData },
        { "glBufferSubData", (uintptr_t)&glBufferSubData },
        { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus },
        { "glCheckFramebufferStatusOES", (uintptr_t)&glCheckFramebufferStatusOES_probe },
        { "glClear", (uintptr_t)&glClear_probe },
        { "glClearColor", (uintptr_t)&glClearColor },
        { "glClearColorx", (uintptr_t)&glClearColorx },
        { "glClearDepthf", (uintptr_t)&glClearDepthf },
        { "glClearDepthx", (uintptr_t)&glClearDepthx },
        { "glClearStencil", (uintptr_t)&glClearStencil },
        { "glClientActiveTexture", (uintptr_t)&glClientActiveTexture },
        { "glClipPlanef", (uintptr_t)&glClipPlanef },
        { "glClipPlanex", (uintptr_t)&glClipPlanex },
        { "glColor4f", (uintptr_t)&glColor4f },
        { "glColor4ub", (uintptr_t)&glColor4ub },
        { "glColor4x", (uintptr_t)&glColor4x },
        { "glColorMask", (uintptr_t)&glColorMask },
        { "glColorPointer", (uintptr_t)&glColorPointer_probe },
        { "glCompileShader", (uintptr_t)&glCompileShader_soloader },
        { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D_probe },
        { "glCompressedTexSubImage2D", (uintptr_t)&ret0 },
        { "glCopyTexImage2D", (uintptr_t)&glCopyTexImage2D },
        { "glCopyTexSubImage2D", (uintptr_t)&glCopyTexSubImage2D },
        { "glCreateProgram", (uintptr_t)&glCreateProgram },
        { "glCreateShader", (uintptr_t)&glCreateShader },
        { "glCullFace", (uintptr_t)&glCullFace },
        { "glCurrentPaletteMatrixOES", (uintptr_t)&ret0 },
        { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
        { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
        { "glDeleteFramebuffersOES", (uintptr_t)&glDeleteFramebuffers },
        { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
        { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers },
        { "glDeleteRenderbuffersOES", (uintptr_t)&glDeleteRenderbuffers },
        { "glDeleteShader", (uintptr_t)&glDeleteShader },
        { "glDeleteTextures", (uintptr_t)&glDeleteTextures_probe },
        { "glDepthFunc", (uintptr_t)&glDepthFunc },
        { "glDepthMask", (uintptr_t)&glDepthMask },
        { "glDepthRangef", (uintptr_t)&glDepthRangef },
        { "glDepthRangex", (uintptr_t)&glDepthRangex },
        { "glDetachShader", (uintptr_t)&ret0 },
        { "glDisable", (uintptr_t)&glDisable_probe },
        { "glDisableClientState", (uintptr_t)&glDisableClientState_probe },
        { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
        { "glDrawArrays", (uintptr_t)&glDrawArrays },
        { "glDrawElements", (uintptr_t)&glDrawElements_probe },
        { "glDrawTexfOES", (uintptr_t)&ret0 },
        { "glDrawTexfvOES", (uintptr_t)&ret0 },
        { "glDrawTexiOES", (uintptr_t)&ret0 },
        { "glDrawTexivOES", (uintptr_t)&ret0 },
        { "glDrawTexsOES", (uintptr_t)&ret0 },
        { "glDrawTexsvOES", (uintptr_t)&ret0 },
        { "glDrawTexxOES", (uintptr_t)&ret0 },
        { "glDrawTexxvOES", (uintptr_t)&ret0 },
        { "glEGLImageTargetRenderbufferStorageOES", (uintptr_t)&ret0 },
        { "glEGLImageTargetTexture2DOES", (uintptr_t)&ret0 },
        { "glEnable", (uintptr_t)&glEnable_probe },
        { "glEnableClientState", (uintptr_t)&glEnableClientState_probe },
        { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
        { "glFinish", (uintptr_t)&glFinish },
        { "glFlush", (uintptr_t)&glFlush },
        { "glFogf", (uintptr_t)&glFogf },
        { "glFogfv", (uintptr_t)&glFogfv },
        { "glFogx", (uintptr_t)&glFogx },
        { "glFogxv", (uintptr_t)&glFogxv },
        { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
        { "glFramebufferRenderbufferOES", (uintptr_t)&glFramebufferRenderbuffer },
        { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
        { "glFramebufferTexture2DOES", (uintptr_t)&glFramebufferTexture2DOES_probe },
        { "glFrontFace", (uintptr_t)&glFrontFace },
        { "glFrustumf", (uintptr_t)&glFrustumf },
        { "glFrustumx", (uintptr_t)&glFrustumx },
        { "glGenBuffers", (uintptr_t)&glGenBuffers },
        { "glGenerateMipmap", (uintptr_t)&glGenerateMipmap },
        { "glGenerateMipmapOES", (uintptr_t)&glGenerateMipmap },
        { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
        { "glGenFramebuffersOES", (uintptr_t)&glGenFramebuffers },
        { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
        { "glGenRenderbuffersOES", (uintptr_t)&glGenRenderbuffers },
        { "glGenTextures", (uintptr_t)&glGenTextures_probe },
        { "glGetActiveAttrib", (uintptr_t)&glGetActiveAttrib },
        { "glGetActiveUniform", (uintptr_t)&glGetActiveUniform },
        { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
        { "glGetBooleanv", (uintptr_t)&glGetBooleanv },
        { "glGetBufferParameteriv", (uintptr_t)&glGetBufferParameteriv },
        { "glGetBufferPointervOES", (uintptr_t)&ret0 },
        { "glGetClipPlanef", (uintptr_t)&ret0 },
        { "glGetClipPlanex", (uintptr_t)&ret0 },
        { "glGetError", (uintptr_t)&glGetError },
        { "glGetFixedv", (uintptr_t)&ret0 },
        { "glGetFloatv", (uintptr_t)&glGetFloatv },
        { "glGetFramebufferAttachmentParameterivOES", (uintptr_t)&glGetFramebufferAttachmentParameteriv },
        { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
        { "glGetLightfv", (uintptr_t)&ret0 },
        { "glGetLightxv", (uintptr_t)&ret0 },
        { "glGetMaterialfv", (uintptr_t)&ret0 },
        { "glGetMaterialxv", (uintptr_t)&ret0 },
        { "glGetPointerv", (uintptr_t)&ret0 },
        { "glGetRenderbufferParameterivOES", (uintptr_t)&ret0 },
        { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
        { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
        { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },
        { "glGetShaderSource", (uintptr_t)&glGetShaderSource },
        { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
        { "glGetString", (uintptr_t)&glGetString_probe },
        { "glGetTexEnvfv", (uintptr_t)&ret0 },
        { "glGetTexEnviv", (uintptr_t)&glGetTexEnviv },
        { "glGetTexEnvxv", (uintptr_t)&ret0 },
        { "glGetTexGenfvOES", (uintptr_t)&ret0 },
        { "glGetTexGenivOES", (uintptr_t)&ret0 },
        { "glGetTexGenxvOES", (uintptr_t)&ret0 },
        { "glGetTexParameterfv", (uintptr_t)&ret0 },
        { "glGetTexParameteriv", (uintptr_t)&ret0 },
        { "glGetTexParameterxv", (uintptr_t)&ret0 },
        { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
        { "glHint", (uintptr_t)&glHint },
        { "glIsBuffer", (uintptr_t)&ret0 },
        { "glIsRenderbuffer", (uintptr_t)&glIsRenderbuffer },
        { "glIsEnabled", (uintptr_t)&glIsEnabled },
        { "glIsFramebufferOES", (uintptr_t)&glIsFramebuffer },
        { "glIsRenderbufferOES", (uintptr_t)&glIsRenderbuffer },
        { "glIsTexture", (uintptr_t)&glIsTexture },
        { "glLightf", (uintptr_t)&ret0 },
        { "glLightfv", (uintptr_t)&glLightfv },
        { "glLightModelf", (uintptr_t)&ret0 },
        { "glLightModelfv", (uintptr_t)&glLightModelfv },
        { "glLightModelx", (uintptr_t)&ret0 },
        { "glLightModelxv", (uintptr_t)&glLightModelxv },
        { "glLightx", (uintptr_t)&ret0 },
        { "glLightxv", (uintptr_t)&glLightxv },
        { "glLineWidth", (uintptr_t)&glLineWidth },
        { "glLineWidthx", (uintptr_t)&glLineWidthx },
        { "glLinkProgram", (uintptr_t)&glLinkProgram },
        { "glLoadIdentity", (uintptr_t)&glLoadIdentity },
        { "glLoadMatrixf", (uintptr_t)&glLoadMatrixf },
        { "glLoadMatrixx", (uintptr_t)&glLoadMatrixx },
        { "glLoadPaletteFromModelViewMatrixOES", (uintptr_t)&ret0 },
        { "glLogicOp", (uintptr_t)&ret0 },
        { "glMapBuffer", (uintptr_t)&glMapBuffer },
        { "glMapBufferOES", (uintptr_t)&glMapBuffer },
        { "glMaterialf", (uintptr_t)&glMaterialf },
        { "glMaterialfv", (uintptr_t)&glMaterialfv },
        { "glMaterialx", (uintptr_t)&glMaterialx },
        { "glMaterialxv", (uintptr_t)&glMaterialxv },
        { "glMatrixIndexPointerOES", (uintptr_t)&ret0 },
        { "glMatrixMode", (uintptr_t)&glMatrixMode },
        { "glMultiTexCoord4f", (uintptr_t)&ret0 },
        { "glMultiTexCoord4x", (uintptr_t)&ret0},
        { "glMultMatrixf", (uintptr_t)&glMultMatrixf },
        { "glMultMatrixx", (uintptr_t)&glMultMatrixx },
        { "glNormal3f", (uintptr_t)&glNormal3f },
        { "glNormal3x", (uintptr_t)&glNormal3x },
        { "glNormalPointer", (uintptr_t)&glNormalPointer },
        { "glOrthof", (uintptr_t)&glOrthof_probe },
        { "glOrthox", (uintptr_t)&glOrthox },
        { "glPixelStorei", (uintptr_t)&glPixelStorei },
        { "glPointParameterf", (uintptr_t)&ret0 },
        { "glPointParameterfv", (uintptr_t)&ret0 },
        { "glPointParameterx", (uintptr_t)&ret0 },
        { "glPointParameterxv", (uintptr_t)&ret0 },
        { "glPointSize", (uintptr_t)&glPointSize },
        { "glPointSizePointerOES", (uintptr_t)&ret0 },
        { "glPointSizex", (uintptr_t)&glPointSizex },
        { "glPolygonOffset", (uintptr_t)&glPolygonOffset },
        { "glPolygonOffsetx", (uintptr_t)&glPolygonOffsetx },
        { "glPopMatrix", (uintptr_t)&glPopMatrix },
        { "glPushMatrix", (uintptr_t)&glPushMatrix },
        { "glQueryMatrixxOES", (uintptr_t)&ret0 },
        { "glReadPixels", (uintptr_t)&glReadPixels },
        { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage },
        { "glRenderbufferStorageOES", (uintptr_t)&glRenderbufferStorageOES_probe },
        { "glRotatef", (uintptr_t)&glRotatef },
        { "glRotatex", (uintptr_t)&glRotatex },
        { "glSampleCoverage", (uintptr_t)&ret0 },
        { "glSampleCoveragex", (uintptr_t)&ret0 },
        { "glScalef", (uintptr_t)&glScalef },
        { "glScalex", (uintptr_t)&glScalex },
        { "glScissor", (uintptr_t)&glScissor_probe },
        { "glShadeModel", (uintptr_t)&glShadeModel },
        { "glShaderSource", (uintptr_t)&glShaderSource_soloader },
        { "glStencilFunc", (uintptr_t)&glStencilFunc },
        { "glStencilFuncSeparate", (uintptr_t)&glStencilFuncSeparate },
        { "glStencilMask", (uintptr_t)&glStencilMask },
        { "glStencilOp", (uintptr_t)&glStencilOp },
        { "glStencilOpSeparate", (uintptr_t)&glStencilOpSeparate },
        { "glTexCoordPointer", (uintptr_t)&glTexCoordPointer },
        { "glTexEnvf", (uintptr_t)&glTexEnvf },
        { "glTexEnvfv", (uintptr_t)&glTexEnvfv_probe },
        { "glTexEnvi", (uintptr_t)&glTexEnvi_probe },
        { "glTexEnviv", (uintptr_t)&ret0 },
        { "glTexEnvx", (uintptr_t)&glTexEnvx },
        { "glTexEnvxv", (uintptr_t)&glTexEnvxv },
        { "glTexGenfOES", (uintptr_t)&ret0 },
        { "glTexGenfvOES", (uintptr_t)&ret0 },
        { "glTexGeniOES", (uintptr_t)&ret0 },
        { "glTexGenivOES", (uintptr_t)&ret0 },
        { "glTexGenxOES", (uintptr_t)&ret0 },
        { "glTexGenxvOES", (uintptr_t)&ret0 },
        { "glTexImage2D", (uintptr_t)&glTexImage2D_probe },
        { "glTexParameterf", (uintptr_t)&glTexParameterf },
        { "glTexParameterfv", (uintptr_t)&ret0 },
        { "glTexParameteri", (uintptr_t)&glTexParameteri },
        { "glTexParameteriv", (uintptr_t)&glTexParameteriv },
        { "glTexParameterx", (uintptr_t)&glTexParameterx },
        { "glTexParameterxv", (uintptr_t)&ret0 },
        { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D },
        { "glTranslatef", (uintptr_t)&glTranslatef },
        { "glTranslatex", (uintptr_t)&glTranslatex },
        { "glUniform1f", (uintptr_t)&glUniform1f },
        { "glUniform1fv", (uintptr_t)&glUniform1fv },
        { "glUniform1i", (uintptr_t)&glUniform1i },
        { "glUniform1iv", (uintptr_t)&glUniform1iv },
        { "glUniform2i", (uintptr_t)&glUniform2i },
        { "glUniform2f", (uintptr_t)&glUniform2f },
        { "glUniform2fv", (uintptr_t)&glUniform2fv },
        { "glUniform2iv", (uintptr_t)&glUniform2iv },
        { "glUniform3i", (uintptr_t)&glUniform3i },
        { "glUniform3f", (uintptr_t)&glUniform3f },
        { "glUniform3fv", (uintptr_t)&glUniform3fv },
        { "glUniform3iv", (uintptr_t)&glUniform3iv },
        { "glUniform4i", (uintptr_t)&glUniform4i },
        { "glUniform4f", (uintptr_t)&glUniform4f },
        { "glUniform4fv", (uintptr_t)&glUniform4fv },
        { "glUniform4iv", (uintptr_t)&glUniform4iv },
        { "glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fv },
        { "glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv },
        { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
        { "glUnmapBuffer", (uintptr_t)&glUnmapBuffer },
        { "glUnmapBufferOES", (uintptr_t)&glUnmapBuffer },
        { "glUseProgram", (uintptr_t)&glUseProgram },
        { "glValidateProgram", (uintptr_t)&ret0 },
        { "glVertexAttrib4f", (uintptr_t)&glVertexAttrib4f },
        { "glVertexAttrib4fv", (uintptr_t)&glVertexAttrib4fv },
        { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
        { "glVertexPointer", (uintptr_t)&glVertexPointer },
        { "glViewport", (uintptr_t)&glViewport_probe },
        { "glWeightPointerOES", (uintptr_t)&ret0 },




        // Pthread
        { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
        { "pthread_attr_init", (uintptr_t) &pthread_attr_init_soloader },
        { "pthread_attr_setdetachstate", (uintptr_t) &pthread_attr_setdetachstate_soloader },
        { "pthread_attr_setstacksize", (uintptr_t) &pthread_attr_setstacksize_soloader },
        { "pthread_attr_setschedparam", (uintptr_t) &ret0 },

        { "pthread_cond_broadcast", (uintptr_t) &pthread_cond_broadcast_soloader },
        { "pthread_cond_destroy", (uintptr_t) &pthread_cond_destroy_soloader },
        { "pthread_cond_init", (uintptr_t) &pthread_cond_init_soloader },
        { "pthread_cond_signal", (uintptr_t) &pthread_cond_signal_soloader },
        { "pthread_cond_timedwait", (uintptr_t) &pthread_cond_timedwait_soloader },
        { "pthread_cond_wait", (uintptr_t) &pthread_cond_wait_soloader },

        { "pthread_create", (uintptr_t) &pthread_create_probe },
        { "pthread_detach", (uintptr_t) &pthread_detach_soloader },
        { "pthread_equal", (uintptr_t) &pthread_equal_soloader },
        { "pthread_exit", (uintptr_t)&pthread_exit },
        { "pthread_getschedparam", (uintptr_t) &pthread_getschedparam_soloader },
        { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
        { "pthread_join", (uintptr_t) &pthread_join_soloader },
        { "pthread_key_create", (uintptr_t)&pthread_key_create },
        { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
        { "pthread_kill", (uintptr_t)&pthread_kill_soloader },

        { "pthread_mutex_destroy", (uintptr_t) &pthread_mutex_destroy_soloader },
        { "pthread_mutex_init", (uintptr_t) &pthread_mutex_init_soloader },
        { "pthread_mutex_lock", (uintptr_t) &pthread_mutex_lock_soloader },
        { "pthread_mutex_trylock", (uintptr_t) &pthread_mutex_trylock_soloader },
        { "pthread_mutex_unlock", (uintptr_t) &pthread_mutex_unlock_soloader },
        { "pthread_mutexattr_destroy", (uintptr_t) &pthread_mutexattr_destroy_soloader },
        { "pthread_mutexattr_init", (uintptr_t) &pthread_mutexattr_init_soloader },
        { "pthread_mutexattr_settype", (uintptr_t) &pthread_mutexattr_settype_soloader },
        { "pthread_mutexattr_setpshared", (uintptr_t) &ret0 },
        { "pthread_once", (uintptr_t)&pthread_once_soloader },

        { "pthread_self", (uintptr_t) &pthread_self_soloader },
        { "pthread_setname_np", (uintptr_t) &pthread_setname_np_soloader },
        { "pthread_setschedparam", (uintptr_t) &pthread_setschedparam_soloader },
        { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
        { "pthread_sigmask", (uintptr_t)&ret0 },

        { "sem_destroy", (uintptr_t) &sem_destroy_soloader },
        { "sem_getvalue", (uintptr_t) &sem_getvalue_soloader },
        { "sem_init", (uintptr_t) &sem_init_soloader },
        { "sem_post", (uintptr_t) &sem_post_soloader },
        { "sem_timedwait", (uintptr_t) &sem_timedwait_soloader },
        { "sem_trywait", (uintptr_t) &sem_trywait_soloader },
        { "sem_wait", (uintptr_t) &sem_wait_soloader },

        { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max },
        { "sched_get_priority_min", (uintptr_t)&sched_get_priority_min },
        { "sched_yield", (uintptr_t)&sched_yield },


        // wchar, wctype
        { "btowc", (uintptr_t)&btowc },
        { "iswalpha", (uintptr_t)&iswalpha },
        { "iswcntrl", (uintptr_t)&iswcntrl },
        { "iswctype", (uintptr_t)&iswctype },
        { "iswdigit", (uintptr_t)&iswdigit },
        { "iswdigit", (uintptr_t)&iswdigit },
        { "iswlower", (uintptr_t)&iswlower },
        { "iswprint", (uintptr_t)&iswprint },
        { "iswpunct", (uintptr_t)&iswpunct },
        { "iswspace", (uintptr_t)&iswspace },
        { "iswupper", (uintptr_t)&iswupper },
        { "iswxdigit", (uintptr_t)&iswxdigit },
        { "mbrlen", (uintptr_t)&mbrlen },
        { "mbrtowc", (uintptr_t)&mbrtowc },
        { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs },
        { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
        { "mbstowcs", (uintptr_t)&mbstowcs },
        { "mbtowc", (uintptr_t)&mbtowc },
        { "towlower", (uintptr_t)&towlower },
        { "towupper", (uintptr_t)&towupper },
        { "wcrtomb", (uintptr_t)&wcrtomb },
        { "wcscasecmp", (uintptr_t)&wcscasecmp },
        { "wcscmp", (uintptr_t)&wcscmp },
        { "wcscoll", (uintptr_t)&wcscoll },
        { "wcscpy", (uintptr_t)&wcscpy },
        { "wcsftime", (uintptr_t)&wcsftime },
        { "wcslcat", (uintptr_t)&wcslcat },
        { "wcslcpy", (uintptr_t)&wcslcpy },
        { "wcslen", (uintptr_t)&wcslen },
        { "wcsncasecmp", (uintptr_t)&wcsncasecmp },
        { "wcsncmp", (uintptr_t)&wcsncmp },
        { "wcsncpy", (uintptr_t)&wcsncpy },
        { "wcsnlen", (uintptr_t)&wcsnlen },
        { "wcsnrtombs", (uintptr_t)&wcsnrtombs },
        { "wcsstr", (uintptr_t)&wcsstr },
        { "wcstod", (uintptr_t)&wcstod },
        { "wcstof", (uintptr_t)&wcstof },
        { "wcstol", (uintptr_t)&wcstol },
        { "wcstoll", (uintptr_t)&wcstoll },
        { "wcstombs", (uintptr_t)&wcstombs },
        { "wcstoul", (uintptr_t)&wcstoul },
        { "wcstoull", (uintptr_t)&wcstoull },
        { "wcsxfrm", (uintptr_t)&wcsxfrm },
        { "wctob", (uintptr_t)&wctob },
        { "wctype", (uintptr_t)&wctype },
        { "wmemchr", (uintptr_t)&wmemchr },
        { "wmemcmp", (uintptr_t)&wmemcmp },
        { "wmemcpy", (uintptr_t)&wmemcpy },
        { "wmemmove", (uintptr_t)&wmemmove },
        { "wmemset", (uintptr_t)&wmemset },


        // libdl
        { "dlclose", (uintptr_t)&ret0 },
        { "dlerror", (uintptr_t)&ret0 },
        { "dlopen", (uintptr_t)&ret1 },
        { "dlsym", (uintptr_t)&dlsym_soloader },


        // Errno
        { "__errno", (uintptr_t)&__errno_soloader },
        { "strerror", (uintptr_t)&strerror_soloader },
        { "strerror_r", (uintptr_t)&strerror_r_soloader },
        { "perror", (uintptr_t)&perror }, // TODO: errno translation


        // Strings
        { "memchr", (uintptr_t)&memchr },
        { "memrchr", (uintptr_t)&memrchr },
        { "strcasecmp", (uintptr_t)&strcasecmp },
        { "strcat", (uintptr_t)&strcat },
        { "strchr", (uintptr_t)&strchr },
        { "strcmp", (uintptr_t)&strcmp },
        { "strcoll", (uintptr_t)&strcoll },
        { "strcpy", (uintptr_t)&strcpy },
        { "strcspn", (uintptr_t)&strcspn },
        { "strdup", (uintptr_t)&strdup },
        { "strlcat", (uintptr_t)&strlcat },
        { "strlcpy", (uintptr_t)&strlcpy },
        { "strlen", (uintptr_t)&strlen },
        { "strncasecmp", (uintptr_t)&strncasecmp },
        { "strncat", (uintptr_t)&strncat },
        { "strncmp", (uintptr_t)&strncmp },
        { "strncpy", (uintptr_t)&strncpy },
        { "strnlen", (uintptr_t)&strnlen },
        { "strpbrk", (uintptr_t)&strpbrk },
        { "strrchr", (uintptr_t)&strrchr },
        { "strspn", (uintptr_t)&strspn },
        { "strstr", (uintptr_t)&strstr },
        { "strtok", (uintptr_t)&strtok },
        { "strtok_r", (uintptr_t)&strtok_r },
        { "strxfrm", (uintptr_t)&strxfrm },


        // Syscalls
        { "fork", (uintptr_t)&fork },
        { "getpagesize", (uintptr_t)&getpagesize },
        { "getpid", (uintptr_t)&getpid },
        { "sbrk", (uintptr_t)&sbrk },
        { "syscall", (uintptr_t)&syscall },
        { "sysconf", (uintptr_t)&ret0 },
        { "system", (uintptr_t)&system },
        { "waitpid", (uintptr_t)&ret0 },


        // Time
        { "clock", (uintptr_t)&clock_soloader },
        { "clock_getres", (uintptr_t)&clock_getres_soloader },
        { "clock_gettime", (uintptr_t)&clock_gettime_soloader },
        { "difftime", (uintptr_t)&difftime },
        { "gettimeofday", (uintptr_t)&gettimeofday_soloader },
        { "gmtime", (uintptr_t)&gmtime },
        { "gmtime64", (uintptr_t)&gmtime64 },
        { "gmtime_r", (uintptr_t)&gmtime_r },
        { "localtime", (uintptr_t)&localtime },
        { "localtime64", (uintptr_t)&localtime64 },
        { "localtime_r", (uintptr_t)&localtime_r },
        { "mktime", (uintptr_t)&mktime },
        { "mktime64", (uintptr_t)&mktime64 },
        { "nanosleep", (uintptr_t)&nanosleep },
        { "strftime", (uintptr_t)&strftime },
        { "time", (uintptr_t)&time },
        { "tzset", (uintptr_t)&tzset },


        // Temp
        { "mkstemp", (uintptr_t)&mkstemp },
        { "coshf", (uintptr_t)&coshf },
        { "hypot", (uintptr_t)&hypot },
        { "mallinfo", (uintptr_t)&mallinfo_stub },
        { "mprotect", (uintptr_t)&mprotect_stub },
        { "mremap", (uintptr_t)&mremap_stub },
        { "mktemp", (uintptr_t)&mktemp },
        { "tmpfile", (uintptr_t)&tmpfile },
        { "tmpnam", (uintptr_t)&tmpnam },


        // stdlib
        { "abort", (uintptr_t)&abort_soloader },
        { "atof", (uintptr_t)&atof },
        { "atoi", (uintptr_t)&atoi },
        { "atol", (uintptr_t)&atol },
        { "atoll", (uintptr_t)&atoll },
        { "bsearch", (uintptr_t)&bsearch },
        { "exit", (uintptr_t)&exit_soloader },
        { "lrand48", (uintptr_t)&lrand48 },
        { "prctl", (uintptr_t)&ret0 },
        { "sleep", (uintptr_t)&sleep },
        { "srand48", (uintptr_t)&srand48 },
        { "strtod", (uintptr_t)&strtod },
        { "strtof", (uintptr_t)&strtof },
        { "strtoimax", (uintptr_t)&strtoimax },
        { "strtol", (uintptr_t)&strtol },
        { "strtold", (uintptr_t)&strtold },
        { "strtoll", (uintptr_t)&strtoll },
        { "strtoul", (uintptr_t)&strtoul },
        { "strtoull", (uintptr_t)&strtoull },
        { "strtoumax", (uintptr_t)&strtoumax },
        { "usleep", (uintptr_t)&usleep },

        #ifdef USE_SCELIBC_IO
            { "qsort", (uintptr_t)&sceLibcBridge_qsort },
            { "rand", (uintptr_t)&sceLibcBridge_rand },
            { "srand", (uintptr_t)&sceLibcBridge_srand },
        #else
            { "qsort", (uintptr_t)&qsort },
            { "rand", (uintptr_t)&rand },
            { "srand", (uintptr_t)&srand },
        #endif


        // Env
        { "getenv", (uintptr_t)&getenv_soloader },
        { "setenv", (uintptr_t)&setenv_soloader },


        // Jmp
        { "setjmp", (uintptr_t)&setjmp }, // TODO: May have different struct size?
        { "longjmp", (uintptr_t)&longjmp }, // TODO: May have different struct size?


        // Signals
        { "bsd_signal", (uintptr_t)&signal },
        { "raise", (uintptr_t)&raise },
        { "sigaction", (uintptr_t)&sigaction },


        // Locale
        { "freelocale", (uintptr_t)&freelocale },
        { "localeconv", (uintptr_t)&localeconv },
        { "newlocale", (uintptr_t)&newlocale },
        { "setlocale", (uintptr_t)&setlocale },
        { "uselocale", (uintptr_t)&uselocale },


        // zlib
        { "adler32", (uintptr_t)&adler32 },
        { "compress", (uintptr_t)&compress },
        { "compressBound", (uintptr_t)&compressBound },
        { "crc32", (uintptr_t)&crc32 },
        { "deflate", (uintptr_t)&deflate },
        { "deflateEnd", (uintptr_t)&deflateEnd },
        { "deflateInit2_", (uintptr_t)&deflateInit2_ },
        { "deflateInit_", (uintptr_t)&deflateInit_ },
        { "deflateReset", (uintptr_t)&deflateReset },
        { "gzclose", (uintptr_t)&gzclose },
        { "gzgets", (uintptr_t)&gzgets },
        { "gzopen", (uintptr_t)&gzopen },
        { "inflate", (uintptr_t)&inflate },
        { "inflateEnd", (uintptr_t)&inflateEnd },
        { "inflateInit2_", (uintptr_t)&inflateInit2_ },
        { "inflateInit_", (uintptr_t)&inflateInit_ },
        { "inflateReset", (uintptr_t)&inflateReset },
        { "inflateReset2", (uintptr_t)&inflateReset2 },
        { "uncompress", (uintptr_t)&uncompress },
};

void *dlsym_soloader(void * handle, const char * symbol) {
    for (int i = 0; i < sizeof(default_dynlib) / sizeof(default_dynlib[0]); i++) {
        if (strcmp(symbol, default_dynlib[i].symbol) == 0) {
            return &default_dynlib[i].func;
        }
    }

    l_error("dlsym: Unknown symbol \"%s\".", symbol);
    return NULL;
}

void resolve_imports(so_module* mod) {
    __sF_fake[0] = *stdin;
    __sF_fake[1] = *stdout;
    __sF_fake[2] = *stderr;

    so_resolve(mod, default_dynlib, sizeof(default_dynlib), 0);
}