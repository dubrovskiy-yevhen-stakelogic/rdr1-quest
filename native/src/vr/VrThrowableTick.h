#pragma once
// THROWABLE main-thread side (VrThrowable.h has the contract): ModuleThrowable tick = the AIM arc (ArcChannel::Throw, the shared tracer,
// the engine's gravity once a launch told it), a decided throw -> world flight (the held model flies it until the engine launches; a
// surface hit before that freezes it at the hit), expiry; adjustLaunch = the projProjectile::Launch substitution (GameXr, main thread).
#include "VrThrowable.h"
#include "VrArc.h"
#include <algorithm>

namespace rdrvr::vr::throwable {
namespace hs=::rdrvr::vr::hands;
// GameXr glue: the VR-held weapon of `hand` (engine item) at its last placement: start pose (model, LOCAL->world map, flying point,
// up, mover velocity, actor) and its weapon pointer. False = no usable placement.
using HeldStartFn=bool(*)(int hand,std::uint64_t now,T::StartPose& out,std::uintptr_t& weapon) noexcept;
inline std::atomic<HeldStartFn> heldStartFn{};

namespace detail {
struct Flight {
    bool flying{},stuck{},launchable{};std::uint64_t seq{},startNs{},requestNs{};std::uint32_t mode{};int hand{-1};
    std::uintptr_t weapon{};std::uint32_t actorGuid{};float upm{1};V3 up{0,1,0};
    T::Segment seg{};      // the centre's ballistic segment (world units), learned gravity
    V3 hitP{},hitV{};      // stuck: launch from here with this velocity
    V3 lastP{};std::uint64_t lastProbeNs{};
    T::Arc arc{};
};
struct Shown {std::uint64_t ns{};V3 origin{},velocity{};bool valid{};};
// AIM ASSIST target: where the holding controller's aim ray hit while the hand was still ("point, then throw"), world units.
struct Target {std::uint64_t ns{};V3 point{};int hand{-1};bool valid{};};
// DIRECTION-ONLY fallback: the knife has no VR placement to start a visual flight from, the engine throws it along its own camera aim: the decided throw's
// world velocity (aimed: the shown arc; physical: the swing heading, flat elevation, the game's speed) replaces only the launch velocity
struct DirOnly {std::uint64_t seq{},ns{};V3 v{},origin{};bool valid{},hasOrigin{};};
// TOSS visual: the held model flies the input thread's LOCAL toss (TossBrain) mapped to world every tick; d0 = the model's own grip
// point minus the mapped VR grip at the start (decays over 250 ms: the model starts exactly where it was and joins the catch track).
struct TossVisual {bool on{},stuck{};std::uint64_t seq{},startNs{},lastNs{};std::uintptr_t weapon{};std::uint32_t actorGuid{};int hand{-1};
    M3 r{};V3 centre{},d0{},lastP{};float scale{1};bool lastValid{};T::Arc arc{};};
struct State {
    std::uint64_t seenSeq{};Flight f{};Shown shown{};Target target{};DirOnly dirOnly{};TossVisual toss{};
    arc::Tracer tracer{};arc::DirectionFilter filter{};bool arcShown{},armed{};
    V3 lastGrip{};int lastGripHand{-1};std::uint64_t lastGripNs{},ticks{};
};
inline State& state() noexcept {static State s;return s;}
inline V3 centreAt(const T::Segment& s,std::uint64_t now) noexcept {
    const float dt=now>s.t0?std::fmin(float(double(now-s.t0)*1e-9),3.f):0.f;
    return T::math::add(T::math::add(s.p,T::math::mul(s.v,dt)),T::math::mul(s.g,.5f*dt*dt));}
inline V3 velocityAt(const T::Segment& s,std::uint64_t now) noexcept {
    const float dt=now>s.t0?std::fmin(float(double(now-s.t0)*1e-9),3.f):0.f;return T::math::add(s.v,T::math::mul(s.g,dt));}
inline void publishFlight(Flight& f) noexcept {f.arc.seg[0]=f.seg;f.arc.switchNs=0;f.arc.state=f.stuck?FlightStuck:FlightFlying;
    if(config.visual)arcBox.store(f.arc);}
inline void endFlight(Flight& f) noexcept {
    if(!f.flying)return;
    flightWeapon.store(0,std::memory_order_release);T::Arc a{};a.state=FlightIdle;arcBox.store(a);f=Flight{};
}
inline void endToss(TossVisual& t) noexcept {
    if(!t.on)return;
    if(flightWeapon.load(std::memory_order_relaxed)==t.weapon){flightWeapon.store(0,std::memory_order_release);T::Arc a{};a.state=FlightIdle;arcBox.store(a);}
    tossWeapon.store(0,std::memory_order_release);const auto seq=t.seq;t=TossVisual{};t.seq=seq;
}
inline bool tossMap(std::uint64_t now,T::M4& map,float& upm) noexcept {
    const auto fn=T::mapFn.load(std::memory_order_acquire);if(!fn)return false;T::Map m{};bool ok=false;try{ok=fn(now,m);}catch(...){ok=false;}
    if(!ok||!T::math::finite(m.localToWorld))return false;
    map=m.localToWorld;upm=std::sqrt(map[0]*map[0]+map[1]*map[1]+map[2]*map[2]);return upm>1e-4f&&std::isfinite(upm);
}
// One toss tick: start (new seq), the world pose of this tick (probe: a surface ends it as missed), the end (caught = back in a hand,
// missed = keeps falling until the put-away landed, <= 1 s).
inline void tossTick(State& s,std::uint64_t now) noexcept {
    using namespace T::math;auto& tv=s.toss;
    const auto q=tossSeq.load(std::memory_order_acquire);Toss t{};
    if(!q){endToss(tv);return;}
    // a busy seqlock (the input thread preempted inside its store) or the box one publish ahead of the seq is NOT the end
    // of the toss - ending here kept tv.seq, so that toss never got its visual back (the item showed in the open hand while it 'flew')
    if(!tossBox.load(t)||t.seq!=q)return;
    if(t.seq!=tv.seq){
        endToss(tv);tv.seq=t.seq;
        if(t.state!=TossFlying)return;
        if(s.f.flying||!config.visual){tossFailSeq.store(t.seq,std::memory_order_release);return;} // never two flights on the one arc box
        const auto fn=tossStartFn.load(std::memory_order_acquire);TossStart st{};bool ok=false;
        if(fn){try{ok=fn(t.hand,now,st);}catch(...){ok=false;}}
        T::M4 map{};float upm=1.f;
        if(!ok||!st.weapon||!finite(st.model)||!finite(st.k)||!tossMap(now,map,upm)){tossFailSeq.store(t.seq,std::memory_order_release);return;}
        tv.on=true;tv.weapon=st.weapon;tv.actorGuid=st.actorGuid;tv.hand=t.hand;tv.startNs=now;tv.lastNs=now;
        tv.r=rotationOf(st.model);tv.scale=scaleOf(st.model);tv.centre={st.k[12],st.k[13],st.k[14]}; // the hand bone (grip) in model space
        const V3 own=xform(st.model,tv.centre),mapped=xform(map,t.p);tv.d0=sub(own,mapped);
        if(!finite(tv.d0)||length(tv.d0)>.5f*upm)tv.d0={};
        tv.lastP=own;tv.lastValid=true;
        tv.arc=T::Arc{};tv.arc.generation=t.seq;tv.arc.weapon=st.weapon;tv.arc.centre=tv.centre;tv.arc.scale=tv.scale;tv.arc.animated=0u;
        PoseK pk{};pk.weapon=st.weapon;pk.ns=now;pk.k=st.k;poseKBox.store(pk);tossWeapon.store(st.weapon,std::memory_order_release);
        bump(stats.tossVisuals);
    }
    if(!tv.on)return;
    const int hold=heldHand.load(std::memory_order_acquire); // the VR hand holding the throwable (holsters route; -1 = put away)
    if(t.state==TossCaughtSame||t.state==TossCancelled){endToss(tv);return;}
    if(t.state==TossCaughtOther&&(hold!=t.hand||now<t.endNs||now-t.endNs>600'000'000ull)){endToss(tv);return;} // the other hand has it
    if(t.state==TossMissed&&(hold!=t.hand||now<t.endNs||now-t.endNs>1'000'000'000ull)){endToss(tv);return;}    // put away
    if(now>t.startNs&&now-t.startNs>config.tossMaxNs+1'500'000'000ull){endToss(tv);return;}                      // safety net
    T::M4 map{};float upm=1.f;if(!tossMap(now,map,upm))return; // keep the last published pose
    const M3 ms{map[0],map[1],map[2],map[4],map[5],map[6],map[8],map[9],map[10]};M3 rmap=ms;for(float& x:rmap)x/=upm;orthonormalize(rmap);
    const float dtTick=now>tv.lastNs?std::fmin(float(double(now-tv.lastNs)*1e-9),.1f):0.f;tv.lastNs=now;
    const float age=now>tv.startNs?float(double(now-tv.startNs)*1e-9):0.f;const float keep=std::fmax(0.f,1.f-age/.25f);
    const float dt=now>t.t0?std::fmin(float(double(now-t.t0)*1e-9),3.f):0.f;
    const V3 pl{t.p[0]+t.v[0]*dt,t.p[1]+t.v[1]*dt+.5f*t.g*dt*dt,t.p[2]+t.v[2]*dt},vl{t.v[0],t.v[1]+t.g*dt,t.v[2]};
    T::Segment g{};g.t0=now;
    if(tv.stuck){g.p=tv.lastP;}
    else{
        g.p=add(xform(map,pl),mul(tv.d0,keep));g.v=mulv(ms,vl);g.g=mulv(ms,{0.f,t.g,0.f});g.w=mulv(rmap,t.w);
        tv.r=multiply(rotation(mul(g.w,dtTick)),tv.r);orthonormalize(tv.r);
        if(t.state==TossFlying&&tv.lastValid&&length(sub(g.p,tv.lastP))>1e-4f*upm){ // a surface between the last and this pose: missed
            if(const auto pf=T::probeFn.load(std::memory_order_acquire)){bool blocked=false;T::Hit h{};bool cast=false;
                try{cast=pf(tv.actorGuid,tv.lastP,g.p,blocked,h);}catch(...){cast=false;}
                if(cast&&blocked&&finite(h.point)){const V3 d=normalized(sub(g.p,tv.lastP));g.p=sub(h.point,mul(d,.03f*upm));tv.stuck=true;
                    g.v={};g.g={};g.w={};tossHitSeq.store(t.seq,std::memory_order_release);}}
        }
        if(t.state!=TossFlying&&t.state!=TossMissed){g.v={};g.g={};g.w={};}
    }
    g.r=tv.r;tv.lastP=g.p;tv.lastValid=true;
    tv.arc.seg[0]=g;tv.arc.switchNs=0;tv.arc.state=FlightFlying;arcBox.store(tv.arc);flightWeapon.store(tv.weapon,std::memory_order_release);
}
// Warm yellow ribbon; a ring where it lands (a cross when nothing is hit in range).
inline arc::Look throwLook(bool hit) noexcept {
    arc::Look l{};const float r=1.f,g=hit?.78f:.40f,b=hit?.30f:.15f;
    l.rgbaStart=l.rgbaEnd=hs::packRgba(r,g,b,.85f);l.halfWidthStartM=.006f;l.halfWidthEndM=.009f;l.groundTolM=.30f;
    l.style.dashPeriodM=0;l.style.xrayAlpha=.30f;l.style.fadeInM=.10f;l.style.tube=1;l.style.core=.35f;l.style.flags=0;
    l.marker.shape=hit?hs::ArcMarkerShape::Ring:hs::ArcMarkerShape::Cross;l.marker.rgba=hs::packRgba(r,g,b,.90f);l.marker.radiusM=.35f;
    l.marker.liftM=.02f;l.marker.depthTolM=.30f;l.marker.xrayAlpha=.45f;
    return l;
}
inline void hideArc(State& s) noexcept {
    if(s.arcShown){hs::clearArc(hs::ArcChannel::Throw);s.arcShown=false;bump(stats.arcHidden);}
    s.tracer.reset();s.filter.reset();
}
// Launch direction of speed `speed` from o that lands on t under gravity g along -up (the flat solution; out of reach = 45 deg towards t).
// False = t too close horizontally.
inline bool solveDirection(V3 o,V3 t,float speed,float g,V3 up,V3& dir,bool& reachable) noexcept {
    using namespace T::math;
    const V3 d=sub(t,o);const float h=dot(d,up);const V3 hz=sub(d,mul(up,h));const float x=length(hz);
    if(!(x>.3f)||!(speed>.1f)||!(g>0.f))return false;
    const V3 hu=mul(hz,1.f/x);const float v2=speed*speed,disc=v2*v2-g*(g*x*x+2.f*h*v2);
    reachable=disc>=0.f;const float th=reachable?std::atan((v2-std::sqrt(disc))/(g*x)):.7853982f;
    dir=normalized(add(mul(hu,std::cos(th)),mul(up,std::sin(th))));return finite(dir)&&length(dir)>.5f;
}
// AIM ASSIST: dir0 bent towards the direction that lands on the target (k = 0..1 of the THROWING page).
inline V3 assisted(V3 dir0,V3 o,V3 t,float speed,float g,V3 up,float k) noexcept {
    using namespace T::math;
    V3 d{};bool reachable=false;if(!(k>0.f)||!solveDirection(o,t,speed,g,up,d,reachable))return dir0;
    if(!reachable)bump(stats.assistUnreachable);
    bump(stats.assistApplied);const V3 r=normalized(add(mul(dir0,1.f-k),mul(d,k)));return length(r)>.5f?r:dir0;
}
// The aim ray of `hand` cast up to 40 m (used by the THROWABLE probe): its first surface / actor.
inline bool rayTarget(const arc::Context& c,unsigned hand,V3& point) noexcept {
    V3 o{},dir{};if(!c.aimRay(hand,o,dir))return false;
    arc::ProbeQuery q{};q.from=o;q.to=T::math::add(o,T::math::mul(dir,40.f*c.upm()));q.include=0x500208;q.select=arc::Select::BulletStopper;
    arc::Hit h{};bump(stats.assistProbes);
    if(arc::probe(c,arc::ProbeUser::Throwable,q,h)!=arc::ProbeResult::Hit)return false;
    point=h.point;return T::math::finite(point);
}
// While a throwable is held (not aiming) and the hand is still (< 0.6 m/s): the assist target, every other tick.
inline void sampleTarget(State& s,std::uint64_t now) noexcept {
    const int hand=heldHand.load(std::memory_order_relaxed);
    if(hand<0||hand>1||!(assist()>0.f)){s.lastGripHand=-1;return;}
    auto& c=arc::frameContext(now);const auto* raw=c.raw();
    if(!raw||!c.world()||!validPose(raw->grip[unsigned(hand)])){s.lastGripHand=-1;return;}
    const auto& g=raw->grip[unsigned(hand)];const V3 p{g.position[0],g.position[1],g.position[2]};
    float speed=1e9f;
    if(s.lastGripHand==hand&&now>s.lastGripNs){const float dt=float(double(now-s.lastGripNs)*1e-9);if(dt>1e-3f)speed=T::math::length(T::math::sub(p,s.lastGrip))/dt;}
    s.lastGrip=p;s.lastGripHand=hand;s.lastGripNs=now;
    if(speed>.6f||(s.ticks&1u))return;
    V3 t{};if(rayTarget(c,unsigned(hand),t)){s.target={now,t,hand,true};bump(stats.assistTargets);}
}
// AIM: the arc from the aiming hand's grip along its aim ray at the AIMED SPEED (world), the engine's gravity (x THROW GRAVITY), bent by
// AIM ASSIST towards the ray's own hit; the launch it encodes is kept (Shown) - an aimed throw flies exactly that launch. SHOW ARC OFF:
// the same launch, nothing drawn.
inline void aimArc(State& s,std::uint64_t now) noexcept {
    const int hand=aimHand.load(std::memory_order_relaxed);
    if(hand<0||hand>1||::rdrvr::vr::killed(::rdrvr::vr::Feature::Arc)||::rdrvr::vr::killed(::rdrvr::vr::Feature::Hands)){hideArc(s);if(s.armed&&hand<0){hs::armArc(hs::ArcChannel::Throw,false);s.armed=false;}return;}
    if(!s.armed){hs::armArc(hs::ArcChannel::Throw,true);s.armed=true;}
    const auto st=hs::arcStatus();
    if(st==hs::ArcStatus::Failed||st==hs::ArcStatus::Killed||st==hs::ArcStatus::Suppressed){hideArc(s);return;}
    auto& c=arc::frameContext(now);
    if(!c.inputValid()||!c.world()){hideArc(s);return;}
    V3 o{},dir{};arc::MapInputs m{};
    if(!c.aimRay(unsigned(hand),o,dir)||!c.map(m)){hideArc(s);return;}
    V3 start{};if(c.startPoint(unsigned(hand),start)==arc::StartSource::None)start=o;
    if(arc::filterEnabled.load(std::memory_order_relaxed))dir=s.filter.filter(dir,now);
    arc::Launch L{};L.origin=T::math::add(start,T::math::mul(dir,.05f*m.upm));
    L.gravityMps2=gravityMps2();L.maxTimeS=3.f;L.maxRangeM=60.f;L.maxDropM=30.f;
    const float k=assist();V3 target{};
    if(k>0.f&&rayTarget(c,unsigned(hand),target)){bump(stats.assistTargets);dir=assisted(dir,L.origin,target,nativeLaunchSpeed(heldKindNow.load(std::memory_order_relaxed),m.upm),L.gravityMps2*m.upm,c.up(),k);}
    L.velocity=T::math::mul(dir,nativeLaunchSpeed(heldKindNow.load(std::memory_order_relaxed),m.upm)); // the game's throw speed
    s.shown={now,L.origin,L.velocity,true};
    if(!showArc()){hideArc(s);return;}
    arc::TraceConfig cfg{};cfg.chordDtS=.10f;cfg.maxChords=16;cfg.pointsPerChord=4;cfg.losFromEye=false;
    cfg.query.include=0x500208;cfg.query.select=arc::Select::BulletStopper;cfg.user=arc::ProbeUser::Throwable;
    const arc::Trace t=s.tracer.run(c,L,cfg);
    hs::ArcFrame fr{};arc::toFrame(c,s.tracer,t,throwLook(t.hit),t.hit,fr);
    hs::publishArc(hs::ArcChannel::Throw,fr);s.arcShown=true;bump(stats.arcPublished);
}
// A decided throw -> the world flight of the held model's flying point.
inline void beginFlight(State& s,const Request& r,std::uint64_t now) noexcept {
    using namespace T::math;
    endFlight(s.f);bump(stats.requestsSeen);
    const auto fn=heldStartFn.load(std::memory_order_acquire);T::StartPose p{};std::uintptr_t w{};bool ok=false;
    if(fn){try{ok=fn(r.hand,now,p,w);}catch(...){ok=false;}}
    // the start pose of a natively held item (the tomahawk / knives on the BODY hand) is the LAST VR placement, seconds old -
    // older than 150 ms = no visual flight, the direction-only launch from the VR hand
    const bool stale=ok&&(!p.ns||p.ns>now||now-p.ns>150'000'000ull);if(stale)bump(stats.startStale);
    if(!ok||stale||!w||!finite(p.model)||!finite(p.map)||!(p.upm>0.f)){bump(stats.startFailures);
        s.dirOnly={};
        if(r.mode==ModeAimed){const auto& sh=s.shown;if(sh.valid&&now>=sh.ns&&now-sh.ns<=200'000'000ull&&finite(sh.velocity)){
            s.dirOnly={r.seq,now,sh.velocity,sh.origin,true,finite(sh.origin)};}} // from the arc's own origin (the knife flew right of the arc)
        else if(r.mode==ModePhysical){auto& c=arc::frameContext(now);arc::MapInputs m{};
            if(c.map(m)&&m.upm>0.f){const auto& L=m.localToWorld;const M3 ms{L[0],L[1],L[2],L[4],L[5],L[6],L[8],L[9],L[10]};
                V3 up=c.up();if(length(up)<.5f)up={0,1,0};up=normalized(up);
                const float hs=length(r.v);V3 vh=mulv(ms,r.v);const V3 vH=sub(vh,mul(up,dot(vh,up)));const float hz=length(vH);
                if(hz>1e-4f&&hs>1e-3f){const float pitch=std::atan2(dot(vh,up),hz);const float el=std::clamp(.35f*pitch,-.1745f,.4363f);
                    const V3 d=add(mul(normalized(vH),std::cos(el)),mul(up,std::sin(el)));
                    const V3 v=mul(d,nativeLaunchSpeed(heldKindNow.load(std::memory_order_relaxed),m.upm)*std::clamp(hs/fullSwingMps,.35f,1.2f));
                    V3 o{};const bool ho=r.hand>=0&&r.hand<2&&c.startPoint(unsigned(r.hand),o)!=arc::StartSource::None&&finite(o);
                    if(finite(v))s.dirOnly={r.seq,now,v,o,true,ho};}}}
        if(s.dirOnly.valid)bump(stats.dirOnlyArmed);
        return;}
    Flight f{};f.seq=r.seq;f.mode=r.mode;f.hand=r.hand;f.weapon=w;f.actorGuid=p.actorGuid;f.upm=p.upm;f.up=normalized(p.up);
    if(length(f.up)<.5f)f.up={0,1,0};
    f.startNs=now;f.requestNs=r.ns?r.ns:now;
    const M3 ms{p.map[0],p.map[1],p.map[2],p.map[4],p.map[5],p.map[6],p.map[8],p.map[9],p.map[10]}; // LOCAL metres -> world (scaled)
    M3 rmap=ms;for(float& x:rmap)x/=p.upm;orthonormalize(rmap);
    V3 mover=p.moverVelocity;if(!finite(mover)||length(mover)>20.f*p.upm)mover={};
    T::Segment g{};g.r=rotationOf(p.model);g.p=xform(p.model,p.centre);g.g=mul(f.up,-gravityMps2()*p.upm);
    if(r.mode==ModePhysical){
        // direction of the release, speed = the game's throw speed x swing strength (hand speed / fullSwingMps)
        const float hs=length(r.v);V3 vh=mulv(ms,r.v);
        // the raw release pitch hits the ground: the swing's horizontal heading, its elevation compressed to a flat throw (.35 x release pitch + 5 deg, -10..+30 deg)
        if(length(vh)>1e-4f){const V3 vH=sub(vh,mul(f.up,dot(vh,f.up)));const float hz=length(vH);
            if(hz>1e-4f){const float pitch=std::atan2(dot(vh,f.up),hz);const float el=std::clamp(.35f*pitch,-.1745f,.4363f); // no bias (flew high)
                vh=add(mul(normalized(vH),std::cos(el)),mul(f.up,std::sin(el)));}}
        if(length(vh)>1e-4f&&hs>1e-3f)vh=mul(normalized(vh),nativeLaunchSpeed(heldKindNow.load(std::memory_order_relaxed),p.upm)*std::clamp(hs/fullSwingMps,.35f,1.2f));
        // AIM ASSIST: the target pointed at before the swing (<= 2 s, this hand), when the throw heads within ~35 deg of it (horizontally)
        const float k=assist();const auto& tg=s.target;
        if(k>0.f&&tg.valid&&tg.hand==r.hand&&now>=tg.ns&&now-tg.ns<2'000'000'000ull){
            const V3 to=sub(tg.point,g.p);const V3 toH=sub(to,mul(f.up,dot(to,f.up))),vH=sub(vh,mul(f.up,dot(vh,f.up)));
            if(length(toH)>.3f*p.upm&&length(vH)>.1f*p.upm&&dot(normalized(toH),normalized(vH))>.82f)
                vh=mul(assisted(normalized(vh),g.p,tg.point,length(vh),gravityMps2()*p.upm,f.up,k),length(vh));
        }
        g.v=add(vh,mover);g.w=mulv(rmap,mul(r.w,spinGain()));g.t0=p.ns&&p.ns<=now?p.ns:now;
    }else{
        const auto& sh=s.shown;
        if(!sh.valid||now<sh.ns||now-sh.ns>200'000'000ull){bump(stats.noShown);return;} // no shown arc: the engine throws natively
        g.p=sh.origin;g.v=sh.velocity;g.t0=now;
        const V3 side=normalized(cross(normalized(sh.velocity),f.up)); // end over end about the throw's side axis
        g.w=mul(side,-10.f*spinGain());
    }
    if(!finite(g.v)||!finite(g.p)){bump(stats.startFailures);return;}
    f.seg=g;f.lastP=g.p;f.lastProbeNs=now;f.flying=true;f.launchable=true;
    f.arc.generation=r.seq;f.arc.weapon=w;f.arc.centre=p.centre;f.arc.scale=scaleOf(p.model);f.arc.animated=p.animated?1u:0u;
    s.f=f;publishFlight(s.f);if(config.visual)flightWeapon.store(w,std::memory_order_release);
    lastSpeed.store(length(g.v)/p.upm,std::memory_order_relaxed);bump(stats.flights);
}
// Probe the visual flight (player excluded); a surface before the engine launch freezes the model there.
inline void flyTick(Flight& f,std::uint64_t now) noexcept {
    using namespace T::math;
    if(!f.flying||f.stuck)return;
    const V3 b=centreAt(f.seg,now+16'000'000ull);const V3 a=f.lastP;
    f.lastP=centreAt(f.seg,now);f.lastProbeNs=now;
    if(length(sub(b,a))<1e-4f*f.upm)return;
    const auto pf=T::probeFn.load(std::memory_order_acquire);if(!pf)return;
    bool blocked=false;T::Hit h{};bool cast=false;try{cast=pf(f.actorGuid,a,b,blocked,h);}catch(...){cast=false;}
    if(!cast||!blocked)return;
    bump(stats.flightProbeHits);
    const V3 v=velocityAt(f.seg,now);const V3 dir=normalized(v);
    f.stuck=true;f.hitV=v;f.hitP=sub(h.point,mul(dir,.10f*f.upm)); // 10 cm before the surface: the engine projectile meets it at once
    T::Segment s=f.seg;s.t0=now;s.p=sub(h.point,mul(dir,.03f*f.upm));s.v={};s.g={};s.w={};
    s.r=multiply(rotation(mul(f.seg.w,now>f.seg.t0?float(double(now-f.seg.t0)*1e-9):0.f)),f.seg.r);orthonormalize(s.r);
    f.seg=s;publishFlight(f);
}
} // namespace detail
inline void tick(const mainthread::Frame& fr) noexcept {
    try{
        bump(stats.ticks);using namespace detail;auto& s=state();const auto now=fr.now;++s.ticks;
        if(killed.load(std::memory_order_relaxed)||::rdrvr::vr::killed(::rdrvr::vr::Feature::Throwable))status::publish(Feature::Throwable,status::Property);
        else status::publish(Feature::Throwable,status::Ready,!active()?nullptr:s.f.flying?"IN FLIGHT":aimHand.load(std::memory_order_relaxed)>=0?"AIMING":nullptr);
        if(const auto gs=gravitySyncFn.load(std::memory_order_acquire))gs(active(),gravityFactor()); // THROW GRAVITY restore
        if(!active()){hideArc(s);endFlight(s.f);endToss(s.toss);return;}
        tossTick(s,now); // TOSS
        aimArc(s,now);
        if(aimHand.load(std::memory_order_relaxed)<0)sampleTarget(s,now);
        const auto seq=requestSeq.load(std::memory_order_acquire);
        if(seq&&seq!=s.seenSeq){Request r{};if(requestBox.load(r)&&r.seq==seq){s.seenSeq=seq;endToss(s.toss);beginFlight(s,r,now);}}
        auto& f=s.f;
        if(f.flying){
            if(launchedSeq.load(std::memory_order_acquire)>=f.seq){endFlight(f);return;}
            if(now>f.startNs&&now-f.startNs>maxDelayNs()){expiredSeq.store(f.seq,std::memory_order_relaxed);bump(stats.expired);endFlight(f);return;}
            flyTick(f,now);
        }
    }catch(...){}
}
inline bool install() noexcept {return mainthread::registerModule(mainthread::ModuleThrowable,"throwable",&tick,nullptr);}

// projProjectile::Launch of the local player's THROWN / THROWN_EXPLODING weapon (GameXr, main thread, inside FireProjectile).
// world / velocity / muzzle: the native arguments in, the substitution out (true = substitute). gravityScale = the ORIGINAL projectile type
// +0x28 (<= 0 unknown; GameXr has written original x THROW GRAVITY into the type). kind = engine::WeaponClass label.
struct LaunchIo {M4 world{};V3 velocity{},muzzle{};float gravityScale{};int kind{};};
inline bool adjustLaunch(LaunchIo& io,std::uint64_t now) noexcept {
    using namespace T::math;
    try{
        bump(stats.launchCalls);
        if(io.gravityScale>0.f&&io.gravityScale<10.f)learnedGravityScale.store(io.gravityScale,std::memory_order_relaxed);
        lastNativeSpeed.store(length(io.velocity),std::memory_order_relaxed);
        {const float ns=length(io.velocity); // the game's own throw speed of this kind
         if(std::isfinite(ns)&&ns>1.f&&(io.kind==kindThrown||io.kind==kindExploding))nativeSpeed[io.kind==kindExploding?1u:0u].store(ns,std::memory_order_relaxed);}
        if(throwableKind(io.kind)){auto& K=perKind[kindIndex(io.kind)];bump(K.launches); // per kind, with the fuse flag at the launch
            EngineAim ea{};if(engineAim(ea,now)&&aimLit(ea))bump(K.litLaunches);}
        using namespace detail;auto& f=state().f;
        // a NATIVE launch (no flight: noShown / a start failure / .override 0, or a stale / declined one) of the local
        // player's throw while a request waits is that request's launch - the holster / reins wait ends here, not after maxDelay + settle
        const auto launchedNative=[&]{const auto rq=requestSeq.load(std::memory_order_acquire);
            if(rq>launchedSeq.load(std::memory_order_relaxed)){Request r{};
                if(requestBox.load(r)&&r.seq==rq&&now>=r.ns&&now-r.ns<=maxDelayNs()+config.settleNs){
                    launchedNs.store(now,std::memory_order_relaxed);launchedSeq.store(rq,std::memory_order_release);}}};
        if(!active()){bump(stats.launchesNative);return false;}
        if(!config.launchOverride){bump(stats.launchesNative);launchedNative();return false;}
        if(!f.flying||!f.launchable){
            auto& d=state().dirOnly; // no visual flight - only the direction of the decided throw
            if(d.valid&&d.seq>launchedSeq.load(std::memory_order_relaxed)&&now>=d.ns&&now-d.ns<=maxDelayNs()){
                const V3 v=d.v,o=d.origin;const bool ho=d.hasOrigin;d={};
                if(finite(v)&&length(v)>.1f){
                    if(ho){io.world[12]=o[0];io.world[13]=o[1];io.world[14]=o[2];io.muzzle=o;} // from the VR hand / arc origin
                    M3 rr{io.world[0],io.world[1],io.world[2],io.world[4],io.world[5],io.world[6],io.world[8],io.world[9],io.world[10]};
                    const V3 nd=normalized(io.velocity),vd=normalized(v);
                    if(length(nd)>.5f&&length(vd)>.5f&&dot(nd,vd)>-.999f){const M3 q=shortestArc(nd,vd);rr=multiply(q,rr);}
                    for(unsigned c=0;c<3;++c)for(unsigned i=0;i<3;++i)io.world[c*4+i]=rr[c*3+i];
                    io.velocity=v;bump(stats.launchesDirOnly);launchedNative();return true;}}
            bump(stats.launchesNative);launchedNative();return false;}
        if(now<f.startNs||now-f.startNs>maxDelayNs()){bump(stats.launchesStale);endFlight(f);launchedNative();return false;}
        // position / velocity of the flight at this launch, with the engine's own gravity for this projectile type
        V3 p{},v{};
        if(f.stuck){p=f.hitP;v=f.hitV;}
        else{T::Segment s=f.seg;const float g=(io.gravityScale>0.f?9.8f*io.gravityScale*gravityFactor():gravityMps2())*f.upm;s.g=mul(f.up,-g);
            p=centreAt(s,now);v=velocityAt(s,now);}
        if(!finite(p)||!finite(v)||length(v)<.1f*f.upm){bump(stats.launchDeclined);endFlight(f);launchedNative();return false;}
        // keep the native orientation relative to the flight direction (the engine builds it from its own launch direction)
        M3 r{io.world[0],io.world[1],io.world[2],io.world[4],io.world[5],io.world[6],io.world[8],io.world[9],io.world[10]};
        const V3 nd=normalized(io.velocity),vd=normalized(v);
        if(length(nd)>.5f&&length(vd)>.5f&&dot(nd,vd)>-.999f){const M3 q=shortestArc(nd,vd);r=multiply(q,r);}
        LastLaunch L{};L.ns=now;L.seq=f.seq;L.nativeV=io.velocity;L.v=v;L.nativeP={io.world[12],io.world[13],io.world[14]};L.p=p;
        L.gravityScale=io.gravityScale;L.mode=f.mode;L.kind=std::uint32_t(io.kind);
        const float delayMs=now>f.requestNs?float(double(now-f.requestNs)/1e6):0.f;L.delayMs=delayMs;lastLaunchBox.store(L);
        for(unsigned c=0;c<3;++c)for(unsigned i=0;i<3;++i)io.world[c*4+i]=r[c*3+i];
        io.world[12]=p[0];io.world[13]=p[1];io.world[14]=p[2];io.velocity=v;io.muzzle=p;
        bump(stats.launchesOverridden);stats.launchDelayMsSum.fetch_add(std::uint64_t(delayMs),std::memory_order_relaxed);
        maxOf(stats.launchDelayMsMax,std::uint64_t(delayMs));
        launchedNs.store(now,std::memory_order_relaxed);launchedSeq.store(f.seq,std::memory_order_release);
        endFlight(f);return true;
    }catch(...){bump(stats.launchDeclined);return false;}
}
} // namespace rdrvr::vr::throwable
