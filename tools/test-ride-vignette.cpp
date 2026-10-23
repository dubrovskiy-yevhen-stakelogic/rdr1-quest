// Host regression: run with a temporary directory for vr.cfg as argv[1].
// HORSE VIGNETTE / MOUNT TURN: the second settings word (layout, vr.cfg round trip, old files), the mounted -> aperture fade and the dip.
#include "vr/VrSettings.h"
#include "vr/VrComfort.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
namespace V=rdrvr::vr;
namespace C=V::comfort;
static std::string read(const std::filesystem::path& p) {std::ifstream f(p,std::ios::binary);std::stringstream s;s<<f.rdbuf();return s.str();}
static bool near(float a,float b,float e=1e-4f) {return std::fabs(a-b)<=e;}
int main(int argc,char** argv) {
    assert(argc==2);
    const auto dir=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(dir);
    const auto cfg=dir/"vr.cfg";std::filesystem::remove(cfg);
    // Layout: every key before HORSE VIGNETTE keeps its word-0 position; HORSE VIGNETTE starts word 1.
    for(unsigned k=0;k<unsigned(V::Key::RideVignette);++k)assert(V::keyWord(k)==0);
    assert(V::keyWord(unsigned(V::Key::RideVignette))==1&&V::keyShift(unsigned(V::Key::RideVignette))==0);
    assert(V::Snapshot{}.rideVignette()==0&&V::info(V::Key::RideVignette).count==4);
    assert(V::keyWord(unsigned(V::Key::MountTurn))==1&&V::Snapshot{}.mountTurnSnap()); // MOUNT TURN: SNAP by default
    // Missing file = defaults; an edit of the new key leaves every other key alone and is saved by name.
    assert(V::vrSettings.configurePath(cfg.string())&&V::vrSettings.loadOnce());
    const auto before=V::vrSettings.requested();
    assert(V::vrSettings.set(V::Key::RideVignette,2));
    const auto after=V::vrSettings.requested();
    for(unsigned k=0;k<V::keyCount;++k)if(k!=unsigned(V::Key::RideVignette))assert(after[V::Key(k)]==before[V::Key(k)]);
    assert(after.rideVignette()==2&&after.packed.w[0]==before.packed.w[0]);
    assert(V::vrSettings.set(V::Key::TurnMode,1)&&V::vrSettings.set(V::Key::RideUp,15)); // the last keys of word 0
    assert(V::vrSettings.set(V::Key::MountTurn,1)&&!V::vrSettings.requested().mountTurnSnap());
    assert(V::vrSettings.requested().rideVignette()==2&&V::vrSettings.requested().smoothTurn());
    assert(read(cfg).find("rideVignette=2\n")!=std::string::npos&&read(cfg).find("mountTurn=1\n")!=std::string::npos);
    { V::VrSettingsStore again;assert(again.configurePath(cfg.string())&&again.loadOnce());
      const auto s=again.requested();assert(s.rideVignette()==2&&!s.mountTurnSnap()&&s[V::Key::RideUp]==15&&s.smoothTurn()&&s==V::vrSettings.requested()); }
    // A file written before HORSE VIGNETTE (no rideVignette line) keeps its values and gets OFF; an out-of-range value skips that line only.
    { std::ofstream(cfg,std::ios::binary)<<"version=1\nturnMode=1\nrideUpCm=3\nrideVignette=7\n";
      V::VrSettingsStore old;assert(old.configurePath(cfg.string())&&old.loadOnce());
      const auto s=old.requested();assert(s.rideVignette()==0&&s.mountTurnSnap()&&s.smoothTurn()&&s[V::Key::RideUp]==3); }
    // Weapon presets and RESET ALL still work across both words.
    assert(V::vrSettings.applyWeaponPreset(V::PresetCasual)&&V::weaponPresetOf(V::vrSettings.requested())==V::PresetCasual);
    assert(V::vrSettings.requested().rideVignette()==2);
    assert(V::vrSettings.restoreDefaults()&&V::vrSettings.requested()==V::Snapshot{});
    // Mounted = a riding sample within staleNs, whatever the horse does; the levels close further and further.
    constexpr std::uint64_t ms=1'000'000ull;
    assert(C::targetOf(0,1000*ms)==0.f&&C::targetOf(1000*ms,1000*ms)==1.f&&C::targetOf(1000*ms,1000*ms+C::staleNs)==1.f);
    assert(C::targetOf(1000*ms,1000*ms+C::staleNs+1)==0.f&&C::targetOf(1000*ms,999*ms)==0.f); // stale = not riding
    assert(C::clearTan(0,1.f)==0.f&&C::clearTan(4,1.f)==0.f);
    for(unsigned l=1;l<C::levelCount;++l){
        assert(near(C::clearTan(l,0.f),std::tan(C::openDeg*3.14159265f/180.f)));
        assert(near(C::clearTan(l,1.f),std::tan(C::clearDeg[l]*3.14159265f/180.f)));
        if(l>1)assert(C::clearTan(l,1.f)<C::clearTan(l-1,1.f));} // HIGH closes furthest
    std::uint64_t t=5000*ms;
    C::observeRide(false,t);C::observeRide(true,t+27'777'778ull);t+=27'777'778ull;
    assert(C::rideSamples.load()==2&&C::transitionSamples.load()==1&&C::rideNs.load()==t);
    // MOUNT TURN dip: none before a snap; black, then a fade in; 1 again afterwards.
    assert(C::dipAlpha(t)==1.f);
    C::noteMountSnap(t,-1.5708f);assert(C::mountSnaps.load()==1&&near(C::heldDeg.load(),90.f,.01f));
    assert(C::dipAlpha(t)==0.f&&C::dipAlpha(t+C::dipBlackNs-1)==0.f);
    { const float mid=C::dipAlpha(t+C::dipBlackNs+C::dipInNs/2);assert(mid>.4f&&mid<.6f); }
    assert(C::dipAlpha(t+C::dipBlackNs+C::dipInNs)==1.f&&C::dipAlpha(t-1)==1.f);
    // Fader: OFF is 0 at once; mounting closes quickly (standing still or not), the dismount opens slowly.
    C::Vignette v;assert(v.step(0,t)==0.f);
    for(int i=0;i<36;++i){t+=13'888'889ull;C::observeRide(false,t);v.step(3,t);}
    const float closed=v.step(3,t);assert(closed>.9f);
    for(int i=0;i<720;++i){t+=13'888'889ull;if(i%2==0)C::observeRide(false,t);assert(v.step(3,t)>=closed);} // 10 s in the saddle: stays on
    t+=C::staleNs+1;float opened=closed;for(int i=0;i<36;++i){t+=13'888'889ull;opened=v.step(3,t);}
    assert(opened<closed&&opened>.05f); // 0.5 s after the last sample: still opening
    for(int i=0;i<216;++i){t+=13'888'889ull;opened=v.step(3,t);}assert(opened==0.f);
    t+=13'888'889ull;assert(v.step(0,t)==0.f);
    std::cout<<"ride vignette host tests passed\n";
    return 0;
}
