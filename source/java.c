#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <so_util/so_util.h>

#include "utils/logger.h"
#include "reimpl/movie.h"

extern so_module so_mod;

/*
 * JNI Methods
*/

// I metodi Java che ritornano String devono restituire una JavaString costruita
// da FalsoJNI, NON un char* grezzo: GetStringUTFChars dereferenzia str->utf16 e
// su un puntatore a stringa C va in data abort (terzo crash osservato).
extern jstring NewStringUTF(JNIEnv* env, const char* bytes);

// Cache: InitLocale puo' richiamarli, e ogni NewStringUTF alloca.
static jobject jstr_cached(jstring* slot, const char* text) {
    if (*slot == NULL)
        *slot = NewStringUTF(&jni, text);
    return (jobject)*slot;
}

// "en"/"US" e' il percorso piu' battuto; per l'italiano basta cambiarli
//           in "it"/"IT" (il gioco cerca le stringhe localizzate nei .pak).
static jstring s_language = NULL, s_country = NULL, s_account = NULL;

// Claw::AndroidApplication::InitLocale() passa il risultato di questi due a
// Claw::NarrowString(const char*), che ci fa strlen(): con NULL si schianta.
static jobject Method_GetLanguageCode(jmethodID id, va_list args) { return jstr_cached(&s_language, "en"); }
static jobject Method_GetCountryCode(jmethodID id, va_list args)  { return jstr_cached(&s_country,  "US"); }
static jobject Method_GetUserAccount(jmethodID id, va_list args)  { return jstr_cached(&s_account,  ""); }

// Nessun account di sistema da esporre.
static jint Method_GetUserAccountsCount(jmethodID id, va_list args) { return 0; }

// Il motore NON apre i .pak da solo: chiede a Java
// ClawActivityCommon.FillAndroidFD("data.pak") di aprirli e poi legge il file
// descriptor dal campo "descriptor" di java/io/FileDescriptor. Su Android quel
// metodo pesca l'asset dentro l'APK; qui i .pak sono file veri in DATA_PATH
// "assets/", quindi basta aprirli e pubblicare l'fd.
// Indice dell'entry "descriptor" dentro fieldsInt[] (vedi in fondo al file).
#define FD_FIELD_INDEX 1

static const char * jstring_to_cstr(jstring s) {
    JavaString * js = (JavaString *)s;
    if (!js || !js->utf8 || !js->utf8->array)
        return NULL;
    return (const char *)js->utf8->array;
}

static jboolean Method_FillAndroidFD(jmethodID id, va_list args) {
    const char * name = jstring_to_cstr(va_arg(args, jstring));
    if (!name) {
        fjni_log_err("[JNI] FillAndroidFD: nome file nullo");
        return 0;
    }

    char path[512];
    snprintf(path, sizeof(path), DATA_PATH "assets/%s", name);

    // gli fd precedenti restano aperti di proposito. Il motore apre
    //           pochi .pak una volta sola e puo' tenerli aperti tutti insieme;
    //           chiudere il precedente romperebbe il pak gia' montato.
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        fjni_logv_err("[JNI] FillAndroidFD(\"%s\"): open(%s) fallita", name, path);
        return 0;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        fjni_logv_err("[JNI] FillAndroidFD(\"%s\"): fstat fallita", name);
        close(fd);
        return 0;
    }

    // Il campo che nativeFillAndroidFD andra' a leggere con GetIntField.
    fieldsInt[FD_FIELD_INDEX].value = fd;

    // Su Android il metodo Java, dopo aver aperto l'asset, richiama il nativo per
    // consegnargli fd/offset/lunghezza. Restituire solo true non basta: il motore
    // resterebbe con fd 0 e lunghezza 0, ed e' esattamente la mmap(0x0, 0, ...)
    // vista nel log. Qui i .pak sono file interi, quindi offset 0.
    static void (*nativeFillAndroidFD)(JNIEnv *, jclass, jobject, int, int) = NULL;
    if (!nativeFillAndroidFD) {
        nativeFillAndroidFD = (void *)so_symbol(&so_mod,
                "Java_com_Claw_Android_ClawActivityCommon_nativeFillAndroidFD");
        if (!nativeFillAndroidFD) {
            fjni_log_err("[JNI] nativeFillAndroidFD non trovata nel modulo");
            return 0;
        }
    }

    // L'oggetto FileDescriptor non viene mai dereferenziato da FalsoJNI: conta
    // solo che non sia NULL, il valore arriva dal campo "descriptor".
    nativeFillAndroidFD(&jni, (jclass)0x42424242, (jobject)0x42424243, 0, (int)st.st_size);

    fjni_logv_info("[JNI] FillAndroidFD(\"%s\") -> fd %i, %i byte (%s)",
                   name, fd, (int)st.st_size, path);
    return 1;
}

// Layout dei tasti: su Vita PAL/NTSC la croce conferma, quindi niente scambio.
static jboolean Method_IsOXKeysSwapped(jmethodID id, va_list args) { return 0; }

// ClawAudio: il motore istanzia com/Claw/Android/ClawAudio e ne chiama Start()/Stop().
// L'audio non e' implementato: senza queste voci le chiamate restano irrisolte e il
// motore aspetta un sottosistema che non rispondera' mai.
// NB FalsoJNI risolve per NOME, non per classe: "Start"/"Stop" sono generici e
// intercettano anche quelli degli SDK pubblicitari. Per dei no-op e' quello che vogliamo.
static void Method_NoOp(jmethodID id, va_list args) { (void)id; (void)args; }

