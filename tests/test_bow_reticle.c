// Actual production visibility/resource-style hooks + hunter resolution, with synthetic guest memory.
// This tests direction/landing visibility and resource eligibility, not native motion, collision or rendering.
// Native predicate fixtures: main build fb08f1d20fd1204fe7070672777f869b.
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/hle.h"

static unsigned char image[0x2400000], heap[0x20000];
static void *test_guest(uint32_t address) {
    if (address < sizeof image) return image + address;
    assert(address >= 0x08000000u && address - 0x08000000u < sizeof heap);
    return heap + address - 0x08000000u;
}
#undef G2H
#define G2H(a) test_guest((uint32_t)(a))
#include "../runtime/hle/weapon.c"
#include "../runtime/hle/bow_reticle.c"

static const uint32_t hunter = 0x08004000u, remote = 0x0800c000u;
static const uint32_t guide_callers[] = {BOW_GUIDE_AIM_LR, BOW_LANDING_AIM_LR};
static HostFn registered_hook, installed_hook, registered_style_hook, installed_style_hook;
static unsigned registrations, installs, next_calls, charge_calls;
static unsigned style_next_calls, param_calls, fail_install_mask;
static bool override_gate, clobber_next, next_finished, style_next_finished;
static uint32_t gate_result, style_result, arc_parameter;
static Cpu next_input, next_output, query_input, style_next_input, style_next_output, *active_cpu;

bool mousecam_hunter_aiming(uint32_t h) { (void)h; return false; }

// 0x13b93dc is a pure switch over these 19 native actions, not a held-input timer.
static bool native_charge_fixture(uint32_t h) {
    unsigned st = LD16(h + PL_ACT_ST), no = LD16(h + PL_ACT_NO);
    if (st == 1) return no == 0x80 || no == 0x81 || no == 0x82;
    if (st == 4) return no == 0xa;
    if (st != 0x10) return false;
    switch (no) {
        case 0x2: case 0x3: case 0x4: case 0x5: case 0x6: case 0x7: case 0xd:
        case 0x13: case 0x1b: case 0x2d: case 0x35: case 0x3b: case 0x3c: case 0x43: case 0x44:
            return true;
        default: return false;
    }
}

// 0x13b7750 reads drawn state and the game's Type 1 held-aim / Type 2 toggle state.
// Native output is boolean; arbitrary nonzero plugin results are tested separately.
static bool native_guide_aim_fixture(uint32_t h) {
    if (!h || !(LD8(h + PL_FLAGS) & 1)) return false;
    unsigned st = LD16(h + PL_ACT_ST);
    if (st == 0x40 || st == 0x80 || st == 0x100 || st == 0x800) return false;
    if (LD8(LD32(G_GAMECONTROL) + 0x6c) == 1) return LD8(h + 0x3334) != 0;
    return (LD32(h + 0x1414) & (1u << 0x1c)) != 0;
}

void guest_call(Cpu *c, uint32_t fn) {
    if (fn == BOW_GUIDE_PARAM) {
        assert(c != active_cpu && style_next_finished && c->r[0] == style_next_input.r[0] && c->r[1] == 0x1b);
        Cpu expected = style_next_output;
        expected.r[0] = style_next_input.r[0]; expected.r[1] = 0x1b;
        assert(memcmp(c, &expected, sizeof expected) == 0);
        ++param_calls;
        memset(c, 0xa5, sizeof *c);
        c->r[0] = arc_parameter;
        return;
    }
    assert(fn == BOW_GUIDE_CHARGING && c != active_cpu && c->r[0] == hunter);
    assert(next_finished);  // the existing native/plugin chain must finish before the query
    Cpu expected = next_output;
    expected.r[0] = hunter;
    query_input = *c;
    assert(memcmp(&query_input, &expected, sizeof expected) == 0);
    ++charge_calls;
    bool charging = native_charge_fixture(c->r[0]);
    // Exercise every CPU/VFP byte a guest call could clobber, beyond its result.
    memset(c, 0xa5, sizeof *c);
    c->r[0] = charging;
}

