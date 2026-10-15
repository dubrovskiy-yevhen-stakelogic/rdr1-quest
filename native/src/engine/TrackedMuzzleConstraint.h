#pragma once
#include "ControllerAim.h"
#include "NativeAimProbe.h"
#include <algorithm>
namespace rdrvr::engine {
struct TrackedMuzzleConstraint {ControllerRay ray{};bool clamped{};float requestedDistance{},safeDistance{};};
// Keep the hand's orientation, but prevent its ray/model muzzle from starting
// beyond a native bullet stopper between the rendered viewpoint and the hand.
// Same principle as the working Quest Vice City eye-to-throwable LOS guard.
inline bool constrainTrackedMuzzle(const NativeAimProbe& physics,std::uint32_t actorGuid,
                                  const ControllerRay& requested,TrackedMuzzleConstraint& out)noexcept{
    const int saved=errno;struct Restore{int saved;~Restore(){errno=saved;}} restore{saved};
    const auto scale=requested.unitsPerMetre;
    if(!requested.sceneId||!requested.controllerSequence||!requested.trackingEpoch||!requested.referenceSpaceEpoch||
       !std::isfinite(scale)||scale<.01f||scale>100)return false;
    AimNativeVector start{},end{};double length2{};
    for(unsigned i=0;i<3;++i){
        start.v[i]=requested.viewOrigin[i];end.v[i]=requested.origin[i];
        if(!std::isfinite(start.v[i])||!std::isfinite(end.v[i]))return false;
        const double d=double(end.v[i])-start.v[i];length2+=d*d;
    }
    const double length=std::sqrt(length2);
    if(!std::isfinite(length)||length>2000)return false;
    TrackedMuzzleConstraint result{};result.ray=requested;
    result.requestedDistance=result.safeDistance=float(length);
    // A coincident source is already at the view; no segment exists to probe.
    if(length>=.0001){
        NativeAimHit hit{};if(!physics.castBetween(actorGuid,start,end,hit))return false;
        if(hit.blocked){
            result.safeDistance=std::max(0.f,hit.distance-.08f*scale);
            result.safeDistance=std::min(result.safeDistance,float(length));result.clamped=result.safeDistance<float(length);
            for(unsigned i=0;i<3;++i)result.ray.origin[i]=float(double(start.v[i])+(double(end.v[i])-start.v[i])*result.safeDistance/length);
        }
    }
    out=result;return true;
}
}
