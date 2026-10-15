#include "SceneCameraBridge.h"
#include <limits>
#include <sstream>

namespace rdrvr::engine {
bool SceneCameraBridge::observe(std::uintptr_t viewport,std::uint64_t timeNs,std::uint64_t thread,
                                Reader read,void* context,StereoCameraPlan* prepared) {
    // Never block a second engine render thread behind another scene's observation.
    std::unique_lock guard(mutex_,std::try_to_lock);
    if(!guard.owns_lock()){++busy_;return false;}
    ++scenes_;
    if(!firstThread_)firstThread_=thread;
    else if(thread!=firstThread_)++otherThreadScenes_;
    std::array<std::byte,cameraReadBytes> fields{};
    StockCamera camera{};
    if(!read || !viewport || viewport>std::numeric_limits<std::uintptr_t>::max()-cameraReadOffset-cameraReadBytes ||
       !read(context,viewport+cameraReadOffset,fields.data(),fields.size()) || !decodeCamera(fields,camera)) {
        ++badCamera_;adapter_.invalidate();return false;
    }
    ++decoded_;stock_=camera;
    runtime::TrackedViews views{};
    if(!views_.snapshot(timeNs,views)){++missingViews_;adapter_.invalidate();return false;}
    StereoCameraPlan plan{};
    // Provisional one unit/metre is recorded, never applied to the retail viewport.
    // Physical scale and the replay boundary must be validated before activating writes.
    if(adapter_.prepare(camera,views,scenes_,1.0f,plan)!=CameraResult::Ready){++badPair_;return false;}
    last_=plan;++prepared_;if(prepared)*prepared=plan;return true;
}
std::string SceneCameraBridge::report() const {
    std::lock_guard guard(mutex_);std::ostringstream o;
    o<<"{\"mode\":\"PREPARE_ONLY\",\"viewportWrites\":0,\"worldStereoFrames\":0,\"scaleCalibration\":\"UNVERIFIED\""
      <<",\"scenes\":"<<scenes_<<",\"decoded\":"<<decoded_<<",\"preparedPairs\":"<<prepared_
      <<",\"missingViews\":"<<missingViews_<<",\"badCamera\":"<<badCamera_<<",\"badPair\":"<<badPair_
      <<",\"busy\":"<<busy_<<",\"firstThread\":"<<firstThread_<<",\"otherThreadScenes\":"<<otherThreadScenes_
      <<",\"lastPreparedScene\":"<<last_.sceneId<<",\"lastXrFrame\":"<<last_.tracking.frameId
      <<",\"ipdMetres\":"<<last_.ipdMetres<<",\"nearClip\":"<<stock_.nearClip<<",\"farClip\":"<<stock_.farClip
      <<",\"stockPosition\":["<<stock_.world[12]<<','<<stock_.world[13]<<','<<stock_.world[14]<<"]}";
    return o.str();
}
} // namespace rdrvr::engine