void fnhook_call_next(Cpu *c, uint32_t fn) {
    if (fn == BOW_GUIDE_STYLE) {
        assert(c == active_cpu && !style_next_finished);
        style_next_input = *c; ++style_next_calls;
        if (clobber_next) memset(c, 0x6b, sizeof *c);
        c->r[0] = style_result;
        style_next_output = *c; style_next_finished = true;
        return;
    }
    assert(fn == BOW_GUIDE_AIM && c == active_cpu && !next_finished);
    next_input = *c;
    ++next_calls;
    uint32_t result = override_gate ? gate_result : native_guide_aim_fixture(c->r[0]);
    if (clobber_next) memset(c, 0x6b, sizeof *c);
    c->r[0] = result;
    next_output = *c;
    next_finished = true;
}

uint32_t hle_register(const char *name, HostFn fn) {
    assert(name && registrations < 2);
    if (strcmp(name, "bow_guide:visible") == 0) {
        assert(!registered_hook); registered_hook = fn;
        ++registrations; return HLE_BASE;
    }
    assert(strcmp(name, "bow_guide:landing_style") == 0 && !registered_style_hook);
    registered_style_hook = fn;
    ++registrations; return HLE_BASE + 4;
}
bool fnhook_set_outer(uint32_t fn, uint32_t detour) {
    // Only the two world-guide hooks, never either former cUIOQuestScope hook.
    assert(fn == BOW_GUIDE_AIM || fn == BOW_GUIDE_STYLE);
    assert(fn != 0x81e438u && fn != 0x4a1c48u);
    assert(installs < 2);
    ++installs;
    if (fn == BOW_GUIDE_AIM) {
        assert(detour == HLE_BASE && registered_hook);
        if (fail_install_mask & 1) return false;
        installed_hook = registered_hook;
    } else {
        assert(detour == HLE_BASE + 4 && registered_style_hook);
        if (fail_install_mask & 2) return false;
        installed_style_hook = registered_style_hook;
    }
    return true;
}

