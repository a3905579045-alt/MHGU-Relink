// Production input publication and input_view hooks with original native translator fixtures.
// The fixture models the native button object's edge bookkeeping; it does not run player movement.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/hle.h"
#include "../runtime/hle/input.h"

volatile uint64_t g_present_count;  // config.c model-preview dependency; this harness never renders.

static unsigned char image[0x2400000], heap[0x20000];
static void *test_guest(uint32_t address) {
    if (address < sizeof image) return image + address;
    assert(address >= 0x08000000u && address - 0x08000000u < sizeof heap);
    return heap + address - 0x08000000u;
}
#undef G2H
#define G2H(a) test_guest((uint32_t)(a))

#define PTHREAD_MUTEX_INITIALIZER 0
static unsigned lock_depth, locks, ticks;
static void (*after_unlock)(void);
static int pthread_mutex_lock(pthread_mutex_t *mutex) {
    (void)mutex;
    assert(lock_depth == 0);
    ++lock_depth; ++locks;
    return 0;
}
static int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    (void)mutex;
    assert(lock_depth == 1);
    --lock_depth;
    void (*callback)(void) = after_unlock;
    after_unlock = NULL;
    if (callback) callback();
    return 0;
}
static uint64_t clock_ns = 1000000000ull;
uint64_t plat_now_ns(void) { return clock_ns; }
void emsync_tick(void) { ++ticks; }
void hpscale_tick(void) { ++ticks; }
void hitsync_tick(void) { ++ticks; }
void chat_tick(Cpu *c) { (void)c; ++ticks; }
void presence_tick(void) { ++ticks; }

// This generated header is an unchanged substring of the active input.c, checked by the runner.
#include "input_publication_under_test.h"
#include "../runtime/hle/input_view.c"
#include "native_translator_fixture.h"

static struct { char name[48]; HostFn fn; uint32_t entry; } hooks[64];
static unsigned hook_count;
static uint32_t native_role = 0x40;
static Cpu next_input, next_output;
static uint32_t next_function;
static unsigned next_calls;
static bool clobber_next;
static struct { uint32_t fn, player, event, r2; } native_calls[16];
static unsigned native_call_count;
static uint32_t local_result = 1;
static bool checking_kc;
static unsigned expected_kc_next_calls;
static const uint32_t pad_address = 0x08000000u, player_address = 0x08008000u;
static const uint32_t remote_address = 0x0800c000u, options_address = 0x08012000u;

static uint32_t translated(uint32_t word, bool menu) {
    const uint32_t *bits = menu ? native_mb2_bits : native_mb1_bits;
    uint32_t result = 0;
    for (unsigned i = 0; i < 32; ++i) if (word & (1u << i)) result |= bits[i];
    return result;
}
uint32_t hle_register(const char *name, HostFn fn) {
    assert(hook_count < sizeof hooks / sizeof *hooks);
    snprintf(hooks[hook_count].name, sizeof hooks[hook_count].name, "%s", name);
    hooks[hook_count].fn = fn;
    return HLE_BASE + 16 * hook_count++;
}
bool fnhook_set_outer(uint32_t fn, uint32_t detour) {
    assert(detour >= HLE_BASE && (detour - HLE_BASE) % 16 == 0);
    unsigned i = (detour - HLE_BASE) / 16;
    assert(i < hook_count);
    hooks[i].entry = fn;
    return true;
}
void guest_call(Cpu *c, uint32_t fn) {
    if (fn == FN_PAD_INDEX) { c->r[0] = 0; return; }
    assert(fn == FN_PL_IS_LOCAL || fn == FN_PL_EVENT);
    if (checking_kc) assert(next_function == FN_KC_R && next_calls == expected_kc_next_calls);
    assert(native_call_count < sizeof native_calls / sizeof *native_calls);
    native_calls[native_call_count].fn = fn;
    native_calls[native_call_count].player = c->r[0];
    native_calls[native_call_count].event = c->r[1];
    native_calls[native_call_count++].r2 = c->r[2];
    uint32_t result = 0;
    if (fn == FN_PL_IS_LOCAL) result = c->r[0] == player_address ? local_result : 0;
    else {
        assert(c->r[0] == player_address && (c->r[1] == 0 || c->r[1] == 31));
        // Exact 0x485de4 write, independently checked against original ARM32 by the runner.
        uint32_t word = c->r[0] + 0x1414 + 4 * (c->r[1] >> 5);
        ST32(word, LD32(word) | (1u << (c->r[1] & 31)));
    }
    // call() promises GPR preservation only. The original local/setter fixtures do not use VFP.
    for (unsigned i = 0; i < 16; ++i) c->r[i] = 0xaaaa0000u + i;
    c->r[0] = result;
}
void fnhook_call_next(Cpu *c, uint32_t fn) {
    ++next_calls; next_input = *c; next_function = fn;
    uint32_t result = c->r[0];
    if (fn == FN_TR_MB1 || fn == FN_TR_MB2) result = translated(c->r[1], fn == FN_TR_MB2);
    else if (fn == FN_KC_R) result = native_role;
    else if (fn == FN_MB_UPDATE) {
        uint32_t mb = c->r[0];
        Cpu query = *c;
        if (mb == spad() + MB1_OFF) tr_mb1(&query); else tr_mb2(&query);
        uint32_t previous = LD32(mb + 4), current = query.r[0];
        ST32(mb + 4, current); ST32(mb + 8, previous);
        ST32(mb + 12, current & ~previous); ST32(mb + 16, previous & ~current);
    }
    if (clobber_next) {
        uint32_t callee_saved[8];
        memcpy(callee_saved, c->r + 4, sizeof callee_saved);
        memset(c, 0x6b, sizeof *c);
        memcpy(c->r + 4, callee_saved, sizeof callee_saved);
    }
    c->r[0] = result; next_output = *c;
}

