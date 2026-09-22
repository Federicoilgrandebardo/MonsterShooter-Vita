/**
 * @file  audio.c
 * @brief Uscita audio su Vita per il mixer del Claw Engine.
 *
 * Il gioco non spedisce file audio: il PCM lo genera il suo mixer. Su Android il
 * lato Java (com/Claw/Android/ClawAudio) teneva un thread che chiamava
 * nativeFillAudioBuffer e passava il risultato ad AudioTrack. Qui facciamo lo
 * stesso con sceAudioOut: stesso riempitore, consumatore diverso.
 */

#include "reimpl/audio.h"

#include <falso_jni/FalsoJNI.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>
#include <stdbool.h>

#include "utils/logger.h"

extern so_module so_mod;

// Frame per riempimento. sceAudioOutOpenPort vuole un multiplo di 64.
#define AUDIO_GRAIN 1024

// Slot in coda. Il mixer ogni tanto si pianta per centinaia di millisecondi
// (sintetizza i moduli tracker e legge da disco): con soli due buffer l'uscita
// resta a secco. Quattro slot da 1024 frame a 22050 Hz = ~370 ms di scorta.
#define AUDIO_SLOTS 4

static void (*native_fill_audio_buffer)(JNIEnv *env, void *clazz, void *array, int size_bytes) = NULL;

static int port = -1;
static SceUID thread_id = -1;
static volatile bool running = false;

// Valori scelti dal motore in MonstazAIApplication::OnStartup: stereo 44100 oppure mono 11025.
static int rate = 44100;
static int channels = 2;

// Doppio buffer: sceAudioOutOutput torna quando ha finito il buffer PRECEDENTE,
// quindi quello appena passato e' ancora in riproduzione. Riempirlo di nuovo
// subito lo fa gracchiare: si riempie l'altro.
static void *pcm_array[AUDIO_SLOTS];  // handle FalsoJNI: e' quello che vuole il riempitore
static short *pcm[AUDIO_SLOTS];       // stessa memoria, vista da noi e da sceAudioOut

static SceUID sem_free = -1;   // slot liberi, il produttore aspetta qui
static SceUID sem_ready = -1;  // slot pieni, il consumatore aspetta qui
static SceUID thread_out = -1;

void audio_set_format(const void *claw_audio_format) {
	const int *f = (const int *)claw_audio_format;  // { canali, frequenza }
	if (!f) return;
	channels = (f[0] == 2) ? 2 : 1;
	rate = f[1];
	l_debug("[AUDIO] formato dal motore: %i canali, %i Hz", channels, rate);
}

// Produttore: chiede PCM al mixer del gioco e riempie gli slot liberi.
static int audio_thread(SceSize args, void *argp) {
	(void)args; (void)argp;
	const int bytes = AUDIO_GRAIN * channels * (int)sizeof(short);
	int i = 0;

	while (running) {
		if (sceKernelWaitSema(sem_free, 1, NULL) < 0) break;
		if (!running) break;

		// Il mixer scrive dentro pcm[i]; a gioco in pausa lascia il buffer com'e'.
		native_fill_audio_buffer(&jni, NULL, pcm_array[i], bytes);

		sceKernelSignalSema(sem_ready, 1);
		i = (i + 1) % AUDIO_SLOTS;
	}
	return sceKernelExitDeleteThread(0);
}

// Consumatore: versa gli slot pieni in sceAudioOut. Separato dal produttore,
// cosi' una pausa del mixer si mangia la scorta invece di bucare l'uscita.
static int audio_out_thread(SceSize args, void *argp) {
	(void)args; (void)argp;
	int i = 0;

	while (running) {
		if (sceKernelWaitSema(sem_ready, 1, NULL) < 0) break;
		if (!running) break;

		sceAudioOutOutput(port, pcm[i]);
		sceKernelSignalSema(sem_free, 1);
		i = (i + 1) % AUDIO_SLOTS;
	}
	return sceKernelExitDeleteThread(0);
}

void audio_start(void) {
	if (running) return;

	if (!native_fill_audio_buffer) {
		native_fill_audio_buffer = (void *)so_symbol(&so_mod,
				"Java_com_Claw_Android_ClawAudio_nativeFillAudioBuffer");
		if (!native_fill_audio_buffer) {
			l_error("[AUDIO] nativeFillAudioBuffer non risolta");
			return;
		}
	}

	if (!pcm_array[0]) {
		JNIEnv *env = &jni;
		for (int i = 0; i < AUDIO_SLOTS; i++) {
			pcm_array[i] = (*env)->NewShortArray(env, AUDIO_GRAIN * channels);
			if (!pcm_array[i]) {
				l_error("[AUDIO] NewShortArray fallita");
				return;
			}
			// ReleaseShortArrayElements in FalsoJNI e' un no-op: il puntatore resta valido.
			pcm[i] = (*env)->GetShortArrayElements(env, pcm_array[i], NULL);
			if (!pcm[i]) {
				l_error("[AUDIO] buffer PCM nullo");
				return;
			}
		}
	}

	port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN, rate,
			channels == 2 ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO);
	if (port < 0) {
		l_error("[AUDIO] sceAudioOutOpenPort ha risposto 0x%08X", port);
		port = -1;
		return;
	}

	sem_free = sceKernelCreateSema("claw_audio_free", 0, AUDIO_SLOTS, AUDIO_SLOTS, NULL);
	sem_ready = sceKernelCreateSema("claw_audio_ready", 0, 0, AUDIO_SLOTS, NULL);
	if (sem_free < 0 || sem_ready < 0) {
		l_error("[AUDIO] creazione semafori fallita");
		sceAudioOutReleasePort(port);
		port = -1;
		return;
	}

	running = true;
	// Priorita' assoluta 0x60, sopra il default dei thread di gioco: a pari priorita'
	// il thread audio viene scavalcato e il riempimento arriva tardi. La forma
	// relativa (0x10000080) il kernel la rifiuta con 0x80028023, priorita' illegale.
	thread_id = sceKernelCreateThread("claw_audio", audio_thread, 0x60, 0x10000, 0, 0, NULL);
	if (thread_id < 0) {
		l_error("[AUDIO] creazione thread fallita: 0x%08X", thread_id);
		running = false;
		sceAudioOutReleasePort(port);
		port = -1;
		return;
	}
	sceKernelStartThread(thread_id, 0, NULL);

	thread_out = sceKernelCreateThread("claw_audio_out", audio_out_thread, 0x60, 0x10000, 0, 0, NULL);
	if (thread_out < 0) {
		l_error("[AUDIO] creazione thread di uscita fallita: 0x%08X", thread_out);
		running = false;
		return;
	}
	sceKernelStartThread(thread_out, 0, NULL);
	l_success("[AUDIO] uscita attiva: %i Hz, %s", rate, channels == 2 ? "stereo" : "mono");
}

void audio_stop(void) {
	if (!running) return;
	running = false;
	// Sblocca i due thread fermi sui semafori, altrimenti l'attesa non finisce.
	sceKernelSignalSema(sem_free, AUDIO_SLOTS);
	sceKernelSignalSema(sem_ready, AUDIO_SLOTS);
	sceKernelWaitThreadEnd(thread_id, NULL, NULL);
	sceKernelWaitThreadEnd(thread_out, NULL, NULL);
	thread_id = -1;
	thread_out = -1;
	sceKernelDeleteSema(sem_free);
	sceKernelDeleteSema(sem_ready);
	sem_free = sem_ready = -1;
	if (port >= 0) {
		sceAudioOutReleasePort(port);
		port = -1;
	}
}