static void action(unsigned state, unsigned number) {
    ST16(hunter + PL_ACT_ST, state); ST16(hunter + PL_ACT_NO, number);
}
static void manual_aim(unsigned type, bool enabled) {
    ST8(LD32(G_GAMECONTROL) + 0x6c, type);
    // Give the unused control the opposite value to verify the option-specific native path.
    ST32(hunter + 0x1414, ((type == 0 ? enabled : !enabled) ? 1u : 0u) << 0x1c);
    ST8(hunter + 0x3334, type == 1 ? enabled : !enabled);
}
static void world(void) {
    memset(heap, 0, sizeof heap);
    uint32_t gc = 0x08000000, sp = 0x08001000;
    ST32(G_GAMECONTROL, gc); ST32(G_SPLAYER, sp);
    ST32(gc + 0x30, 0); ST32(sp + 0x1c, hunter);
    ST32(hunter, VT_BOW); ST32(remote, VT_BOW);
    ST32(VT_HEAVY + 4 * VT_DRAWN, FN_DRAWN);
    ST32(VT_LIGHT + 4 * VT_DRAWN, FN_DRAWN); ST32(VT_BOW + 4 * VT_DRAWN, FN_DRAWN);
    ST8(hunter + PL_FLAGS, 1); ST32(hunter + 0x13cc, 1);
    ST32(hunter + PL_DATA, 0x08018000); ST32(remote + PL_DATA, 0x08019000);
    action(1, 0); manual_aim(0, false);
    override_gate = clobber_next = next_finished = style_next_finished = false;
    gate_result = 0;
    style_result = 0; arc_parameter = 0;
    next_calls = charge_calls = style_next_calls = param_calls = 0;
}
static void install_failures(unsigned mask) {
    registrations = installs = 0; fail_install_mask = mask;
    registered_hook = installed_hook = registered_style_hook = installed_style_hook = NULL;
    bow_reticle_init();
    assert(installs == 2 && registrations == 2 && registered_hook && registered_style_hook);
    assert((installed_hook != NULL) == !(mask & 1));
    assert((installed_style_hook != NULL) == !(mask & 2));
}
static void install(bool failure) { install_failures(failure ? 3 : 0); }
static void invoke(Cpu *c) {
    Cpu input = *c;
    unsigned calls = next_calls;
    active_cpu = c; next_finished = false;
    if (installed_hook) installed_hook(c);
    else fnhook_call_next(c, BOW_GUIDE_AIM);
    active_cpu = NULL;
    assert(next_finished && next_calls == calls + 1);
    assert(memcmp(&next_input, &input, sizeof input) == 0);
}
static void invoke_style(Cpu *c) {
    Cpu input = *c;
    unsigned calls = style_next_calls;
    active_cpu = c; style_next_finished = false;
    if (installed_style_hook) installed_style_hook(c); else fnhook_call_next(c, BOW_GUIDE_STYLE);
    active_cpu = NULL;
    assert(style_next_finished && style_next_calls == calls + 1);
    assert(memcmp(&style_next_input, &input, sizeof input) == 0);
}
static Cpu seeded(uint32_t h, uint32_t lr) {
    Cpu cpu; memset(&cpu, 0x38, sizeof cpu);
    cpu.r[0] = h; cpu.r[1] = remote; cpu.r[14] = lr;
    return cpu;
}
static void assert_only_result(const Cpu *c, uint32_t result) {
    Cpu expected = next_output; expected.r[0] = result;
    assert(memcmp(c, &expected, sizeof expected) == 0);
}
static void expect_gate(uint32_t h, uint32_t lr, uint32_t result, unsigned queries) {
    unsigned calls = charge_calls;
    Cpu cpu = seeded(h, lr);
    invoke(&cpu);
    assert_only_result(&cpu, result);
    assert(charge_calls == calls + queries);
}
static void expect_guides(uint32_t h, uint32_t result, unsigned queries) {
    for (unsigned i = 0; i < sizeof guide_callers / sizeof *guide_callers; ++i)
        expect_gate(h, guide_callers[i], result, queries);
}
static uint32_t expect_style(uint32_t h, uint32_t lr, uint32_t result, unsigned queries) {
    unsigned calls = param_calls;
    Cpu cpu = seeded(h, lr); invoke_style(&cpu);
    Cpu expected = style_next_output; expected.r[0] = result;
    assert(memcmp(&cpu, &expected, sizeof expected) == 0 && param_calls == calls + queries);
    return cpu.r[0];
}

static void charging_contract(void) {
    const unsigned states[][2] = {{1,0x80},{1,0x81},{1,0x82},{4,0xa},
        {0x10,2},{0x10,3},{0x10,4},{0x10,5},{0x10,6},{0x10,7},{0x10,0xd},{0x10,0x13},
        {0x10,0x1b},{0x10,0x2d},{0x10,0x35},{0x10,0x3b},{0x10,0x3c},{0x10,0x43},{0x10,0x44}};
    const unsigned ends[][2] = {{1,0},{1,0x55},{2,0},{0x10,0},{0x10,1},{0x10,8},
        {0x10,0x1c},{0x10,0x2f},{0x10,0x33},{0x10,0x34},{0x10,0x45},{0x8010,0xd}};
    assert(sizeof states / sizeof *states == 19);
    assert(BOW_GUIDE_AIM_LR == 0x657690u && BOW_LANDING_AIM_LR == 0x656ef0u);
    world(); install(false);
    for (unsigned type = 0; type < 2; ++type) {
        manual_aim(type, false);
        for (unsigned i = 0; i < sizeof states / sizeof *states; ++i) {
            action(states[i][0], states[i][1]);
            assert(native_charge_fixture(hunter) && !native_guide_aim_fixture(hunter));
            expect_guides(hunter, 1, 1);
        }
        for (unsigned i = 0; i < sizeof ends / sizeof *ends; ++i) {
            action(0x10,0xd); expect_guides(hunter, 1, 1);
            action(ends[i][0], ends[i][1]);
            assert(!native_charge_fixture(hunter));
            expect_guides(hunter, 0, 1);
        }
        action(0x10,0xd); ST8(hunter + PL_FLAGS, 0);
        expect_guides(hunter, 0, 0);
        ST8(hunter + PL_FLAGS, 1);
        expect_guides(hunter, 1, 1);
    }
    puts("PASS: direction and landing-marker gates supplement all 19 native charging fixtures for Bow Types 1/2; release, shot, melee, cancel and sheath do not retain either added result.");
}

