#pragma once
// VR settings for the VR menu page (rdrvr/vr.cfg next to graphics.cfg), the module status codes the page shows
// as row suffixes, and the debug.rdrvr.vr.<feature> kill switches. Header-only (no new TU for build_library.py).
//
//   rdrvr::vr::settings()            effective snapshot (kill switches applied), lock-free, any thread
//   rdrvr::vr::vrSettings.requested() what the menu/vr.cfg asks for (no kill switches)
//   rdrvr::vr::status::publish(...)  module state for the row suffix (" - UNAVAILABLE", " - PROP", " - FAILED", " - <NOTE>")
//   rdrvr::vr::enabled(Feature)      false when "setprop debug.rdrvr.vr.<name> 0" was set at startup: the module installs no hooks
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr {

// ---- keys (vr.cfg key = cfg, menu row = label, value index = position in names) ----
enum class Key : unsigned {Locomotion,MoveSpeed,Roomscale,Camera,EyeHeight,BumpReactions,Stumble,Hands,Holsters,WeaponGrip,HolsterMarkers,
    TwoHandAim,Melee,MeleeSpeed,MeleeFists,Laser,AutoCover,DualWield,Body,BodyUp,BodyBack,ThrowWeapon,FistBlock,
    TeleportFade,TeleportRange,LassoThrow,LassoAssist,ReloadMode,CylinderClose,Chambers,Hammer,FanSpread,GunSpin,QuestMarker,Cutscenes,Hud,HudSize,Subtitles,RideUp,RideFwd,TurnMode,Count};
// variable-width packing - each key takes ceil(log2(count)) bits (2 values = 1 bit, 3..4 = 2, 5..8 = 3), offsets in key order
// . 22 keys = 31 bits; FIST BLOCK: 23 keys = 32 bits. vr.cfg (key=value text) is unchanged.
// tp: LOCOMOTION widened in place to 3 values (2 bits) + TELEPORT FADE (1) + TELEPORT RANGE (2) = 25 keys / 36 bits.
// lasso: + LASSO THROW (2 bits) + LASSO ASSIST (1) = 27 keys / 39 bits.
// reload: + RELOAD MODE (2) + CYLINDER CLOSE (2) = 29 keys / 43 bits.
// hammer: + HAMMER (4 values, 2 bits) + FAN SPREAD (3 values, 2 bits) = 31 keys / 47 bits.
// SPIN: + GUN SPIN (1 bit) after FAN SPREAD = 32 keys / 48 bits (vr.cfg is by name).
// HAMMER -> 2 values (CASUAL / REALISTIC, 1 bit, cfg "hammerStyle") = 32 keys / 47 bits.
// + CHAMBERS (SIMPLE / REALISTIC, 1 bit) after CYLINDER CLOSE = 33 keys / 48 bits.
// + QUEST MARKER (OFF / ON, 1 bit, cfg "questMarker", appended) = 34 keys / 49 bits.
// + CUTSCENES (SCREEN / CINEMATIC / FIRST PERSON, 2 bits) last = 35 keys / 51 bits.
// + HUD (OFF / HEAD / HAND, 2 bits) + HUD SIZE (SMALL / MEDIUM / LARGE, 2 bits) = 37 keys / 55 bits (cfg "hud", "hudSize").
inline constexpr unsigned keyCount=unsigned(Key::Count);
// Kill-switch features (debug.rdrvr.vr.<name>) = the status slots of the modules.
enum class Feature : unsigned {Menu,Loco,Bump,Stumble,Camera,Roomscale,Hands,Holsters,TwoHand,Cheats,Melee,Laser,Cover,Dual,Body,Throw,
    Arc,TpLoco,Lasso,Reload,Hammer,Spin,Marker,Cutscene,Hud,Count};
inline constexpr unsigned featureCount=unsigned(Feature::Count);
inline constexpr std::array<const char*,featureCount> featureNames{"menu","loco","bump","stumble","camera","roomscale","hands","holsters",
    "twohand","cheats","melee","laser","cover","dual","body","throw",
    "arc","tploco","lasso","reload","hammer","spin","marker","cutscene","hud"}; // Feature::Reload = the property VrReload reads itself (same name / meaning)

struct KeyInfo {
    const char* cfg;const char* label;unsigned count,defaultValue;
    Feature feature;      // kill switch that forces legacyValue
    int legacyValue;      // effective value while the feature is killed (-1 = unchanged)
    bool showNote;        // row shows the module's status note (" - ACTIVE")
    std::array<const char*,16> names;
};
// Panel glyphs only (A-Z 0-9 space . : / - % ? < >), labels <= 16 columns.
inline constexpr std::array<KeyInfo,keyCount> keyInfo{{
    // LOCOMOTION: TELEPORT is index 2 (FREE is the default). Its effective value has
    // its own kill rules (effective() below): TELEPORT survives a Loco kill, a TpLoco kill turns it into FREE, both -> NATIVE.
    {"locomotion",    "LOCOMOTION",     3,0,Feature::Loco,     1,true, {"FREE","NATIVE","TELEPORT"}},
    {"moveSpeed",     "MOVE SPEED",     4,1,Feature::Loco,    -1,false,{"80%","100%","120%","150%"}},
    {"roomscale",     "ROOMSCALE",      2,0,Feature::Roomscale,0,true, {"OFF","ON"}},
    {"camera",        "CAMERA",         2,0,Feature::Camera,   1,true, {"BODY","HEAD"}},
    {"eyeHeight",     "EYE HEIGHT",     5,0,Feature::Camera,  -1,false,{"AUTO","1.50","1.60","1.70","1.80"}},
    {"bumpReactions", "BUMP REACTIONS", 2,0,Feature::Bump,     1,true, {"OFF","NATIVE"}},
    {"stumble",       "STUMBLE",        2,0,Feature::Stumble,  1,true, {"OFF","NATIVE"}},
    // HANDS: the floating VR hand meshes default OFF (while BODY is active they are not drawn at all).
    {"hands",         "HANDS",          2,0,Feature::Hands,    0,true, {"OFF","ON"}},
    {"holsters",      "HOLSTERS",       2,1,Feature::Holsters, 0,true, {"OFF","ON"}},
    {"weaponGrip",    "WEAPON GRIP",    2,0,Feature::Holsters,-1,false,{"HOLD","TOGGLE"}},
    {"holsterMarkers","HOLSTER MARKERS",3,0,Feature::Holsters, 2,false,{"NEAR","ALWAYS","OFF"}},
    {"twoHandAim",    "TWO HAND AIM",   2,1,Feature::TwoHand,  0,true, {"OFF","ON"}},
    // MELEE: physical melee (Gothic port) or the native trigger melee; strike speed (m/s, Gothic
    // default 2.2); fists with an empty closed hand. debug.rdrvr.vr.melee 0 forces NATIVE.
    {"melee",         "MELEE",          2,0,Feature::Melee,    1,true, {"PHYSICAL","NATIVE"}},
    {"meleeSpeed",    "MELEE SPEED",    4,1,Feature::Melee,   -1,false,{"1.8","2.2","2.6","3.0"}},
    {"meleeFists",    "MELEE FISTS",    2,1,Feature::Melee,   -1,false,{"OFF","ON"}},
    // LASER SIGHT: aim laser from the held firearm's muzzle along the real shot direction; debug.rdrvr.vr.laser 0 forces OFF.
    {"laser",         "LASER SIGHT",    2,0,Feature::Laser,    0,true, {"OFF","ON"}},
    // AUTO COVER: the engine's automatic take-cover (the player snaps behind crates/walls); OFF blocks
    // it, debug.rdrvr.vr.cover 0 = NATIVE.
    {"autoCover",     "AUTO COVER",     2,0,Feature::Cover,    1,true, {"OFF","NATIVE"}},
    // DUAL: hand-to-hand transfer, a second item in the other hand (knife + revolver, ...), swap-fire of
    // two firearms. debug.rdrvr.vr.dual 0 forces OFF.
    {"dualWield",     "DUAL WIELD",     2,1,Feature::Dual,     0,true, {"OFF","ON"}},
    // BODY: the character's own body in the eyes (no head), both arms IK-driven to the controllers, the
    // held weapon in the skeletal hand. debug.rdrvr.vr.body 0 forces OFF (no hooks installed).
    {"body",          "BODY",           2,1,Feature::Body,     0,true, {"OFF","ON"}},
    // Where the character's body sits relative to the eyes while BODY is shown. UP = the eye above
    // the head joint (camera AUTO eye height = head joint + UP), BACK = the head joint behind the
    // eye (the body is translated so its head joint sits UP below / BACK behind the HMD; the head
    // pivot model). Larger = the body lower / further back (= the view higher / further forward).
    {"bodyUp",        "BODY LOWER",     8,4,Feature::Body,    -1,false,{"0 CM","4 CM","8 CM","12 CM","16 CM","20 CM","24 CM","28 CM"}},
    {"bodyBack",      "BODY BACK",      5,2,Feature::Body,   -1,false,{"0 CM","5 CM","10 CM","15 CM","20 CM"}},
    // THROW: open the grip while the hand moves fast -> the held revolver flies; catch it with either hand
    // (air / ground), it flies back to the holster on its own. debug.rdrvr.vr.throw 0 forces OFF.
    {"throwWeapon",   "THROW WEAPON",   2,1,Feature::Throw,    0,true, {"OFF","ON"}},
    // FIST BLOCK: both empty fists squeezed and raised like a boxing guard block NPC melee strikes (engine
    // block state). Needs MELEE PHYSICAL + MELEE FISTS ON; debug.rdrvr.vr.melee 0 (whole melee) / debug.rdrvr.vr.melee.block 0 (this only).
    {"fistBlock",     "FIST BLOCK",     2,1,Feature::Melee,   -1,false,{"OFF","ON"}},
    // TELEPORT locomotion: the blink (OpenXR colour scale) or an instant cut; the arc's reach (v0 so that a
    // 45 deg throw from 1.2 m above the feet lands at the range). debug.rdrvr.vr.tploco 0 -> rows PROP, LOCOMOTION TELEPORT acts FREE.
    {"teleportFade",  "TELEPORT FADE",  2,0,Feature::TpLoco,  -1,false,{"BLINK","INSTANT"}},
    {"teleportRange", "TELEPORT RANGE", 3,1,Feature::TpLoco,  -1,false,{"6 M","9 M","12 M"}},
    // LASSO THROW / LASSO ASSIST: how the lasso is thrown (SWING = swing and release the throw-hand trigger,
    // TRIGGER = half press aims / full press throws, NATIVE = the engine's own aim + throw from the throw hand's trigger) and the height assist
    // (pitch solve + the engine's horizontal homing toward the chosen target). debug.rdrvr.vr.lasso 0 -> rows PROP, NATIVE / OFF.
    // LASSO THROW values: RELEASE (default: any trigger >= .3 shows the arc, letting go throws along it) / SWING (trigger shows the arc,
    // a forward hand swing throws, or the let-go at the end of it; a let-go without a swing = RELEASE) / NATIVE.
    {"lassoThrow",    "LASSO THROW",    3,0,Feature::Lasso,    2,true, {"RELEASE","SWING","NATIVE"}},
    {"lassoAssist",   "LASSO ASSIST",   2,1,Feature::Lasso,    0,false,{"OFF","ON"}},
    // RELOAD MODE: the manual revolver reload (B opens the cylinder, eject,
    // rounds from the chest / belt one by one or a whole speedloader) or the game's own reload; NATIVE also when debug.rdrvr.vr.reload 0 (PROP)
    // or the install failed (UNAVAILABLE). CYLINDER CLOSE: which gestures close the open
    // cylinder besides B (B AND HAND = + the free hand's push, ALL = + wrist flick / wave); no row note (the SHADOW state is in the report).
    {"reloadMode",    "RELOAD MODE",    3,2,Feature::Reload,   2,true, {"SINGLE","FULL","NATIVE"}},
    {"cylinderClose", "CYLINDER CLOSE", 3,2,Feature::Reload,  -1,false,{"B ONLY","B AND HAND","ALL"}},
    // CHAMBERS: SIMPLE (default: AUTO INDEX to the first live round on every close) / REALISTIC (the
    // chamber aligned with the barrel decides: live fires, spent / empty = a dry click; cocking / the
    // trigger turns the cylinder one chamber first; a swipe across the closed cylinder spins it with
    // friction). Reload killed -> SIMPLE.
    {"chambers",      "CHAMBERS",       2,0,Feature::Reload,   0,false,{"SIMPLE","REALISTIC"}},
    // HAMMER / FAN SPREAD: fanning the revolver's hammer with the free palm. FAN SPREAD = the cone of a
    // FAN series (LOW 1.5 deg, HIGH 4 deg). debug.rdrvr.vr.hammer 0 -> rows PROP, HAMMER OFF.
    // HAMMER has exactly two behaviours. CASUAL (default) = FAN (the trigger fires; a hammer drag / swipe to full cock fires too).
    // REALISTIC = single-action revolvers fire only a cocked hammer (drag / swipe it back, then the trigger), the trigger HELD while the other
    // hand drags / slaps the hammer to full cock fires at once (fanning); double-action / non-revolvers act CASUAL. Cfg key "hammerStyle"
    // (the legacy "hammer" key 0..3 is migrated in parse(): SINGLE ACTION -> REALISTIC, the rest -> CASUAL). Kill switch: no legacy value (the
    // module itself is off).
    {"hammerStyle",   "HAMMER",         2,0,Feature::Hammer,  -1,true, {"CASUAL","REALISTIC"}},
    {"fanSpread",     "FAN SPREAD",     3,1,Feature::Hammer,  -1,false,{"OFF","LOW","HIGH"}},
    // SPIN: open the grip with the index on the trigger -> the revolver hangs / spins on the
    // finger. debug.rdrvr.vr.spin 0 -> row PROP, OFF.
    // ON = every HOLD release away from the holster hangs on the finger (note "HANGS ON FINGER  TILT TO DROP"),
    // OFF = it falls / flies (note "RELEASE DROPS"); near the holster both holster.
    {"gunSpin",       "GUN SPIN",       2,0,Feature::Spin,     0,true, {"OFF","ON"}},
    // QUEST MARKER: the yellow beacon in both eyes at the radar's objective blips / mission GPS destination (VrMarker.h).
    // debug.rdrvr.vr.marker 0 -> row PROP, OFF (no module, nothing read).
    {"questMarker",   "QUEST MARKER",   2,1,Feature::Marker,   0,true, {"OFF","ON"}},
    // CUTSCENES: on the flat theater quad (SCREEN) or in stereo from the
    // cutscene camera (CINEMATIC, default) / John's eyes (FIRST PERSON, falls back to CINEMATIC). debug.rdrvr.vr.cutscene 0 -> PROP, SCREEN.
    {"cutscenes",     "CUTSCENES",      3,1,Feature::Cutscene, 0,true, {"SCREEN","CINEMATIC","FIRST PERSON"}},
    // HUD: the game's 2D HUD as OpenXR quad layers. HEAD = the whole HUD on a quad in front
    // (lazy yaw follow), HAND = the radar on the back of the LEFT wrist + the rest of the HUD in front. debug.rdrvr.vr.hud 0 -> OFF - PROP.
    {"hud",           "HUD",            3,1,Feature::Hud,      0,true, {"OFF","HEAD","HAND"}},
    {"hudSize",       "HUD SIZE",       3,1,Feature::Hud,     -1,false,{"SMALL","MEDIUM","LARGE"}},
    // SUBTITLES: GAME = the game's own option decides, OFF = hudManager::SetSubtitlesEnabled(false) kept (objectives / hints stay).
    {"subtitles",     "SUBTITLES",      2,0,Feature::Hud,     -1,false,{"GAME","OFF"}},
    // HORSE CAMERA (user: on a horse the view sat a bit behind and low): the riding eye moved up / forward along the rider's heading.
    // (user: not enough range): -30..+45 cm in 5 cm steps (user), default 0 (new cfg keys: the old 0..28 cm indices are not reused)
    {"rideUpCm",      "HORSE CAM UP",   16,9,Feature::Camera, -1,false,{"-30 CM","-25 CM","-20 CM","-15 CM","-10 CM","-5 CM","0 CM","+5 CM","+10 CM","+15 CM","+20 CM","+25 CM","+30 CM","+35 CM","+40 CM","+45 CM"}},
    // FWD back to 8 values (-10..+25 cm, 5 cm steps; new key) to free the bit TURN needs (64-bit packing)
    {"rideFwd5",      "HORSE CAM FWD",  8,4,Feature::Camera,  -1,false,{"-10 CM","-5 CM","0 CM","+5 CM","+10 CM","+15 CM","+20 CM","+25 CM"}},
    // TURN (user): SNAP (30 deg steps, default - for those who get sick) / SMOOTH (stick-proportional, debug.rdrvr.vr.turn.speed deg/s)
    {"turnMode",      "TURN",           2,0,Feature::Camera,  -1,false,{"SNAP","SMOOTH"}},
}};
// ---- variable-width packing ----
inline constexpr unsigned bitsFor(unsigned count) noexcept {unsigned b=0;while(b<32&&(1u<<b)<count)++b;return b;}
inline constexpr std::array<unsigned,keyCount> keyWidths=[]{std::array<unsigned,keyCount> w{};for(unsigned i=0;i<keyCount;++i)w[i]=bitsFor(keyInfo[i].count);return w;}();
inline constexpr std::array<unsigned,keyCount> keyShifts=[]{std::array<unsigned,keyCount> s{};unsigned at=0;for(unsigned i=0;i<keyCount;++i){s[i]=at;at+=keyWidths[i];}return s;}();
inline constexpr unsigned packedBits=[]{unsigned n=0;for(unsigned w:keyWidths)n+=w;return n;}();
inline constexpr unsigned keyShift(unsigned k) noexcept {return keyShifts[k];}
inline constexpr std::uint64_t keyMaskOf(unsigned k) noexcept {return (std::uint64_t(1)<<keyWidths[k])-1u;}
// value v of key k written into p (the other keys untouched)
inline constexpr std::uint64_t withValue(std::uint64_t p,unsigned k,unsigned v) noexcept {
    return (p&~(keyMaskOf(k)<<keyShift(k)))|((std::uint64_t(v)&keyMaskOf(k))<<keyShift(k));}
static_assert(packedBits<=64,"Snapshot packs every key (variable width) into 64 bits");
static_assert(keyCount<=64,"vr.cfg parse tracks the seen keys in 64 bits");
inline constexpr bool keyCountsFit() noexcept {for(unsigned i=0;i<keyCount;++i){const auto& k=keyInfo[i];
    if(k.count<2||k.count>(1u<<keyWidths[i])||k.defaultValue>=k.count||keyWidths[i]<1||keyWidths[i]>4)return false;}return true;} // 4 bits (16 values) allowed
static_assert(keyCountsFit(),"every key value must fit its packed width");
inline constexpr const KeyInfo& info(Key k) noexcept {return keyInfo[unsigned(k)];}
inline constexpr std::uint64_t defaultPacked() noexcept {
    std::uint64_t p{};for(unsigned i=0;i<keyCount;++i)p=withValue(p,i,keyInfo[i].defaultValue);return p;}

// One consistent menu state. Typed getters are what the modules read.
struct Snapshot {
    std::uint64_t packed{defaultPacked()};
    constexpr unsigned operator[](Key k) const noexcept {return unsigned((packed>>keyShift(unsigned(k)))&keyMaskOf(unsigned(k)));}
    constexpr bool freeLocomotion() const noexcept {return (*this)[Key::Locomotion]==0;}             // LOCOMOTION FREE
    constexpr bool teleportLocomotion() const noexcept {return (*this)[Key::Locomotion]==2;}         // LOCOMOTION TELEPORT
    constexpr bool locoOwnsCapsule() const noexcept {const unsigned v=(*this)[Key::Locomotion];return v==0||v==2;} // FREE / TELEPORT: LOCO mover path (roomscale, escape walk)
    constexpr bool teleportBlink() const noexcept {return (*this)[Key::TeleportFade]==0;}            // TELEPORT FADE BLINK
    constexpr float teleportRangeMetres() const noexcept {const unsigned v=(*this)[Key::TeleportRange];return v==0?6.f:v==2?12.f:9.f;}
    constexpr float moveSpeedScale() const noexcept {const unsigned v=(*this)[Key::MoveSpeed];return v==0?.8f:v==2?1.2f:v==3?1.5f:1.f;}
    constexpr bool roomscale() const noexcept {return (*this)[Key::Roomscale]==1;}
    constexpr bool bodyCamera() const noexcept {return (*this)[Key::Camera]==0;}                    // CAMERA BODY
    constexpr float eyeHeightMeters() const noexcept {const unsigned v=(*this)[Key::EyeHeight];      // 0 = AUTO (calibrate)
        return v==1?1.5f:v==2?1.6f:v==3?1.7f:v==4?1.8f:0.f;}
    constexpr bool suppressBumps() const noexcept {return (*this)[Key::BumpReactions]==0;}         // BUMP REACTIONS OFF
    constexpr bool suppressStumble() const noexcept {return (*this)[Key::Stumble]==0;}             // STUMBLE OFF
    constexpr bool hands() const noexcept {return (*this)[Key::Hands]==1;}
    constexpr bool holsters() const noexcept {return (*this)[Key::Holsters]==1;}
    constexpr bool gripToggle() const noexcept {return (*this)[Key::WeaponGrip]==1;}               // WEAPON GRIP TOGGLE
    enum Markers : unsigned {MarkersNear=0,MarkersAlways=1,MarkersOff=2};
    constexpr unsigned holsterMarkers() const noexcept {return (*this)[Key::HolsterMarkers];}
    constexpr bool twoHandAim() const noexcept {return (*this)[Key::TwoHandAim]==1;}
    constexpr bool physicalMelee() const noexcept {return (*this)[Key::Melee]==0;}
    constexpr float meleeSpeed() const noexcept {const unsigned v=(*this)[Key::MeleeSpeed];return v==0?1.8f:v==2?2.6f:v==3?3.f:2.2f;}
    constexpr bool meleeFists() const noexcept {return (*this)[Key::MeleeFists]==1;}
    constexpr bool laser() const noexcept {return (*this)[Key::Laser]==1;}                         // LASER SIGHT ON
    constexpr bool suppressAutoCover() const noexcept {return (*this)[Key::AutoCover]==0;}         // AUTO COVER OFF
    constexpr bool dualWield() const noexcept {return (*this)[Key::DualWield]==1;}                 // DUAL WIELD ON
    constexpr bool body() const noexcept {return (*this)[Key::Body]==1;}
    constexpr float bodyEyeUpMeters() const noexcept {return .04f*float((*this)[Key::BodyUp]);}
    constexpr float bodyBackMeters() const noexcept {return .05f*float((*this)[Key::BodyBack]);}
    constexpr bool throwWeapon() const noexcept {return (*this)[Key::ThrowWeapon]==1;}             // THROW WEAPON ON
    constexpr bool fistBlock() const noexcept {return (*this)[Key::FistBlock]==1;}                 // FIST BLOCK ON
    constexpr unsigned lassoThrowMode() const noexcept {const unsigned v=(*this)[Key::LassoThrow];return v<3?v:0u;} // Lasso: 0 RELEASE, 1 SWING, 2 NATIVE
    constexpr bool lassoAssist() const noexcept {return (*this)[Key::LassoAssist]==1;}             // Lasso: LASSO ASSIST ON
    constexpr unsigned reloadMode() const noexcept {const unsigned v=(*this)[Key::ReloadMode];return v<3?v:0u;}        // Reload: 0 SINGLE, 1 FULL, 2 NATIVE
    constexpr unsigned cylinderCloseMode() const noexcept {const unsigned v=(*this)[Key::CylinderClose];return v<3?v:2u;} // Reload: 0 B ONLY, 1 B AND HAND, 2 ALL
    constexpr bool realisticChambers() const noexcept {return (*this)[Key::Chambers]==1;}
    constexpr unsigned hammerStyle() const noexcept {return (*this)[Key::Hammer]?1u:0u;}
    constexpr float fanSpreadDeg() const noexcept {const unsigned v=(*this)[Key::FanSpread];return v==0?0.f:v==2?4.f:1.5f;} // Hammer: OFF / LOW / HIGH
    constexpr bool gunSpin() const noexcept {return (*this)[Key::GunSpin]==1;}                     // SPIN: GUN SPIN ON
    constexpr bool questMarker() const noexcept {return (*this)[Key::QuestMarker]==1;}             // QUEST MARKER ON
    constexpr unsigned cutsceneMode() const noexcept {const unsigned v=(*this)[Key::Cutscenes];return v<3?v:1u;} // 0 SCREEN, 1 CINEMATIC, 2 FIRST PERSON
    constexpr unsigned hudMode() const noexcept {const unsigned v=(*this)[Key::Hud];return v<3?v:0u;}       // 0 OFF, 1 HEAD, 2 HAND
    constexpr bool subtitlesOff() const noexcept {return (*this)[Key::Subtitles]==1;}
    constexpr float rideUpMeters() const noexcept {return .05f*(float((*this)[Key::RideUp])-6.f);}   // HORSE CAM UP -30..+40 cm
    constexpr float rideFwdMeters() const noexcept {return .05f*(float((*this)[Key::RideFwd])-2.f);} // HORSE CAM FWD -10..+25 cm
    constexpr bool smoothTurn() const noexcept {return (*this)[Key::TurnMode]==1;}
    constexpr float hudWidthDeg() const noexcept {const unsigned v=(*this)[Key::HudSize];return v==0?40.f:v==2?62.f:50.f;}
    constexpr bool operator==(const Snapshot&) const noexcept = default;
};

// ---- WEAPON PRESET (user): one HANDS / WEAPONS row sets the weapon-handling keys at once. Not a key (no packed bits, nothing in
// vr.cfg): the row shows the preset the current values match exactly, CUSTOM after any of these rows was changed by hand.
// REALISTIC = finger spin, manual reload round by round, cylinder spin / aligned chamber, single-action hammer; CASUAL = the trigger
// just fires, B = the game's reload, an opened grip drops / throws the gun (no finger spin). THROW WEAPON / FAN SPREAD / LASSO stay.
enum WeaponPreset : unsigned {PresetRealistic,PresetCasual,PresetCustom,PresetCount};
inline constexpr std::array<const char*,PresetCount> weaponPresetNames{"REALISTIC","CASUAL","CUSTOM"};
struct WeaponPresetValue {Key key;unsigned realistic,casual;};
inline constexpr std::array<WeaponPresetValue,5> weaponPresetKeys{{
    {Key::GunSpin,1,0},        // ON / OFF
    {Key::ReloadMode,0,2},     // SINGLE / NATIVE
    {Key::CylinderClose,2,2},  // ALL / ALL (unused while NATIVE)
    {Key::Chambers,1,0},       // REALISTIC / SIMPLE
    {Key::Hammer,1,0}}};       // REALISTIC / CASUAL
inline constexpr bool weaponPresetsFit() noexcept {for(const auto& p:weaponPresetKeys){
    if(p.realistic>=info(p.key).count||p.casual>=info(p.key).count)return false;}return true;}
static_assert(weaponPresetsFit(),"every preset value must be a value of its key");
inline constexpr std::uint64_t withWeaponPreset(std::uint64_t p,unsigned preset) noexcept {
    if(preset>=PresetCustom)return p;
    for(const auto& v:weaponPresetKeys)p=withValue(p,unsigned(v.key),preset==PresetRealistic?v.realistic:v.casual);return p;}
inline constexpr unsigned weaponPresetOf(const Snapshot& s) noexcept {
    for(unsigned preset=PresetRealistic;preset<PresetCustom;++preset)if(withWeaponPreset(s.packed,preset)==s.packed)return preset;
    return PresetCustom;}

// ---- kill switches: debug.rdrvr.vr.<name> = 0 at startup -> feature off, no hooks ----
// 0 = enabled (also before the properties are read), 1 = killed.
inline std::array<std::atomic<int>,featureCount> killSwitch{};
inline std::atomic<bool> killSwitchRead{false};
inline bool killed(Feature f) noexcept {return killSwitch[unsigned(f)].load(std::memory_order_acquire)==1;}
inline bool enabled(Feature f) noexcept {return !killed(f);}
using PropertyReader=bool(*)(const char* name,char* value,std::size_t size) noexcept;
#ifdef __ANDROID__
inline bool androidProperty(const char* name,char* value,std::size_t size) noexcept {
    char buffer[PROP_VALUE_MAX]{};const int n=__system_property_get(name,buffer);
    if(n<=0||!size)return false;
    std::snprintf(value,size,"%s",buffer);return true;
}
#endif
// Call once at startup before the modules install hooks. A value starting with '0' kills the feature; anything else
// (or no property) keeps it. Repeated calls re-read (host tests); the device calls it once.
inline void loadKillSwitches(PropertyReader read) noexcept {
    for(unsigned i=0;i<featureCount;++i){
        char name[64],value[96]{};std::snprintf(name,sizeof name,"debug.rdrvr.vr.%s",featureNames[i]);
        const bool set=read&&read(name,value,sizeof value);
        killSwitch[i].store(set&&value[0]=='0'?1:0,std::memory_order_release);
    }
    killSwitchRead.store(true,std::memory_order_release);
}
#ifdef __ANDROID__
inline void loadKillSwitches() noexcept {loadKillSwitches(&androidProperty);}
#endif
// A key is forced to its legacy value while its feature is killed. ROOMSCALE is also forced OFF while CAMERA is killed
// (roomscale needs the body anchor); the menu page itself (Feature::Menu) never forces a value.
inline bool keyKilled(Key k) noexcept {
    const auto& i=info(k);
    // The LOCOMOTION row is frozen only when both Loco and TpLoco are killed (TELEPORT survives a Loco kill,
    // FREE / NATIVE survive a TpLoco kill); the value rules are locomotionEffective(), the row suffix locomotionKilledFor().
    if(k==Key::Locomotion)return killed(Feature::Loco)&&killed(Feature::TpLoco);
    return killed(i.feature)||(k==Key::Roomscale&&killed(Feature::Camera));
}
// The effective LOCOMOTION value for a requested one:
//   TELEPORT: TpLoco killed -> FREE (both killed -> NATIVE = the legacy value); a Loco kill alone keeps TELEPORT (the warp needs no mover hook).
//   FREE / NATIVE: the requested value; a Loco kill forces NATIVE.
inline unsigned locomotionEffective(unsigned requested) noexcept {
    const bool loco=killed(Feature::Loco),tp=killed(Feature::TpLoco);
    if(requested==2)return tp?(loco?1u:0u):2u;
    return loco?1u:requested;
}
// The LOCOMOTION row's PROP suffix: the requested mode is overridden by a kill switch.
inline bool locomotionKilledFor(unsigned requested) noexcept {return requested==2?killed(Feature::TpLoco):killed(Feature::Loco);}

// ---- module status for the row suffixes (rdrvr::vr::status) ----
namespace status {
enum Code : int {Unknown=-1,Unavailable=0,Ready=1,Property=2,Failed=3};
struct Slot {std::atomic<int> code{Unknown};std::atomic<const char*> note{nullptr};};
inline std::array<Slot,featureCount> slots{};
// note: static upper-case text in panel glyphs (e.g. "ACTIVE", "LOCKED", "RAGDOLL", "MOUNTED"), nullptr = none; any thread.
inline void publish(Feature f,Code code,const char* note=nullptr) noexcept {
    auto& s=slots[unsigned(f)];s.note.store(note,std::memory_order_relaxed);s.code.store(code,std::memory_order_release);}
inline void note(Feature f,const char* text) noexcept {slots[unsigned(f)].note.store(text,std::memory_order_release);}
inline Code code(Feature f) noexcept {return Code(slots[unsigned(f)].code.load(std::memory_order_acquire));}
inline const char* noteOf(Feature f) noexcept {return slots[unsigned(f)].note.load(std::memory_order_acquire);}
inline void resetAll() noexcept {for(auto& s:slots){s.code.store(Unknown);s.note.store(nullptr);}}
// CAMERA: the calibrated AUTO eye height in millimetres (-1 = not calibrated yet); the EYE HEIGHT row shows "AUTO 1.68".
inline std::atomic<int> eyeHeightMm{-1};
// BODY: true while the BODY module owns the player's arms (menu BODY ON + module installed): the HANDS pass draws no hand
// meshes (the laser stays). Written by VrBody.h, read by HandsPass (render thread).
inline std::atomic<bool> bodyHidesHands{false};
// BODY: the camera's AUTO eye height while the body is shown. The native head query sees the
// COLLAPSED head, so VrBody publishes the animated head joint above the skeleton root (= capsule
// base) from hook A (mm, -1 = none) and whether the head collapse is armed (then native head
// samples are not used for the calibration).
inline std::atomic<int> bodyHeadMm{-1};
inline std::atomic<bool> bodyCollapsing{false};
} // namespace status

// ---- RECENTER row: request counter, consumed by the XR thread next to the stick-click recenter chord ----
inline std::atomic<std::uint32_t> recenterRequests{0};
inline void requestRecenter() noexcept {recenterRequests.fetch_add(1,std::memory_order_acq_rel);}
// Returns true once per request batch: `seen` is the caller's last observed counter (XR-thread local).
inline bool takeRecenter(std::uint32_t& seen) noexcept {
    const auto now=recenterRequests.load(std::memory_order_acquire);if(now==seen)return false;seen=now;return true;}

namespace detail {
struct PreserveErrno {int value{errno};~PreserveErrno(){errno=value;}};
inline std::filesystem::path utf8Path(const std::string& value) {return std::filesystem::path(std::u8string(value.begin(),value.end()));}
inline std::FILE* openFile(const std::filesystem::path& p,bool write) noexcept {
#ifdef _WIN32
    std::FILE* file{};return _wfopen_s(&file,p.c_str(),write?L"wb":L"rb")==0?file:nullptr;
#else
    return std::fopen(p.c_str(),write?"wb":"rb");
#endif
}
inline bool durableFlush(std::FILE* f) noexcept {
    if(std::fflush(f)!=0)return false;
#ifdef _WIN32
    return _commit(_fileno(f))==0;
#else
    return ::fsync(::fileno(f))==0;
#endif
}
inline std::string_view trim(std::string_view s) noexcept {
    while(!s.empty()&&(s.front()==' '||s.front()=='\t'||s.front()=='\r'))s.remove_prefix(1);
    while(!s.empty()&&(s.back()==' '||s.back()=='\t'||s.back()=='\r'))s.remove_suffix(1);
    return s;
}
using SeenMask=std::uint64_t; // One bit per key (keyCount <= 64)
static_assert(sizeof(SeenMask)*8>=keyCount);
inline bool parse(std::string_view text,std::uint64_t& out) noexcept {
    std::uint64_t result=defaultPacked();SeenMask seen{};bool version{}; // 64-bit seen mask
    int legacyHammer=-1; // Legacy "hammer" key (0 OFF, 1 FAN, 2 FAN W/ TRIGGER, 3 SINGLE ACTION), at most once
    int legacyRideFwd=-1;
    while(!text.empty()){
        const auto eol=text.find('\n');
        const auto line=trim(text.substr(0,eol));
        text=eol==text.npos?std::string_view{}:text.substr(eol+1);
        if(line.empty()||line.front()=='#')continue;
        const auto eq=line.find('=');if(eq==line.npos)return false;
        const auto key=trim(line.substr(0,eq)),value=trim(line.substr(eq+1));
        unsigned number{};const auto converted=std::from_chars(value.data(),value.data()+value.size(),number);
        if(value.empty()||converted.ec!=std::errc{}||converted.ptr!=value.data()+value.size())return false;
        if(key=="version"){if(version||number!=1)return false;version=true;continue;}
        if(key=="hammer"){if(legacyHammer>=0||number>=4)return false;legacyHammer=number==3?1:0;continue;}
        // (a renamed key reset ALL settings): a removed / renamed key, a duplicate or an out-of-range value now skips only that line
        if(key=="rideFwdCm"){if(number<16)legacyRideFwd=int(number);continue;} // Migration: -30..+45 cm (5 cm) -> rideFwd5 -10..+25 cm
        unsigned k=0;while(k<keyCount&&key!=keyInfo[k].cfg)++k;
        if(k==keyCount||(seen&(SeenMask(1)<<k))||number>=keyInfo[k].count)continue;
        seen|=SeenMask(1)<<k;result=withValue(result,k,number);
    }
    if(!version)return false;
    if(legacyRideFwd>=0&&!(seen&(SeenMask(1)<<unsigned(Key::RideFwd)))){const int cm=(legacyRideFwd-6)*5;
        result=withValue(result,unsigned(Key::RideFwd),unsigned(std::clamp((cm+10)/5,0,7)));}
    if(legacyHammer>=0&&!(seen&(SeenMask(1)<<unsigned(Key::Hammer))))result=withValue(result,unsigned(Key::Hammer),unsigned(legacyHammer)); // hammerStyle wins over the legacy key
    out=result;return true;
}
} // namespace detail

// VR-only configuration, never the retail game's options. Same persistence contract as GraphicsSettings: missing file =
// first run; malformed = defaults + persistenceOk false; accepted edits apply in memory even when the save fails.
class VrSettingsStore {
public:
    static constexpr std::size_t maxFileBytes=4096;
    bool configurePath(std::string path) noexcept {
        detail::PreserveErrno preserve;
        try{
            std::lock_guard lock(mutex_);
            if(loaded_||path.empty()||path.find('\0')!=std::string::npos)return false;
            const auto file=detail::utf8Path(path);
            if(!file.is_absolute()||file.filename()!="vr.cfg")return false;
            path_=std::move(path);return true;
        }catch(...){return false;}
    }
    bool loadOnce() noexcept {
        detail::PreserveErrno preserve;std::lock_guard lock(mutex_);
        if(loaded_)return loadResult_;
        loaded_=true;loadResult_=false;std::uint64_t value=defaultPacked();
        try{
            if(!path_.empty()){
                errno=0;auto* file=detail::openFile(detail::utf8Path(path_),false);
                if(!file)loadResult_=errno==ENOENT;
                else{
                    std::array<char,maxFileBytes+1> data{};
                    const auto size=std::fread(data.data(),1,data.size(),file);
                    const bool good=!std::ferror(file)&&size<=maxFileBytes;
                    const bool closed=std::fclose(file)==0;
                    std::uint64_t parsed{};
                    loadResult_=good&&closed&&detail::parse(std::string_view(data.data(),size),parsed);
                    if(loadResult_)value=parsed;
                }
            }
        }catch(...){loadResult_=false;value=defaultPacked();}
        packed_.store(value,std::memory_order_release);
        persistenceOk_.store(loadResult_,std::memory_order_release);
        loadedFlag_.store(true,std::memory_order_release);
        return loadResult_;
    }
    Snapshot requested() const noexcept {return {packed_.load(std::memory_order_acquire)};}
    // Kill switches applied: what the modules must act on.
    Snapshot effective() const noexcept {
        Snapshot s=requested();
        for(unsigned k=0;k<keyCount;++k){
            if(k==unsigned(Key::Locomotion)){s.packed=withValue(s.packed,k,locomotionEffective(s[Key::Locomotion]));continue;}
            if(keyInfo[k].legacyValue>=0&&keyKilled(Key(k)))s.packed=withValue(s.packed,k,unsigned(keyInfo[k].legacyValue));
        }
        return s;
    }
    unsigned value(Key k) const noexcept {return requested()[k];}
    bool loaded() const noexcept {return loadedFlag_.load(std::memory_order_acquire);}
    bool persistenceOk() const noexcept {return persistenceOk_.load(std::memory_order_acquire);}
    std::uint32_t saves() const noexcept {return saves_.load(std::memory_order_relaxed);}
    std::uint32_t saveFailures() const noexcept {return saveFailures_.load(std::memory_order_relaxed);}
    // false: invalid value, use before loadOnce, or the save failed (the edit still applies; see persistenceOk).
    bool set(Key k,unsigned v) noexcept {
        detail::PreserveErrno preserve;std::lock_guard lock(mutex_);
        if(!loaded_||unsigned(k)>=keyCount||v>=info(k).count)return false;
        packed_.store(withValue(packed_.load(std::memory_order_relaxed),unsigned(k),v),std::memory_order_release);
        return saveLocked();
    }
    // Menu rule (GraphicsMenu::cycle): the stick (adjust +-1) clamps at both ends, A (adjust 0) wraps.
    bool adjust(Key k,int adjust) noexcept {
        const int n=int(info(k).count),cur=int(value(k));
        const int next=adjust?std::clamp(cur+adjust,0,n-1):(cur+1)%n;
        return next==cur||set(k,unsigned(next));
    }
    // WEAPON PRESET: every preset key in one store + one save (PresetCustom / before loadOnce: false, nothing changes).
    bool applyWeaponPreset(unsigned preset) noexcept {
        detail::PreserveErrno preserve;std::lock_guard lock(mutex_);
        if(!loaded_||preset>=PresetCustom)return false;
        packed_.store(withWeaponPreset(packed_.load(std::memory_order_relaxed),preset),std::memory_order_release);return saveLocked();
    }
    bool restoreDefaults() noexcept {
        detail::PreserveErrno preserve;std::lock_guard lock(mutex_);
        if(!loaded_)return false;
        packed_.store(defaultPacked(),std::memory_order_release);return saveLocked();
    }
    bool save() noexcept {detail::PreserveErrno preserve;std::lock_guard lock(mutex_);return saveLocked();}
    // The configured vr.cfg path ("" before configurePath) - weapons.cfg (VrWeaponCalib.h) lives next to it.
    std::string path() const {std::lock_guard lock(mutex_);return path_;}
    // Report fragment for vr.menu.settings (JSON object).
    std::string report() const {
        std::string out="{\"loaded\":";out+=loaded()?"true":"false";
        out+=",\"persistenceOk\":";out+=persistenceOk()?"true":"false";
        out+=",\"saves\":"+std::to_string(saves())+",\"saveFailures\":"+std::to_string(saveFailures());
        const auto r=requested(),e=effective();
        for(const auto* which:{"requested","effective"}){
            const auto& s=which[0]=='r'?r:e;out+=",\"";out+=which;out+="\":{";
            for(unsigned k=0;k<keyCount;++k){if(k)out+=',';out+='"';out+=keyInfo[k].cfg;out+="\":\"";out+=keyInfo[k].names[s[Key(k)]];out+='"';}
            out+='}';
        }
        out+=",\"killed\":[";bool first=true;
        for(unsigned f=0;f<featureCount;++f)if(killed(Feature(f))){if(!first)out+=',';first=false;out+='"';out+=featureNames[f];out+='"';}
        out+="]}";return out;
    }
private:
    mutable std::mutex mutex_;
    std::string path_;
    std::atomic<std::uint64_t> packed_{defaultPacked()};
    std::atomic<bool> persistenceOk_{true},loadedFlag_{false};
    std::atomic<std::uint32_t> saves_{0},saveFailures_{0};
    bool loaded_{},loadResult_{};
    bool saveLocked() noexcept {
        persistenceOk_.store(false,std::memory_order_release);
        if(!loaded_||path_.empty()){saveFailures_.fetch_add(1,std::memory_order_relaxed);return false;}
        std::filesystem::path temporary;std::FILE* file{};
        try{
            const auto target=detail::utf8Path(path_);
            temporary=target;temporary+=".tmp";
            file=detail::openFile(temporary,true);
            if(file){
                const auto s=requested();
                bool written=std::fprintf(file,"version=1\n")>0;
                for(unsigned k=0;k<keyCount;++k)written=written&&std::fprintf(file,"%s=%u\n",keyInfo[k].cfg,s[Key(k)])>0;
                written=written&&detail::durableFlush(file);
                const bool closed=std::fclose(file)==0;file=nullptr;
                std::error_code renamed;
                if(written&&closed){std::filesystem::rename(temporary,target,renamed);
                    if(!renamed){persistenceOk_.store(true,std::memory_order_release);saves_.fetch_add(1,std::memory_order_relaxed);return true;}}
            }
        }catch(...){}
        if(file)std::fclose(file);
        if(!temporary.empty()){std::error_code ignored;std::filesystem::remove(temporary,ignored);}
        saveFailures_.fetch_add(1,std::memory_order_relaxed);return false;
    }
};
inline VrSettingsStore vrSettings;
// What every module reads (any thread, lock-free): the menu state with the kill switches applied.
inline Snapshot settings() noexcept {return vrSettings.effective();}

} // namespace rdrvr::vr
