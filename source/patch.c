/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 */

#include <kubridge.h>
#include <so_util/so_util.h>
#include <stdio.h>
#include <string.h>
#include <vitasdk.h>

#ifdef __cplusplus
extern "C"
{
#endif
	extern so_module so_mod;
#ifdef __cplusplus
};
#endif

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)

#include "utils/logger.h"
#include "utils/dialog.h"
#include "reimpl/sys.h"
#include "utils/utils.h"
#include "reimpl/audio.h"
#include "reimpl/vsticks.h"
#include <stdbool.h>

void __kuser_memory_barrier(void) {
	__sync_synchronize();
}

void kuser_patch(void) {
	SceKernelAllocMemBlockKernelOpt opt;
	memset(&opt, 0, sizeof(SceKernelAllocMemBlockKernelOpt));
	opt.size = sizeof(SceKernelAllocMemBlockKernelOpt);
	opt.attr = 0x1;
	opt.field_C = (SceUInt32)0x9A000000;
	if (kuKernelAllocMemBlock("atomic", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, 0x1000, &opt) < 0)
		fatal_error("Error could not allocate atomic block.");
	kuKernelMemProtect((void *)0x9A000000, (SceSize)0x1000, KU_KERNEL_PROT_EXEC | KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE);

	hook_addr(0x9A000FA0, (uintptr_t)__kuser_memory_barrier);
	hook_addr(0x9A000FC0, (uintptr_t)__atomic_cmpxchg);

	uint32_t patched_addr;
	for (uint32_t addr = so_mod.text_base; addr < so_mod.text_base + so_mod.text_size; addr += 4) {
		uint32_t *a = (uint32_t *)addr;
		if (*a == 0xFFFF0FC0) {
			l_debug("Patching 0x%x -> __kuser_cmpxchg", a);
			patched_addr = 0x9A000FC0;
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &patched_addr, sizeof(uint32_t));
		}
		else if (*a == 0xFFFF0FA0) {
			l_debug("Patching 0x%x -> __kuser_memory_barrier", a);
			patched_addr = 0x9A000FA0;
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &patched_addr, sizeof(uint32_t));
		}
	}
}

static so_hook h_audio_open, h_audio_start, h_audio_stop;

// Claw::AndroidAudioDevice e' una fabbrica: Open costruisce il device con il
// formato scelto dal motore, Start accende la riproduzione (e mette s_playing a 1,
// che nativeFillAudioBuffer controlla). Lasciamo girare gli originali - le loro
// chiamate JNI da noi sono no-op - e agganciamo la nostra uscita sceAudioOut.
// Tick del motore: e' il thread su cui gira il gioco, e quindi l'unico posto
// sicuro da cui iniettare i tocchi delle levette. Da un thread nostro i tocchi
// rientrano nella macchina virtuale Lua dell'interfaccia mentre il gioco ci sta
// gia' dentro, e la sfasciano.
static so_hook h_tick;

static void hooked_tick(void *thiz, float dt) {
	// SO_CONTINUE chiama attraverso un puntatore senza prototipo: un float
	// verrebbe promosso a double e finirebbe nei registri sbagliati, e il motore
	// riceverebbe un delta tempo spazzatura (il caricamento, che avanza con dt,
	// restava bloccato). Con ABI softfp i bit del float passati come intero
	// finiscono nello stesso registro.
	uint32_t dt_bits;
	memcpy(&dt_bits, &dt, sizeof(dt_bits));
	(void)SO_CONTINUE(int, h_tick, thiz, dt_bits);
	vsticks_tick(thiz);
}

static void *hooked_audio_open(const void *fmt, const void *params) {
	// Prima di SO_CONTINUE: il costruttore del device chiama gia' Start, e senza
	// il formato la nostra uscita partirebbe con i valori di default.
	audio_set_format(fmt);
	return SO_CONTINUE(void *, h_audio_open, fmt, params);
}

static void hooked_audio_start(void *thiz) {
	(void)SO_CONTINUE(int, h_audio_start, thiz);
	audio_start();
}

static void hooked_audio_stop(void *thiz) {
	(void)SO_CONTINUE(int, h_audio_stop, thiz);
	audio_stop();
}

void so_patch(void) {
	kuser_patch();

	// NB: hook_arm scrive 8 byte. Prima di agganciare un simbolo controllarne
	// st_size (readelf -Ws): sotto 8 byte l'hook sfora nella funzione successiva.
	// GameplayJob::PreloadEntry (4 byte) ha gia' corrotto cosi' GameplayJob::Render.

	// Tapjoy non esiste su Vita. Se ShowFeaturedApp (32 byte, unico chiamante
	// Loading::UpdateFeatureApp) restituisce !=0 il popup resta ad aspettare una
	// callback dell'SDK che non arriva mai: la schermata di caricamento gira sui
	// tre pallini per sempre e ignora il tap. Con 0 il popup va a "fallito".
	hook_addr(so_symbol(&so_mod, "_ZNK13TapjoyManager15ShowFeaturedAppEv"), (uintptr_t)&ret0);

	// Audio: il mixer del gioco riempie il PCM, noi lo portiamo a sceAudioOut.
	uintptr_t a;

	a = so_symbol(&so_mod, "_ZN4Claw11AbstractApp11PrivateTickEf");
	if (a) h_tick = hook_addr(a, (uintptr_t)&hooked_tick);
	else l_error("[VS] simbolo AbstractApp::PrivateTick non risolto");
	a = so_symbol(&so_mod, "_ZN4Claw18AndroidAudioDevice4OpenERKNS_11AudioFormatERKNS_11MixerParamsE");
	if (a) h_audio_open = hook_addr(a, (uintptr_t)&hooked_audio_open);
	else l_error("[AUDIO] simbolo AndroidAudioDevice::Open non risolto");

	a = so_symbol(&so_mod, "_ZN4Claw18AndroidAudioDevice5StartEv");
	if (a) h_audio_start = hook_addr(a, (uintptr_t)&hooked_audio_start);
	else l_error("[AUDIO] simbolo AndroidAudioDevice::Start non risolto");

	a = so_symbol(&so_mod, "_ZN4Claw18AndroidAudioDevice4StopEv");
	if (a) h_audio_stop = hook_addr(a, (uintptr_t)&hooked_audio_stop);
	else l_error("[AUDIO] simbolo AndroidAudioDevice::Stop non risolto");
	// Sample hook with symbol name
	// hook_addr((uintptr_t)so_symbol(&so_mod, "_ZN6glitch2os7Printer5printEPKcz"), (uintptr_t)&hookedFunction);
	// Or with offset
	// hook_addr((uintptr_t)so_mod.text_base + 0xdeadbabe, (uintptr_t)&hookedFunction);
	// If you use SO_CONTINUE, define a so_hook before the function and assign to it
	// function_hook = hook_addr(...);
}