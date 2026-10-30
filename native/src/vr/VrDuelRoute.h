#pragma once
// VR DUEL (VrDuelControl.h has the rest): the one word engine::WeaponContextReader needs. While set, the player's duel state does not
// count as engine::WeaponContext::duel (holsters, force aim, aim override, hammer, BODY, reticle hiding, gun-up bypass all read that flag);
// duelRaw keeps the true state for the few readers that must stay duel-aware (locomotion, VR melee, reload veto).
#include <atomic>

namespace rdrvr::vr::duelctl {
inline std::atomic<bool> route{};
inline bool live() noexcept {return route.load(std::memory_order_relaxed);}
} // namespace rdrvr::vr::duelctl
