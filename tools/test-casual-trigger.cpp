// Host regression: run with a temporary directory for vr.cfg as argv[1].
#include "vr/VrControls.h"
#include "vr/VrHammer.h"
#include <cassert>
#include <filesystem>
#include <iostream>
namespace V=rdrvr::vr;
namespace H=V::holsters;
namespace R=rdrvr::runtime;
static int hammerCalls{},phaseCalls{},gateCalls{},phaseValue{};
static bool owns(int,std::uint64_t) noexcept {++hammerCalls;return true;}
static void step(const R::ControllerSample&,const H::HammerIn&,H::HammerOut& out) noexcept {
    ++hammerCalls;out={};out.edgeBlock="HAMMER_DOWN";
}
static int phase(std::uint64_t) noexcept {++phaseCalls;return phaseValue;}
static int gate(int,int,std::uint64_t) noexcept {++gateCalls;return 4;}
static constexpr H::HammerSinks sinks{nullptr,&owns,&step,nullptr,nullptr};
int main(int argc,char** argv) {
    assert(argc==2);
    assert(V::vrSettings.configurePath((std::filesystem::absolute(argv[1])/"vr.cfg").string()));
    assert(V::vrSettings.loadOnce());
    assert(V::vrSettings.applyWeaponPreset(V::PresetCasual));
    assert(V::hammer::modeOf(V::settings())==V::hammer::ModeOff);
    H::engineReady=true;V::mainthread::markInstalled();H::contextAllowed=true;
    H::setConfig({true,H::GripMode::Toggle,H::MarkerMode::Near,true,true});
    H::hammerSinks=&sinks;H::cylinderPhaseSink=&phase;H::rtGateSink=&gate;
    H::InputBrain brain;
    R::ControllerSample in{};in.sequence=1;in.epoch=in.continuity=in.referenceSpace=1;
    in.active=in.focused=true;
    std::uint64_t now=1'000'000'000ull;
    auto tick=[&](int hand,float trigger,bool ready=true){
        now+=28'000'000;in.publishedNs=now;++in.sequence;
        in.triggers={};in.triggers[unsigned(hand)]=trigger;
        R::PadState pad{};pad.ready=ready;
        brain.frame(&in,nullptr,true,now,pad);return pad;
    };
    // Sidearm/rifle/shotgun/sniper: hold through multiple polls, release, redraw
    // and transfer while squeezed. Simulate even a stale open-cylinder state.
    for(int slot:{H::SlotSidearm,H::SlotLongarm,H::SlotShotgun,H::SlotSniper}) {
        H::currentSlot=slot;H::heldKind=1;H::holdHand=1;++H::holdGeneration;phaseValue=2;
        for(int i=0;i<12;++i){auto p=tick(1,.8f);assert(p.triggers[1]==.8f&&(p.buttons&R::PadRightTrigger));}
        auto p=tick(1,0);assert(p.triggers[1]==0&&!(p.buttons&R::PadRightTrigger));
        ++H::holdGeneration;p=tick(1,1);assert(p.triggers[1]==1&&(p.buttons&R::PadRightTrigger));
        H::holdHand=0;++H::holdGeneration;
        p=tick(0,.9f);assert(p.triggers[1]==.9f&&(p.buttons&R::PadRightTrigger)&&p.triggers[0]==0);
        p=tick(0,1,false);assert(!p.ready&&p.triggers[1]==0);
    }
    assert(hammerCalls==0&&phaseCalls==0&&gateCalls==0);
    // Realistic keeps its manual gates, including after switching from Casual.
    assert(V::vrSettings.applyWeaponPreset(V::PresetRealistic));
    V::hammer::heldAction=V::hammer::ActionSingle;
    assert(V::hammer::modeOf(V::settings())==V::hammer::ModeSingle);
    tick(0,0);auto p=tick(0,1);assert(p.triggers[1]==0); // open cylinder
    assert(phaseCalls>0);
    phaseValue=0;tick(0,0);p=tick(0,1);assert(p.triggers[1]==0); // down hammer
    assert(hammerCalls>0);
    H::hammerSinks=nullptr;tick(0,0);p=tick(0,1);assert(p.triggers[1]==0&&gateCalls>0); // one-shot latch
    // A live switch to Casual clears the blocked press without requiring a restart.
    assert(V::vrSettings.applyWeaponPreset(V::PresetCasual));
    p=tick(0,.85f);assert(p.triggers[1]==.85f&&(p.buttons&R::PadRightTrigger));
    auto custom=V::settings();custom.packed=V::withValue(custom.packed,unsigned(V::Key::ReloadMode),0);
    assert(V::weaponPresetOf(custom)==V::PresetCustom&&V::hammer::modeOf(custom)==V::hammer::ModeFan);
    // Empty hands and physical melee do not acquire firearm RT from the preset.
    H::holdHand=-1;++H::holdGeneration;p=tick(0,1);assert(p.triggers[1]==0&&p.triggers[0]==0);
    H::holdHand=0;H::currentSlot=H::SlotMelee;H::heldKind=H::kindMelee;++H::holdGeneration;
    tick(0,0);p=tick(0,1);assert(p.triggers[1]==0);
    std::cout<<"Casual held-trigger, redraw, transfer, preset switch and Realistic gate tests passed\n";
}