static void manual_and_plugin_results(void) {
    const uint32_t values[] = {1,2,0x66,UINT_MAX};
    world(); install(false); clobber_next = true;
    for (unsigned type = 0; type < 2; ++type) {
        manual_aim(type, true);
        action(0x10,0xd); expect_guides(hunter, 1, 0);
        action(1,0); expect_guides(hunter, 1, 0);
        manual_aim(type, false);
        action(1,0); expect_guides(hunter, 0, 1);
        override_gate = true;
        for (unsigned i = 0; i < sizeof values / sizeof *values; ++i) {
            gate_result = values[i];
            action(0x10,0xd); expect_guides(hunter, values[i], 0);
            action(0x10,0); expect_guides(hunter, values[i], 0);
            ST8(hunter + PL_FLAGS, 0);
            expect_guides(hunter, values[i], 0);
            ST8(hunter + PL_FLAGS, 1);
        }
        override_gate = false;
    }
    puts("PASS: native held/toggled manual aim and every nonzero plugin result remain exact without an extra charge query, including after charging or sheathing.");
}

static void isolated_calls(void) {
    // Scope/camera callers and neighboring addresses must not gain either world guide.
    const uint32_t callers[] = {0,0x81e24c,0x81e274,0x81e294,0x81e520,
        0x227898,0x4a0824,0x12345678,BOW_GUIDE_AIM_LR - 4,BOW_GUIDE_AIM_LR + 4,
        BOW_LANDING_AIM_LR - 4,BOW_LANDING_AIM_LR + 4};
    world(); install(false); action(0x10,0xd);
    override_gate = true; gate_result = 0; clobber_next = true;
    for (unsigned i = 0; i < sizeof guide_callers / sizeof *guide_callers; ++i) {
        Cpu cpu = seeded(hunter, guide_callers[i]);
        invoke(&cpu); assert_only_result(&cpu, 1);
        assert(charge_calls == i + 1 && query_input.r[0] == hunter);
        assert(next_input.r[0] == hunter && next_input.r[1] == remote && next_input.r[14] == guide_callers[i]);
    }
    for (unsigned i = 0; i < sizeof callers / sizeof *callers; ++i) {
        gate_result = 0; expect_gate(hunter, callers[i], 0, 0);
        gate_result = 0x66; expect_gate(hunter, callers[i], 0x66, 0);
    }
    assert(charge_calls == 2);
    puts("PASS: next always runs first with unchanged arguments; saved hunter/LR survive plugin clobbers; only r0 may change and all CPU/VFP bytes survive the copied query; other callers remain exact.");
}

static void identities(void) {
    const uint32_t kinds[] = {VT_HEAVY,VT_LIGHT,0x1949a88u,0,0x198bf48u};
    const uint32_t invalid[] = {0,remote,3,0xf1000000u};
    world(); install(false); override_gate = true; gate_result = 0; action(0x10,0xd);
    for (unsigned i = 0; i < sizeof kinds / sizeof *kinds; ++i) {
        ST32(hunter,kinds[i]);
        if (kinds[i] == 0x1949a88u) ST32(kinds[i] + 4 * VT_DRAWN,FN_DRAWN);
        expect_guides(hunter, 0, 0);
    }
    ST32(hunter,VT_BOW);
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i)
        expect_guides(invalid[i], 0, 0);
    assert(charge_calls == 0);
    for (unsigned missing = 0; missing < 4; ++missing) {
        world(); install(false); override_gate = true; action(0x10,0xd);
        if (missing == 0) ST32(G_GAMECONTROL,0);
        if (missing == 1) ST32(G_SPLAYER,0);
        if (missing == 2) ST32(LD32(G_GAMECONTROL) + 0x30,4);
        if (missing == 3) ST32(LD32(G_SPLAYER) + 0x1c,0);
        expect_guides(hunter, 0, 0);
    }
    for (unsigned link = 0; link < 2; ++link) {
        world(); install(false);
        ST32(LD32(G_SPLAYER) + 0x1c,0x08008000);
        ST32(0x08008000 + (link ? LINK_B : LINK_A),hunter);
        action(0x10,0xd); expect_guides(hunter, 1, 1);
    }
    puts("PASS: remote/invalid/unknown hunters, other weapons and absent scenes preserve false results without querying; both existing local-player indirections work.");
}

