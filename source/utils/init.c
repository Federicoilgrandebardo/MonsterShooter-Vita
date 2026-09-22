/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021-2022 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/settings.h"
#include "utils/filelog.h"

#include <reimpl/controls.h>

#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <fios/fios.h>

// Base address for the Android .so to be loaded at
#define LOAD_ADDRESS 0x98000000

extern so_module so_mod;

// Il gioco dichiara DT_NEEDED libstlport_shared.so e ne importa 16 simboli
// (std::locale, std::ios_base, __node_alloc). Senza questo modulo caricato,
// so_resolve_link non trova nulla e le JUMP_SLOT restano al PLT0 dell'APK:
// primo crash osservato, prefetch abort a 0x6a010 dentro AdSystem::AdSystem.
// so_util risolve dalle dipendenze cercando per SONAME, quindi basta che
// stlport sia nella lista dei moduli prima che si risolva il modulo principale.
static so_module stlport_mod;
#define STLPORT_PATH DATA_PATH "libstlport_shared.so"
// Con kubridge so_file_load rispetta alla lettera l'indirizzo richiesto: la logica
// che ne sceglie uno libero (vm_avail_addr) e' compilata solo senza kubridge.
// Due moduli sullo stesso LOAD_ADDRESS collidono e il secondo non si carica.
// Il modulo principale occupa 0x98000000 + ~1.8 MB; stlport (413 KB) sta 16 MB sopra.
#define STLPORT_LOAD_ADDRESS 0x99000000

void soloader_init_all() {
    filelog_reset();
	// Launch `app0:configurator.bin` on `-config` init param
    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    SceAppUtilAppEventParam eventParam;
    sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
    sceAppUtilReceiveAppEvent(&eventParam);
    if (eventParam.type == 0x05) {
        char buffer[2048];
        sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
        if (strstr(buffer, "-config"))
            sceAppMgrLoadExec("app0:/configurator.bin", NULL, NULL);
    }

    // Set default overclock values
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

#ifdef USE_SCELIBC_IO
    if (fios_init(DATA_PATH) == 0)
        l_success("FIOS initialized.");
#endif

    if (!module_loaded("kubridge")) {
        l_fatal("kubridge is not loaded.");
        fatal_error("Error: kubridge.skprx is not installed.");
    }
    l_success("kubridge check passed.");

    if (!file_exists(SO_PATH)) {
        fatal_error("Looks like you haven't installed the data files for this "
                    "port, or they are in an incorrect location. Please make "
                    "sure that you have %s file exactly at that path.", SO_PATH);
    }

    if (!file_exists(STLPORT_PATH)) {
        fatal_error("Missing %s. Copy it next to the game .so.", STLPORT_PATH);
    }

    if (so_file_load(&stlport_mod, STLPORT_PATH, STLPORT_LOAD_ADDRESS) < 0) {
        l_fatal("STLport could not be loaded.");
        fatal_error("Error: could not load %s.", STLPORT_PATH);
    }
    so_relocate(&stlport_mod);
    resolve_imports(&stlport_mod);
    so_flush_caches(&stlport_mod);
    so_initialize(&stlport_mod);
    l_success("STLport loaded.");

    if (so_file_load(&so_mod, SO_PATH, LOAD_ADDRESS) < 0) {
        l_fatal("SO could not be loaded.");
        fatal_error("Error: could not load %s.", SO_PATH);
    }

    settings_load();
    l_success("Settings loaded.");

    so_relocate(&so_mod);
    l_success("SO relocated.");

    resolve_imports(&so_mod);
    l_success("SO imports resolved.");

    so_patch();
    l_success("SO patched.");

    so_flush_caches(&so_mod);
    l_success("SO caches flushed.");

    so_initialize(&so_mod);
    l_success("SO initialized.");

    gl_preload();
    l_success("OpenGL preloaded.");

    jni_init();
    l_success("FalsoJNI initialized.");

#ifndef NDK_PORT
    controls_init();
    l_success("Controls initialized.");
#endif
}