// Il film lo riprodurrebbe Java: il motore chiama PlayMovie() e poi sonda
// IsMoviePlaying() finche' non torna false. Qui lo riproduce reimpl/movie.c.
// PlayMovie riceve il nome SENZA estensione ("android_intro") e l'indice.
// Se il file manca, false: il gioco salta il filmato e prosegue.
static jboolean Method_PlayMovie(jmethodID id, va_list args) {
    (void)id;
    const char * name = jstring_to_cstr(va_arg(args, jstring));
    int idx = va_arg(args, int);
    (void)idx;
    return movie_start(name);
}
static jboolean Method_IsMoviePlaying(jmethodID id, va_list args) {
    (void)id; (void)args;
    return movie_is_playing();
}

// SDK pubblicitari, analytics e social: i server sono spenti da anni e la Vita
// non ha ne' banner ne' vibrazione. Senza queste voci ogni chiamata lascia due
// righe di errore sulla memory card anche nella build di rilascio.
// I nomi vengono dal log di una sessione vera, non da un elenco di SDK: si
// stubba quello che il gioco chiede davvero.

NameToMethodID nameToMethodId[] = {
        { 100, "GetLanguageCode",      METHOD_TYPE_OBJECT },
        { 101, "GetCountryCode",       METHOD_TYPE_OBJECT },
        { 102, "GetUserAccount",       METHOD_TYPE_OBJECT },
        { 103, "GetUserAccountsCount", METHOD_TYPE_INT },
        { 104, "IsOXKeysSwapped",      METHOD_TYPE_BOOLEAN },
        { 105, "FillAndroidFD",        METHOD_TYPE_BOOLEAN },
        { 106, "Start",                METHOD_TYPE_VOID },
        { 107, "Stop",                 METHOD_TYPE_VOID },
        { 108, "SetBannerVisibility",    METHOD_TYPE_VOID },
        { 109, "LogGameDesignEvent",     METHOD_TYPE_VOID },
        { 110, "logEvent",               METHOD_TYPE_VOID },
        { 111, "placement",              METHOD_TYPE_VOID },
        { 112, "StopVibra",              METHOD_TYPE_VOID },
        { 113, "SetHorizontalAlignment",  METHOD_TYPE_VOID },
        { 114, "RequestBanner",          METHOD_TYPE_VOID },
        { 115, "reportopen",             METHOD_TYPE_VOID },
        { 116, "initialize",             METHOD_TYPE_VOID },
        { 117, "callToSyncChallenges",   METHOD_TYPE_VOID },
        { 118, "startSession",           METHOD_TYPE_VOID },
        { 119, "HideBanner",             METHOD_TYPE_VOID },
        { 120, "requestAd",              METHOD_TYPE_VOID },
        { 121, "onResume",               METHOD_TYPE_VOID },
        { 122, "load",                   METHOD_TYPE_VOID },
        { 123, "checkPoints",            METHOD_TYPE_VOID },
        { 124, "cancelRequest",          METHOD_TYPE_VOID },
        { 125, "cancel",                 METHOD_TYPE_VOID },
        { 126, "SetOrientation",         METHOD_TYPE_VOID },
        { 127, "OnResume",               METHOD_TYPE_VOID },
        { 128, "Initialize",             METHOD_TYPE_VOID },
        { 129, "Init",                   METHOD_TYPE_VOID },
        { 130, "PlayMovie",              METHOD_TYPE_BOOLEAN },
        { 131, "IsMoviePlaying",          METHOD_TYPE_BOOLEAN },
};

MethodsBoolean methodsBoolean[] = {
        { 104, Method_IsOXKeysSwapped },
        { 105, Method_FillAndroidFD },
        { 130, Method_PlayMovie },
        { 131, Method_IsMoviePlaying },
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};
MethodsInt methodsInt[] = {
        { 103, Method_GetUserAccountsCount },
};
MethodsLong methodsLong[] = {};
MethodsObject methodsObject[] = {
        { 100, Method_GetLanguageCode },
        { 101, Method_GetCountryCode },
        { 102, Method_GetUserAccount },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
        { 106, Method_NoOp },
        { 107, Method_NoOp },
        { 108, Method_NoOp },
        { 109, Method_NoOp },
        { 110, Method_NoOp },
        { 111, Method_NoOp },
        { 112, Method_NoOp },
        { 113, Method_NoOp },
        { 114, Method_NoOp },
        { 115, Method_NoOp },
        { 116, Method_NoOp },
        { 117, Method_NoOp },
        { 118, Method_NoOp },
        { 119, Method_NoOp },
        { 120, Method_NoOp },
        { 121, Method_NoOp },
        { 122, Method_NoOp },
        { 123, Method_NoOp },
        { 124, Method_NoOp },
        { 125, Method_NoOp },
        { 126, Method_NoOp },
        { 127, Method_NoOp },
        { 128, Method_NoOp },
        { 129, Method_NoOp },
};

/*
 * JNI Fields
*/

// System-wide constant that applications sometimes request
// https://developer.android.com/reference/android/content/Context.html#WINDOW_SERVICE
char WINDOW_SERVICE[] = "window";

// System-wide constant that's often used to determine Android version
// https://developer.android.com/reference/android/os/Build.VERSION.html#SDK_INT
// Possible values: https://developer.android.com/reference/android/os/Build.VERSION_CODES
const int SDK_INT = 19; // Android 4.4 / KitKat

NameToFieldID nameToFieldId[] = {
		{ 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT }, 
		{ 1, "SDK_INT", FIELD_TYPE_INT },
		{ 2, "descriptor", FIELD_TYPE_INT },  // java/io/FileDescriptor.descriptor
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
		{ 1, SDK_INT },
		{ 2, -1 },  // FD_FIELD_INDEX: riempito a runtime da FillAndroidFD
};
FieldsObject fieldsObject[] = {
		{ 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