static void no_guest_writes(void) {
    static unsigned char image_before[sizeof image], heap_before[sizeof heap];
    world(); install(false); action(0x10,0xd);
    memset(G2H(hunter + 0x26c0),0x7d,0xa0); ST8(hunter + PL_FLAGS,1);
    style_result = 2; arc_parameter = 1; ST8(LD32(hunter + PL_DATA) + 0x4d9, 2);
    memcpy(image_before,image,sizeof image); memcpy(heap_before,heap,sizeof heap);
    expect_guides(hunter, 1, 1);
    expect_style(hunter, BOW_LANDING_STYLE_LR, 0, 1);
    assert(memcmp(image_before,image,sizeof image) == 0 && memcmp(heap_before,heap,sizeof heap) == 0);
    puts("PASS: all guest image/heap bytes stay unchanged, including aim flags, angles and camera/input canaries; no aim, lock, input or gyro API is linked.");
}

static void landing_style_contract(void) {
    const uint32_t styles[] = {0,1,2,3,4,5,6,7,0x66,UINT_MAX};
    const uint32_t parameters[] = {0,1,2,3,0x66,UINT_MAX};
    assert(BOW_GUIDE_STYLE == 0x468818u && BOW_LANDING_STYLE_LR == 0x656c78u && BOW_GUIDE_PARAM == 0x47c708u);
    world(); install(false); clobber_next = true;
    for (unsigned ready = 0; ready < 2; ++ready) {
        if (!ready) ST32(G_GAMECONTROL,0), ST32(G_SPLAYER,0);
        else ST32(G_GAMECONTROL,0x08000000), ST32(G_SPLAYER,0x08001000);
        for (unsigned i = 0; i < sizeof styles / sizeof *styles; ++i) {
            style_result = styles[i];
            bool added_style = style_result == 2 || style_result == 3 || style_result == 5;
            for (unsigned p = 0; p < sizeof parameters / sizeof *parameters; ++p) {
                arc_parameter = parameters[p];
                expect_style(hunter, BOW_LANDING_STYLE_LR, added_style && arc_parameter < 3 ? 0 : style_result, added_style);
            }
        }
    }
    puts("PASS: only initialization styles 2/3/5 with native arc parameters 0/1/2 gain a resource; supported styles and unknown/plugin results remain exact, including before the global player slot exists.");
}

static void landing_style_isolation(void) {
    const uint32_t callers[] = {0,BOW_GUIDE_AIM_LR,BOW_LANDING_AIM_LR,BOW_LANDING_STYLE_LR-4,BOW_LANDING_STYLE_LR+4,0x12345678};
    const uint32_t invalid[] = {0,3,0x07fffffcu,0xf0000000u,0xf1000000u};
    const uint32_t kinds[] = {VT_HEAVY,VT_LIGHT,0x1949a88u,0,0x198bf48u};
    world(); install(false); clobber_next = true; style_result = 2; arc_parameter = 1;
    for (unsigned i = 0; i < sizeof callers / sizeof *callers; ++i)
        expect_style(hunter, callers[i], 2, 0);
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i)
        expect_style(invalid[i], BOW_LANDING_STYLE_LR, 2, 0);
    expect_style(remote, BOW_LANDING_STYLE_LR, 2, 0);
    for (unsigned i = 0; i < sizeof kinds / sizeof *kinds; ++i) {
        ST32(hunter,kinds[i]); expect_style(hunter, BOW_LANDING_STYLE_LR, 2, 0);
    }
    ST32(hunter,VT_BOW);
    ST32(hunter+0x13cc,0); expect_style(hunter,BOW_LANDING_STYLE_LR,2,0);
    ST32(hunter+0x13cc,2); expect_style(hunter,BOW_LANDING_STYLE_LR,2,0);
    ST32(hunter+0x13cc,3); expect_style(hunter,BOW_LANDING_STYLE_LR,0,1);
    puts("PASS: the resource hook changes only its exact local-Bow initialization caller; remote/invalid hunters, other weapons and other style queries stay native; next/query CPU and VFP clobbers never leak.");
}

