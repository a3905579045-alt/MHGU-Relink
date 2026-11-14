// Bow charge shows the native direction guide and arc-shot landing circle.
// main v1.4.0 (fb08f1d20fd1204fe7070672777f869b): uShellPlEffectW10 effect 4 asks
// 0x13b7750 at LR 0x657690; landing effects 5/6/7 ask at LR 0x656ef0.
// The local resource setup also needs slot 2 for styles without a native arc-shot marker.
// Charge level, suppression and collision checks still govern the landing circle.
#include "../hle.h"
#include "mousecam.h"

#define BOW_GUIDE_AIM         0x13b7750u
#define BOW_GUIDE_AIM_LR      0x657690u
#define BOW_LANDING_AIM_LR    0x656ef0u
#define BOW_GUIDE_CHARGING    0x13b93dcu  // pure act_st/act_no query, Bow virtual +0x694
#define BOW_GUIDE_DRAWN_FLAGS 0x2749u
#define BOW_GUIDE_STYLE       0x468818u
#define BOW_LANDING_STYLE_LR  0x656c78u
#define BOW_GUIDE_PARAM       0x47c708u   // (hunter, parameter): native Bow resource parameter accessor

static void bow_landing_style(Cpu *c) {
    uint32_t hunter = c->r[0];
    // This native local-owner check is available before the global player slot is published.
    bool ours = c->r[14] == BOW_LANDING_STYLE_LR && hunter >= 0x08000000u && hunter < 0xf0000000u &&
                !(hunter & 3) && weapon_ranged(hunter) == WEAPON_BOW && (LD32(hunter + 0x13cc) & 1);
    fnhook_call_next(c, BOW_GUIDE_STYLE);
    uint32_t style = c->r[0];
    if (!ours || (style != 2 && style != 3 && style != 5)) return;
    Cpu query = *c;
    query.r[0] = hunter, query.r[1] = 0x1b;
    guest_call(&query, BOW_GUIDE_PARAM);
    if (query.r[0] >= 3) return;  // The native marker-resource table has only three entries.
    c->r[0] = 0;  // Allow only this resource branch; the hunter's actual style stays unchanged.
    LOG("[bow_guide] enabled landing resource for style %u (bow parameter %u)\n", style, query.r[0]);
}

static void bow_guide_visible(Cpu *c) {
    uint32_t hunter = c->r[0];
    bool ours = (c->r[14] == BOW_GUIDE_AIM_LR || c->r[14] == BOW_LANDING_AIM_LR) && hunter &&
                hunter == weapon_hunter() && weapon_ranged(hunter) == WEAPON_BOW;
    fnhook_call_next(c, BOW_GUIDE_AIM);
    // Preserve manual/plugin visibility; only add the local charging guides.
    if (!ours || c->r[0] || !(LD8(hunter + BOW_GUIDE_DRAWN_FLAGS) & 1)) return;
    // guest_call preserves only LR; isolate the query from the next hook's CPU/VFP result.
    Cpu query = *c;
    query.r[0] = hunter;
    guest_call(&query, BOW_GUIDE_CHARGING);
    if (query.r[0]) c->r[0] = 1;
}

void bow_reticle_init(void) {
    if (!fnhook_set_outer(BOW_GUIDE_AIM, hle_register("bow_guide:visible", bow_guide_visible)))
        LOG("[bow_guide] disabled: native charge-guide hook unavailable\n");
    if (!fnhook_set_outer(BOW_GUIDE_STYLE, hle_register("bow_guide:landing_style", bow_landing_style)))
        LOG("[bow_guide] native landing-resource hook unavailable\n");
}
