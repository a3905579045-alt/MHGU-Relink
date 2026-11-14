// Exact ui.c icon bridge snippets with real SDL mutex/atomic primitives on the host.
// JNI strings below are a narrow adapter fixture, not an Android/JNI runtime test.
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>
#include <jni.h>
#include "../runtime/app_actions.h"

typedef SDL_Mutex *pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER NULL
static unsigned lock_entries, unlock_entries;
static int pthread_mutex_lock(pthread_mutex_t *mutex) {
    assert(*mutex);
    SDL_LockMutex(*mutex); ++lock_entries;
    return 0;
}
static int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    ++unlock_entries; SDL_UnlockMutex(*mutex);
    return 0;
}
#include "touch_icon_android_under_test.h"
#define ui_request_touch_icon pc_request_touch_icon
#define ui_take_touch_icon pc_take_touch_icon
#include "touch_icon_pc_under_test.h"
#undef ui_request_touch_icon
#undef ui_take_touch_icon

static bool activity_ok = true;
static unsigned activity_calls;
static bool activity_call_language(const char *method) {
    assert(strcmp(method, "chooseTouchIcon") == 0);
    ++activity_calls;
    return activity_ok;
}
#include "touch_icon_dispatch_under_test.h"

static const char hash_a[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char hash_b[] = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
static void fresh(void) {
    touch_icon_waiting = false; touch_icon_status = 0; touch_icon_hash[0] = 0;
    SDL_SetAtomicInt(&touch_icon_requested, 0);
    activity_ok = true; activity_calls = 0;
}
static void request_and_result(void) {
    char hash[65];
    fresh();
    touch_icon_result(1, hash_a);  // unsolicited callback cannot become a result.
    assert(ui_take_touch_icon(hash) == 0 && !hash[0]);
    assert(ui_request_touch_icon() && !ui_request_touch_icon());
    assert(SDL_GetAtomicInt(&touch_icon_requested) == 1);
    assert(ui_take_touch_icon(hash) == 0 && !hash[0] && !ui_request_touch_icon());
    touch_icon_result(1, hash_a);
    touch_icon_result(1, hash_b);
    touch_icon_result(-2, NULL);
    assert(!ui_request_touch_icon());
    assert(ui_take_touch_icon(hash) == 1 && strcmp(hash, hash_a) == 0);
    assert(ui_take_touch_icon(hash) == 0 && !hash[0]);
    assert(ui_request_touch_icon()); touch_icon_result(-1, NULL);
    assert(ui_take_touch_icon(hash) == -1 && !hash[0]);
    assert(ui_request_touch_icon()); touch_icon_result(-2, hash_a);
    assert(ui_take_touch_icon(NULL) == -2);
    assert(ui_request_touch_icon()); touch_icon_result(1, hash_b);
    assert(ui_take_touch_icon(hash) == 1 && strcmp(hash, hash_b) == 0);
    puts("PASS: one request/result at a time, first callback wins, results consume once, and cancel/failure permit retry.");
}
static void invalid_results(void) {
    char out[65], short_hash[64], long_hash[66], uppercase[65], path[65];
    memset(short_hash, 'a', sizeof short_hash); short_hash[63] = 0;
    memset(long_hash, 'a', sizeof long_hash); long_hash[65] = 0;
    memcpy(uppercase, hash_a, sizeof uppercase); uppercase[10] = 'A';
    memcpy(path, hash_a, sizeof path); path[0] = '/';
    const char *bad[] = {NULL,"",short_hash,long_hash,uppercase,path};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; ++i) {
        fresh(); assert(ui_request_touch_icon());
        touch_icon_result(1, bad[i]);
        assert(ui_take_touch_icon(out) == -2 && !out[0]);
        assert(ui_request_touch_icon()); touch_icon_result(-1, NULL); ui_take_touch_icon(NULL);
    }
    const int statuses[] = {0,2,-3,37};
    for (unsigned i = 0; i < sizeof statuses / sizeof *statuses; ++i) {
        fresh(); assert(ui_request_touch_icon()); touch_icon_result(statuses[i], hash_a);
        assert(ui_take_touch_icon(out) == -2 && !out[0]);
    }
    puts("PASS: only status 1 with exactly 64 lowercase hex characters succeeds; malformed hashes and unknown statuses fail safely.");
}
static unsigned acquired, released;
static bool utf_failure;
static const char *JNICALL fake_get_utf(JNIEnv *env, jstring value, jboolean *copied) {
    (void)env; (void)copied; ++acquired;
    return utf_failure ? NULL : (const char *)value;
}
static void JNICALL fake_release_utf(JNIEnv *env, jstring value, const char *text) {
    (void)env; assert((const char *)value == text); ++released;
}
static void jni_adapter(void) {
    struct JNINativeInterface_ methods = {0};
    methods.GetStringUTFChars = fake_get_utf;
    methods.ReleaseStringUTFChars = fake_release_utf;
    JNIEnv env = &methods;
    char out[65]; acquired = released = 0; utf_failure = false;
    fresh(); assert(ui_request_touch_icon());
    Java_com_mhgu_relink_GameActivity_nativeTouchIconResult(&env, NULL, 1, (jstring)hash_a);
    assert(ui_take_touch_icon(out) == 1 && strcmp(out, hash_a) == 0 && acquired == 1 && released == 1);
    assert(ui_request_touch_icon());
    Java_com_mhgu_relink_GameActivity_nativeTouchIconResult(&env, NULL, -1, NULL);
    assert(ui_take_touch_icon(out) == -1 && acquired == 1 && released == 1);
    assert(ui_request_touch_icon()); utf_failure = true;
    Java_com_mhgu_relink_GameActivity_nativeTouchIconResult(&env, NULL, 1, (jstring)hash_a);
    assert(ui_take_touch_icon(out) == -2 && acquired == 2 && released == 1);
    puts("PASS: the real JNI wrapper copies/release strings once, handles null cancellation, and reports failed string acquisition.");
}
static void dispatch_and_pc(void) {
    char hash[65] = "old";
    assert(!pc_request_touch_icon() && pc_take_touch_icon(hash) == 0 && !hash[0]);
    assert(pc_take_touch_icon(NULL) == 0);
    fresh(); assert(ui_request_touch_icon());
    dispatch_touch_icon_request(); dispatch_touch_icon_request();
    assert(activity_calls == 1 && SDL_GetAtomicInt(&touch_icon_requested) == 0);
    assert(ui_take_touch_icon(hash) == 0 && !ui_request_touch_icon());
    touch_icon_result(-1, NULL); assert(ui_take_touch_icon(NULL) == -1);
    assert(ui_request_touch_icon()); activity_ok = false;
    dispatch_touch_icon_request(); dispatch_touch_icon_request();
    assert(activity_calls == 2 && ui_take_touch_icon(hash) == -2 && !hash[0]);
    puts("PASS: the real host dispatch consumes each picker request once and reports launch failure; unsupported PC APIs return no import.");
}
static SDL_AtomicInt go;
static int SDLCALL request_thread(void *result) {
    while (!SDL_GetAtomicInt(&go)) SDL_Delay(1);
    *(bool *)result = ui_request_touch_icon();
    return 0;
}
static int SDLCALL result_thread(void *hash) {
    while (!SDL_GetAtomicInt(&go)) SDL_Delay(1);
    touch_icon_result(1, (const char *)hash);
    return 0;
}
static void concurrent_requests(void) {
    SDL_Thread *threads[8]; bool accepted[8] = {0}; char hash[65];
    fresh(); SDL_SetAtomicInt(&go, 0);
    for (unsigned i = 0; i < 8; ++i) {
        threads[i] = SDL_CreateThread(request_thread, "icon-request", &accepted[i]);
        assert(threads[i]);
    }
    SDL_SetAtomicInt(&go, 1);
    unsigned successful = 0;
    for (unsigned i = 0; i < 8; ++i) { SDL_WaitThread(threads[i], NULL); successful += accepted[i]; }
    assert(successful == 1 && !ui_request_touch_icon());
    SDL_SetAtomicInt(&go, 0);
    threads[0] = SDL_CreateThread(result_thread, "icon-result-a", (void *)hash_a);
    threads[1] = SDL_CreateThread(result_thread, "icon-result-b", (void *)hash_b);
    assert(threads[0] && threads[1]); SDL_SetAtomicInt(&go, 1);
    SDL_WaitThread(threads[0], NULL); SDL_WaitThread(threads[1], NULL);
    assert(!ui_request_touch_icon() && ui_take_touch_icon(hash) == 1);
    assert(strcmp(hash, hash_a) == 0 || strcmp(hash, hash_b) == 0);
    assert(ui_take_touch_icon(NULL) == 0 && lock_entries == unlock_entries);
    puts("PASS: eight simultaneous requests admit one picker; competing results remain one complete hash under the real mutex.");
}
int main(void) {
    touch_icon_lock = SDL_CreateMutex(); assert(touch_icon_lock);
    request_and_result(); invalid_results(); jni_adapter(); dispatch_and_pc(); concurrent_requests();
    assert(lock_entries == unlock_entries);
    SDL_DestroyMutex(touch_icon_lock);
    puts("PASS_ICON_BRIDGE_GROUPS=5");
    return 0;
}