static void installation_failure(void) {
    for (unsigned mask = 0; mask < 4; ++mask) {
        world(); install_failures(mask); override_gate = true; action(0x10,0xd);
        expect_guides(hunter, mask & 1 ? 0 : 1, mask & 1 ? 0 : 1);
        gate_result = 0x66; expect_guides(hunter, 0x66, 0);
        style_result = 2; arc_parameter = 1;
        expect_style(hunter, BOW_LANDING_STYLE_LR, mask & 2 ? 2 : 0, mask & 2 ? 0 : 1);
        assert(next_calls == 4 && style_next_calls == 1);
    }
    puts("PASS: either or both hook installations may fail independently with native fallback; exactly the visibility and resource-style hooks, never Scope hooks, are installed.");
}

static void native_contract(void) {
    // The ARM emulator consumes these actual production-hook results instead of reimplementing the fix.
    printf("{\"style_results\":[");
    for (unsigned style = 0; style < 8; ++style) for (unsigned param = 0; param < 4; ++param) {
        world(); install(false); clobber_next = true;
        ST32(G_GAMECONTROL,0); ST32(G_SPLAYER,0);
        style_result = style; arc_parameter = param;
        Cpu cpu = seeded(hunter,BOW_LANDING_STYLE_LR); invoke_style(&cpu);
        printf("%s{\"style\":%u,\"parameter\":%u,\"result\":%u}",style || param ? "," : "",style,param,cpu.r[0]);
    }
    printf("],\"aim_results\":{");
    const char *names[] = {"charging","idle","sheathed","manual"};
    for (unsigned mode = 0; mode < 4; ++mode) {
        world(); install(false); action(0x10,0xd);
        if (mode == 1) action(1,0);
        if (mode == 2) ST8(hunter+PL_FLAGS,0);
        if (mode == 3) { action(1,0); manual_aim(0,true); }
        Cpu cpu = seeded(hunter,BOW_LANDING_AIM_LR); invoke(&cpu);
        printf("%s\"%s\":%u",mode ? "," : "",names[mode],cpu.r[0]);
    }
    printf("}}\n");
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1],"--native-contract") == 0) { native_contract(); return 0; }
    if (argc == 2 && strcmp(argv[1],"--baseline") == 0) {
        world(); install_failures(2); action(0x10,0xd); expect_guides(hunter,1,1);
        unsigned failures = 0;
        const uint32_t styles[] = {2,3,5};
        for (unsigned i = 0; i < sizeof styles / sizeof *styles; ++i) {
            style_result = styles[i]; arc_parameter = 1;
            Cpu cpu = seeded(hunter,BOW_LANDING_STYLE_LR); invoke_style(&cpu);
            failures += cpu.r[0] != 0;
        }
        action(1,0); manual_aim(0,true); expect_guides(hunter, 1, 0);
        printf("EXPECTED_BASELINE_CONTRACT_FAILURES=%u (visibility-only hooks leave styles 2/3/5 unable to create the landing resource)\n",failures);
        return failures == 3 ? 1 : 2;
    }
    charging_contract(); manual_and_plugin_results(); isolated_calls(); identities(); landing_style_contract(); landing_style_isolation(); no_guest_writes(); installation_failure();
    puts("PASS: bow-charge-world-guides-v3 (production visibility/resource hooks with synthetic guest memory; native motion/collision/rendering not executed here; no device run).");
    return 0;
}