static const uint32_t state_address = 0x08018000u, id_address = 0x08018100u;
static Cpu cpu_seed(uint32_t first, uint32_t second, uint32_t lr) {
    Cpu cpu; memset(&cpu, 0x38, sizeof cpu);
    cpu.r[0] = first; cpu.r[1] = second; cpu.r[14] = lr;
    return cpu;
}
static uint64_t delivered_buttons(void) {
    uint64_t buttons;
    memcpy(&buttons, G2H(state_address + 8), sizeof buttons);
    return buttons;
}
static void poll_player(unsigned id) {
    ST32(id_address, id);
    Cpu cpu = cpu_seed(state_address, id_address, 0);
    fill_state(&cpu);
    assert(lock_depth == 0);
}
static void reset_publication(void) {
    assert(lock_depth == 0 && after_unlock == NULL);
    memset(&act_staged, 0, sizeof act_staged);
    memset(&act_pub, 0, sizeof act_pub);
    memset(&act_delivered, 0, sizeof act_delivered);
    g_buttons = 0;
    for (unsigned i = 0; i < 4; ++i) g_sticks[i] = 0;
    sampling = 0; locks = ticks = 0;
}
static ActSnap sample(bool running, uint64_t held, uint64_t pulse) {
    ActSnap snap = {0};
    for (unsigned i = 0; i < ACT_VARS; ++i) snap.hold[i] = held, snap.pulse[i] = pulse;
    snap.ptr_x = 27; snap.ptr_y = 81; snap.ptr_down = snap.ptr_valid = true;
    snap.touch_run = running;
    return snap;
}
static void publication_contract(void) {
    reset_publication();
    ActSnap on = sample(true, 1ull << ACT_ATTACK_X, 1ull << ACT_DODGE), observed;
    input_act_stage(&on);
    input_act_latch(&observed);
    assert(!observed.touch_run && !observed.hold[0]);
    input_set(1ull << BTN_R, 0, 32767, 0, 0);
    input_act_latch(&observed);
    assert(!observed.touch_run && !observed.hold[0]);
    assert(act_pub.touch_run && !act_staged.pulse[0]);
    poll_player(1);
    assert(!delivered_buttons() && act_pub.pulse[0] == on.pulse[0]);
    poll_player(0);
    assert(delivered_buttons() == ((1ull << BTN_R) | (1ull << 17)));
    input_act_latch(&observed);
    assert(observed.touch_run == on.touch_run);
    for (unsigned i = 0; i < ACT_VARS; ++i)
        assert(observed.hold[i] == on.hold[i] && observed.pulse[i] == on.pulse[i]);
    input_act_latch(&observed);
    assert(observed.touch_run && observed.hold[0] == on.hold[0] && !observed.pulse[0]);
    assert(observed.ptr_valid && observed.ptr_down && observed.ptr_x == 27 && observed.ptr_y == 81);
    ActSnap off = sample(false, 0, 0);
    input_act_stage(&off);
    input_set(1ull << BTN_R, 1000, 0, 0, 0);
    input_act_latch(&observed);
    assert(observed.touch_run);  // publication is not delivery, even when it releases run.
    poll_player(0); input_act_latch(&observed);
    assert(!observed.touch_run && delivered_buttons() == (1ull << BTN_R));
    assert(ticks == 15 && locks > 0 && lock_depth == 0);
    puts("PASS: run, native R, sticks, pointer and action pulses use the same publish/poll/latch snapshot; other players do not consume it and run release waits for its matching pad poll.");
}
static void publish_release_after_poll(void) {
    ActSnap off = sample(false, 1ull << ACT_ITEM_USE, 0);
    input_act_stage(&off);
    input_set(1ull << BTN_ZR, -32767, 0, 0, 0);
}
static void publication_interleaving(void) {
    reset_publication();
    ActSnap on = sample(true, 1ull << ACT_ATTACK_X, 0), observed;
    input_act_stage(&on); input_set(1ull << BTN_R, 0, 32767, 0, 0);
    after_unlock = publish_release_after_poll;
    poll_player(0);
    input_act_latch(&observed);
    assert(observed.touch_run && observed.hold[0] == on.hold[0]);
    assert(delivered_buttons() == ((1ull << BTN_R) | (1ull << 17)));
    assert(!act_pub.touch_run && g_buttons == (1ull << BTN_ZR));
    poll_player(0); input_act_latch(&observed);
    assert(!observed.touch_run && observed.hold[0] == (1ull << ACT_ITEM_USE));
    assert(delivered_buttons() == ((1ull << BTN_ZR) | (1ull << 16)));
    puts("PASS: a new host update immediately after poll unlock cannot mix its run flag or buttons into the snapshot already delivered to the game.");
}

