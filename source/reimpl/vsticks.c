/**
 * @file  vsticks.c
 * @brief Levette e tasti fisici per Monster Shooter.
 *
 * Il gioco si comanda solo a tocchi: fra i simboli importati non c'e'
 * AMotionEvent_getAxisValue, quindi gli assi del joystick non li legge mai.
 * Le levette muovono due dita finte sugli stick virtuali disegnati a schermo;
 * i tasti invece chiamano direttamente i metodi del GameManager.
 *
 * Quattro strade gia' percorse e scartate, per non ritentarle:
 *
 * 1. Dita finte nella coda di input Android: quando un dito si alza l'array dei
 *    puntatori si compatta e l'altro cambia indice. Il motore segue l'indice,
 *    non l'identificativo, e uno stick rovinava l'altro.
 * 2. Tocchi chiamati da un thread nostro: rientrano nella macchina virtuale Lua
 *    che pilota l'interfaccia e, col gioco gia' dentro, la sfasciano (data abort
 *    dentro Lua, visto nel coredump). Da qui lo hook su PrivateTick.
 * 3. Identificativi alti (60, 61): il motore li usa come indice nelle sue
 *    strutture delle dita. Con uno solo sembrava reggere, col secondo si rompeva
 *    tutto. La UI del gioco usa id piccoli, e ora anche noi.
 * 4. Comandi scritti nei vettori analogici del gioco con
 *    GameManager::TouchUpdate, per poter nascondere i controlli a schermo: i
 *    widget touch li riscrivono a ogni frame, e spegnendoli (TouchEnabled =
 *    false in Lua) muore anche l'input. Nascondere i controlli e mantenerli
 *    comandabili sono incompatibili: i controlli restano visibili.
 */

#include "reimpl/vsticks.h"

#include <math.h>
#include <psp2/ctrl.h>
#include <so_util/so_util.h>
#include <stdbool.h>
#include <stdint.h>

#include "utils/logger.h"

extern so_module so_mod;

// Geometria a schermo (960x544), speculare fra i due stick.
#define VS_CENTER_Y   460.f
#define VS_LEFT_X      80.f
#define VS_RIGHT_X    880.f
#define VS_RADIUS      60.f
#define VS_DEADZONE   0.18f

// Id piccoli: il motore li usa come indice. Restano comunque sotto 256, sopra
// quella soglia verrebbero scartati come eventi del pad a schermo.
#define VS_ID_LEFT   1
#define VS_ID_RIGHT  2

typedef void (*touch_fn)(void *app, int x, int y, int id);
typedef void (*gm_fn)(void *gm);

static touch_fn touch_down = NULL, touch_move = NULL, touch_up = NULL;

// GameManager: l'istanza sta in un puntatore statico del gioco.
static void **gm_instance = NULL;
static gm_fn gm_fire_grenade = NULL;
static gm_fn gm_use_healthkit = NULL;
static gm_fn gm_weapon_boost = NULL;

static bool resolved = false;
static uint32_t buttons_old = 0;

typedef struct {
	float center_x;
	int id;
	bool down;
} Stick;

static Stick left  = { VS_LEFT_X,  VS_ID_LEFT,  false };
static Stick right = { VS_RIGHT_X, VS_ID_RIGHT, false };

static float axis(unsigned char raw) {
	return ((float)raw - 128.f) / 128.f;
}

// Un dito per stick: scende al centro, si sposta secondo la levetta, si alza a
// comando. Scendere gia' spostato non va bene: gli stick del gioco ancorano
// l'origine dove il dito tocca.
static void stick_update(Stick *s, void *app, float x_in, float y_in, bool want_down) {
	float magnitude = sqrtf(x_in * x_in + y_in * y_in);
	if (magnitude > 1.f) { x_in /= magnitude; y_in /= magnitude; }

	int x = (int)(s->center_x + x_in * VS_RADIUS);
	int y = (int)(VS_CENTER_Y + y_in * VS_RADIUS);

	if (want_down && !s->down) {
		touch_down(app, (int)s->center_x, (int)VS_CENTER_Y, s->id);
		s->down = true;
	} else if (want_down) {
		touch_move(app, x, y, s->id);
	} else if (s->down) {
		touch_up(app, x, y, s->id);
		s->down = false;
	}
}

void vsticks_tick(void *app) {
	if (!app) return;

	if (!resolved) {
		touch_down = (touch_fn)so_symbol(&so_mod, "_ZN4Claw11AbstractApp16PrivateTouchDownEiii");
		touch_move = (touch_fn)so_symbol(&so_mod, "_ZN4Claw11AbstractApp16PrivateTouchMoveEiii");
		touch_up   = (touch_fn)so_symbol(&so_mod, "_ZN4Claw11AbstractApp14PrivateTouchUpEiii");
		gm_instance      = (void **)so_symbol(&so_mod, "_ZN11GameManager10s_instanceE");
		gm_fire_grenade  = (gm_fn)so_symbol(&so_mod, "_ZN11GameManager11FireGrenadeEv");
		gm_use_healthkit = (gm_fn)so_symbol(&so_mod, "_ZN11GameManager12UseHealthKitEv");
		gm_weapon_boost  = (gm_fn)so_symbol(&so_mod, "_ZN11GameManager11WeaponBoostEv");
		resolved = true;
		if (!touch_down || !touch_move || !touch_up)
			l_error("[VS] simboli dei tocchi non risolti, levette disattivate");
		else
			l_success("[VS] levette attive: sinistra muove, destra mira e spara");
	}
	if (!touch_down || !touch_move || !touch_up) return;

	SceCtrlData pad;
	sceCtrlPeekBufferPositiveExt2(0, &pad, 1);

	float lx = axis(pad.lx), ly = axis(pad.ly);
	float rx = axis(pad.rx), ry = axis(pad.ry);

	bool left_active = sqrtf(lx * lx + ly * ly) > VS_DEADZONE;
	// Si spara inclinando la levetta destra, come nei twin-stick. R resta come
	// alternativa: le funzioni Ext2 riportano le dorsali come R1, non come
	// RTRIGGER, quindi la maschera copre entrambe le convenzioni.
	bool right_active = sqrtf(rx * rx + ry * ry) > VS_DEADZONE
			|| (pad.buttons & (SCE_CTRL_R1 | SCE_CTRL_RTRIGGER)) != 0;

	stick_update(&left, app, lx, ly, left_active);
	stick_update(&right, app, rx, ry, right_active);

	// Tasti: chiamiamo i metodi del GameManager invece di simulare tocchi sulle
	// icone dell'interfaccia.
	void *gm = gm_instance ? *gm_instance : NULL;
	uint32_t pressed = pad.buttons & ~buttons_old;
	buttons_old = pad.buttons;
	if (!gm) return;

	if ((pressed & SCE_CTRL_SQUARE) && gm_fire_grenade) gm_fire_grenade(gm);
	if ((pressed & SCE_CTRL_TRIANGLE) && gm_use_healthkit) gm_use_healthkit(gm);
	if ((pressed & (SCE_CTRL_L1 | SCE_CTRL_LTRIGGER)) && gm_weapon_boost) gm_weapon_boost(gm);
}

void vsticks_init(void) {
	sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
}