static void world(bool enabled, bool split, bool menus, bool dash_zr) {
    reset_publication();
    memset(heap, 0x5a, sizeof heap);
    ST32(G_SPAD, 0x1000); ST32(0x1000, pad_address);
    ST32(G_OPTIONS, options_address); ST8(options_address + OPT_DASH_ROLE, dash_zr);
    ActProfile profile = {0};
    profile.enable = enabled; profile.split = split; profile.menus = menus;
    act_use_profile(&profile);
    memset(&g_snap, 0, sizeof g_snap);
    g_frame = 20; g_pl_frame = g_var_frame = 20; g_town_frame = 0;
    g_var = 0; g_weapon_pub = 0; g_player = player_address; g_drawn = true;
    g_on = enabled; g_split = act_split(); g_menus = menus; g_dash_zr = dash_zr;
    g_bar = g_pointer = g_log = g_kclog = false;
    g_kc_n = 0; local_result = 1; clobber_next = false;
    native_call_count = next_calls = 0;
    ST32(player_address + 0x1414, 0x42004000);
    ST16(player_address + PL_ACT_ST, 1); ST16(player_address + PL_ACT_NO, 0);
    for (unsigned i = 1; i <= 4; ++i) {
        ST32(pad_address + MB1_OFF + 4 * i, 0);
        ST32(pad_address + MB2_OFF + 4 * i, 0);
    }
}
static bool legacy_weapon_site(uint32_t site) {
    static const uint32_t callers[] = {0x47e7b0,0x4a7bb0,0x47d480,0x47d508,0x47d530,
                                      0x47d564,0x47cc58,0x47d2fc,0x47d3f8};
    site &= ~1u;
    if (site >= 0x1360000 && site < 0x13f0000) return true;
    for (unsigned i = 0; i < sizeof callers / sizeof *callers; ++i) if (site == callers[i]) return true;
    return false;
}
static void expect_role(uint32_t lr, uint32_t player, bool run, uint32_t mask) {
    static unsigned char expected_heap[sizeof heap];
    memcpy(expected_heap, heap, sizeof heap);
    uint32_t site = lr & ~1u;
    bool allowed = site == 0x47cd3c || site == 0x48d358;
    bool valid = player >= 0x08000000u && player < 0xf0000000u && !(player & 3);
    bool queried = run && allowed && valid;
    bool requested = queried && player == player_address && local_result == 1;
    if (requested) {
        uint32_t before;
        size_t offset = player_address - 0x08000000u + 0x1414;
        memcpy(&before, expected_heap + offset, sizeof before);
        before |= site == 0x47cd3c ? 0x80000001u : 1u;
        memcpy(expected_heap + offset, &before, sizeof before);
    }
    g_snap.touch_run = run; native_role = mask; native_call_count = 0;
    Cpu cpu = cpu_seed(0xabcdef01, 0x12345678, lr);
    cpu.r[4] = player;
    unsigned previous_calls = next_calls;
    checking_kc = true; expected_kc_next_calls = previous_calls + 1;
    kc_r(&cpu);
    checking_kc = false;
    assert(next_calls == previous_calls + 1 && next_function == FN_KC_R);
    assert(next_input.r[14] == lr && next_input.r[4] == player);
    Cpu expected = next_output;
    expected.r[0] = g_on && g_split && legacy_weapon_site(lr) ? ACT_VBIT_WEAPON_R : mask;
    assert(memcmp(&cpu, &expected, sizeof cpu) == 0);
    unsigned events = requested ? (site == 0x47cd3c ? 2 : 1) : 0;
    assert(native_call_count == (unsigned)queried + events);
    if (queried) {
        assert(native_calls[0].fn == FN_PL_IS_LOCAL && native_calls[0].player == player);
        assert(native_calls[0].event == 0 && native_calls[0].r2 == 0);
    }
    for (unsigned i = 0; i < events; ++i) {
        assert(native_calls[i + 1].fn == FN_PL_EVENT && native_calls[i + 1].player == player_address);
        assert(native_calls[i + 1].event == (i ? 31u : 0u) && native_calls[i + 1].r2 == 0);
    }
    assert(memcmp(heap, expected_heap, sizeof heap) == 0);
}
static void native_run_sites(void) {
    const uint32_t masks[] = {0,0x40,0x800,0x840,0x12345678,UINT32_MAX};
    const uint32_t sites[] = {0x47cd3c,0x47cd3d,0x48d358,0x48d359,
        0,0x47cd38,0x47cd40,0x48d354,0x48d35c,0x48d3f0,0x47cab8,0x47cc24,
        0x47e7b0,0x4a7bb0,0x47d480,0x47d508,0x47d530,0x47d564,0x47cc58,0x47d2fc,0x47d3f8,
        0x135fffc,0x1360000,0x13b7750,0x13efffc,0x13f0000,0x85f93c,0x86eeec,0x12345678};
    for (unsigned enabled = 0; enabled < 2; ++enabled)
        for (unsigned split = 0; split < 2; ++split)
            for (unsigned clobber = 0; clobber < 2; ++clobber) {
                world(enabled, split, true, false); clobber_next = clobber;
                for (unsigned mask = 0; mask < sizeof masks / sizeof *masks; ++mask)
                    for (unsigned run = 0; run < 2; ++run)
                        for (unsigned site = 0; site < sizeof sites / sizeof *sites; ++site)
                            expect_role(sites[site], player_address, run, masks[mask]);
            }
    puts("PASS: only the two exact hunt/town run callers request events 0/31 or 0; native/plugin R masks, existing Split behavior and next-call GPR results remain exact with Actions On/Off.");
}
static void locality_and_release(void) {
    const uint32_t invalid[] = {0,3,0x07fffffc,0xf0000000,0xf1000000,remote_address};
    world(false, false, false, false);
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        expect_role(0x47cd3c, invalid[i], true, 0x40);
        expect_role(0x48d358, invalid[i], true, 0x800);
    }
    for (unsigned local = 0; local < 3; ++local) {
        local_result = local;
        expect_role(0x47cd3c, player_address, true, 0x40);
        expect_role(0x48d358, player_address, true, 0x800);
    }
    local_result = 1;
    for (unsigned i = 0; i < 4; ++i) {
        expect_role(0x47cd3c, player_address, true, 0x40);
        expect_role(0x47cd3c, player_address, false, 0x40);
    }
    puts("PASS: remote, misaligned, absent and nonlocal players receive no run events; only the native local result 1 is accepted, and releasing run adds no later event while manual R remains intact.");
}
static void translator_independence(void) {
    const uint32_t words[] = {0,0x200,0x800,0xa00,0xf0000,0x2000,0x100,0xffff,0xffffffff};
    for (unsigned enabled = 0; enabled < 2; ++enabled)
        for (unsigned split = 0; split < 2; ++split)
            for (unsigned dash_zr = 0; dash_zr < 2; ++dash_zr)
                for (unsigned menus = 0; menus < 2; ++menus) {
                    world(enabled, split, menus, dash_zr);
                    g_snap = sample(false, (1ull << ACT_DASH) | (1ull << ACT_WEAPON_R) |
                        (1ull << ACT_TOWN_RUN) | (1ull << ACT_MENU_TAB_R) | (1ull << ACT_ATTACK_X), 0);
                    for (unsigned context = 0; context < 3; ++context) {
                        g_pl_frame = context == 2 ? 0 : g_frame;
                        g_town_frame = context == 2 ? g_frame : 0;
                        g_drawn = context == 0;
                        for (unsigned w = 0; w < sizeof words / sizeof *words; ++w) {
                            for (unsigned menu = 0; menu < 2; ++menu) {
                                Cpu baseline = cpu_seed(pad_address + (menu ? MB2_OFF : MB1_OFF), words[w], 0);
                                Cpu running = baseline;
                                g_snap.touch_run = false;
                                if (menu) tr_mb2(&baseline); else tr_mb1(&baseline);
                                g_snap.touch_run = true;
                                if (menu) tr_mb2(&running); else tr_mb1(&running);
                                assert(memcmp(&baseline, &running, sizeof running) == 0);
                                if (!enabled || (menu && !menus))
                                    assert(running.r[0] == translated(words[w], menu));
                                assert(native_call_count == 0);
                            }
                        }
                    }
                }
    puts("PASS: automatic running changes no MB1/MB2 translator bit in field, sheathed, town or menu contexts; native R/ZR and existing Actions/Split outputs remain identical.");
}
static void latch_and_edges(void) {
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        world(enabled, false, true, false);
        ActSnap on = sample(true, (1ull << ACT_DASH) | (1ull << ACT_MENU_TAB_R), 0);
        on.ptr_valid = on.ptr_down = false;
        input_act_stage(&on); input_set(1ull << BTN_R, 0, 32767, 0, 0); poll_player(0);
        Cpu mb = cpu_seed(pad_address + MB1_OFF, 0x200, 0);
        mb_update(&mb);
        assert(g_snap.touch_run && g_on == (bool)enabled);
        uint32_t manual = LD32(pad_address + MB1_OFF + 4);
        assert(manual & 0x40);  // R is still the original gameplay R bit.
        expect_role(0x47cd3c, player_address, g_snap.touch_run, 0x40);
        on.touch_run = false; input_act_stage(&on);
        input_set(1ull << BTN_R, 0, 32767, 0, 0); poll_player(0);
        Cpu menu = cpu_seed(pad_address + MB2_OFF, 0x200, 0);
        mb_update(&menu);
        assert(g_snap.touch_run);  // only MB1 latches the new game frame.
        mb = cpu_seed(pad_address + MB1_OFF, 0x200, 0); mb_update(&mb);
        assert(!g_snap.touch_run);
        assert(LD32(pad_address + MB1_OFF + 4) == manual);
        assert(LD32(pad_address + MB1_OFF + 12) == 0 && LD32(pad_address + MB1_OFF + 16) == 0);
        expect_role(0x47cd3c, player_address, g_snap.touch_run, 0x40);
        input_act_stage(&(ActSnap){0}); input_set(0, 0, 0, 0, 0); poll_player(0);
        mb = cpu_seed(pad_address + MB1_OFF, 0, 0); mb_update(&mb);
        assert(!g_snap.touch_run && LD32(pad_address + MB1_OFF + 4) == 0);
        assert(LD32(pad_address + MB1_OFF + 16) == manual);
    }
    puts("PASS: Actions On/Off latch run only with MB1's delivered snapshot; stopping run creates no phantom R edge and releasing the independent R button still produces its native release edge.");
}
static void installation_contract(void) {
    world(false, false, false, false);
    g_hooked = g_core_hooked = false; hook_count = 0;
    input_view_apply();
    assert(hook_count == 2 && g_core_hooked && !g_hooked);
    assert(hooks[0].entry == FN_MB_UPDATE && hooks[0].fn == mb_update);
    assert(hooks[1].entry == FN_KC_R && hooks[1].fn == kc_r);
    input_view_apply(); assert(hook_count == 2);
    ActProfile profile = {0}; profile.enable = true; act_use_profile(&profile);
    input_view_apply();
    unsigned installed = hook_count;
    assert(installed > 2 && g_hooked);
    input_view_apply(); assert(hook_count == installed);
    profile.enable = false; act_use_profile(&profile);
    input_view_apply(); assert(hook_count == installed && g_core_hooked);
    puts("PASS: run snapshot/R-role hooks install exactly once even with Actions disabled; enabling rebinding later adds its existing hooks without duplicating core hooks.");
}

int main(void) {
    publication_contract();
    publication_interleaving();
    native_run_sites();
    locality_and_release();
    translator_independence();
    latch_and_edges();
    installation_contract();
    puts("PASS_GROUPS=7");
    return 0;
}
