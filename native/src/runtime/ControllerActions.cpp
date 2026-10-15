#include "ControllerActions.h"
#include "vr/VrTouch.h" // Reload / cylinder close: XrSpaceVelocity of the grip spaces -> the XR tape; SPIN: trigger-touch side channel
#include "vr/VrInputRec.h" // INPUT RECORD / REPLAY: the raw action states of read() (record, or replace while a replay plays)
#include <cstring>
namespace rdrvr::runtime {
// SPIN (VrTouch.h): the trigger-touch action lives here as a file-scope static - no member in the mirror ControllerActions.h (ODR).
static XrAction gTriggerTouch{XR_NULL_HANDLE};
void ControllerActions::create(XrInstance instance,XrActionSet set,const std::array<XrPath,2>& hands,std::vector<XrActionSuggestedBinding>& bindings){
    hands_=hands;
    auto action=[&](const char* name,const char* label,XrActionType type,bool perHand){
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};info.actionType=type;
        std::strncpy(info.actionName,name,sizeof(info.actionName)-1);std::strncpy(info.localizedActionName,label,sizeof(info.localizedActionName)-1);
        if(perHand){info.countSubactionPaths=2;info.subactionPaths=hands_.data();}
        XrAction result{};checkXr(xrCreateAction(set,&info,&result),"controller action");return result;
    };
    stick_=action("pad_stick","Movement and camera",XR_ACTION_TYPE_VECTOR2F_INPUT,true);
    trigger_=action("pad_trigger","Triggers",XR_ACTION_TYPE_FLOAT_INPUT,true);
    squeeze_=action("pad_grip","Shoulder buttons",XR_ACTION_TYPE_FLOAT_INPUT,true);
    click_=action("pad_stick_click","Stick clicks",XR_ACTION_TYPE_BOOLEAN_INPUT,true);
    gripPose_=action("pad_grip_pose","Controller grip pose",XR_ACTION_TYPE_POSE_INPUT,true);
    haptic_=action("game_rumble","Game vibration",XR_ACTION_TYPE_VIBRATION_OUTPUT,true);
    const char* names[]{"pad_a","pad_b","pad_x","pad_y","pad_menu"};
    const char* labels[]{"A","B","X","Y","Pause"};
    for(unsigned i=0;i<5;++i)face_[i]=action(names[i],labels[i],XR_ACTION_TYPE_BOOLEAN_INPUT,false);
    auto bind=[&](XrAction a,const char* name){XrPath path{};checkXr(xrStringToPath(instance,name,&path),"controller binding path");bindings.push_back({a,path});};
    const char* side[]{"/user/hand/left/input/","/user/hand/right/input/"};
    for(unsigned i=0;i<2;++i){
        const XrAction actions[]{stick_,trigger_,squeeze_,click_,gripPose_};
        const char* suffix[]{"thumbstick","trigger/value","squeeze/value","thumbstick/click","grip/pose"};
        for(unsigned j=0;j<5;++j)bind(actions[j],(std::string(side[i])+suffix[j]).c_str());
    }
    const char* paths[]{"/user/hand/right/input/a/click","/user/hand/right/input/b/click","/user/hand/left/input/x/click","/user/hand/left/input/y/click","/user/hand/left/input/menu/click"};
    for(unsigned i=0;i<5;++i)bind(face_[i],paths[i]);
    bind(haptic_,"/user/hand/left/output/haptic");bind(haptic_,"/user/hand/right/output/haptic");
    // SPIN: index on the trigger (capacitive) - bindings appended LAST so XrHarness can drop exactly them if the runtime refuses
    gTriggerTouch=XR_NULL_HANDLE;::rdrvr::vr::touch::bindingCount.store(0,std::memory_order_relaxed);
    if(::rdrvr::vr::touch::enabled()){
        gTriggerTouch=action("pad_trigger_touch","Trigger touch",XR_ACTION_TYPE_BOOLEAN_INPUT,true);
        bind(gTriggerTouch,"/user/hand/left/input/trigger/touch");bind(gTriggerTouch,"/user/hand/right/input/trigger/touch");
        ::rdrvr::vr::touch::bindingCount.store(2,std::memory_order_relaxed);
    }
}
void ControllerActions::createSpaces(XrSession session){
    hapticSession_=session;hapticOutput_.configure({this,applyHaptic,stopHaptic});
    for(unsigned i=0;i<2;++i){XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};info.action=gripPose_;info.subactionPath=hands_[i];info.poseInActionSpace.orientation.w=1;
        checkXr(xrCreateActionSpace(session,&info,&gripSpaces_[i]),"controller grip space");}
}
void ControllerActions::destroySpaces() noexcept {invalidate();hapticSession_={};for(auto& space:gripSpaces_){if(space)xrDestroySpace(space);space={};}}
bool ControllerActions::applyHaptic(void* context,unsigned hand,float amplitude,std::uint64_t duration){
    auto& self=*static_cast<ControllerActions*>(context);if(!self.hapticSession_||!self.haptic_||hand>=2)return false;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};info.action=self.haptic_;info.subactionPath=self.hands_[hand];
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};vibration.duration=static_cast<XrDuration>(duration);
    vibration.frequency=XR_FREQUENCY_UNSPECIFIED;vibration.amplitude=amplitude;
    return xrApplyHapticFeedback(self.hapticSession_,&info,reinterpret_cast<const XrHapticBaseHeader*>(&vibration))==XR_SUCCESS;
}
bool ControllerActions::stopHaptic(void* context,unsigned hand){
    auto& self=*static_cast<ControllerActions*>(context);if(!self.hapticSession_||!self.haptic_||hand>=2)return false;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};info.action=self.haptic_;info.subactionPath=self.hands_[hand];
    return xrStopHapticFeedback(self.hapticSession_,&info)==XR_SUCCESS;
}
bool ControllerActions::read(XrSession session,XrSpace base,XrTime time,ControllerSample& sample,bool& recenter,bool& theaterToggle){
    recenter=theaterToggle=false;sample.active=false;sample.buttons=0;sample.sticks={};sample.triggers={};sample.grips={};sample.grip={};
    if(!sample.focused){invalidate();return false;}
    // INPUT RECORD / REPLAY (VrInputRec.h): every OpenXR action state of this read goes into one PadRaw first (result, isActive,
    // currentState, changedSinceLastSync, grip locate flags + pose + XrSpaceVelocity); ir::pad records it or, while a replay plays, replaces it;
    // the sample is then built from the PadRaw (good / anyActive / per-action neutral state).
    namespace ir=::rdrvr::vr::inputrec;namespace rb=ir::bit;ir::PadRaw raw{};
    const auto mark=[](std::uint32_t& word,unsigned b,bool on){if(on)word|=1u<<b;};
    const auto note=[&](unsigned b,XrResult result,XrBool32 isActive,XrBool32 changed){const bool ok=XR_SUCCEEDED(result);
        mark(raw.ok,b,ok);mark(raw.active,b,ok&&isActive);mark(raw.changed,b,ok&&changed);return ok;};
    auto get=[&](XrAction action,int hand){XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};info.action=action;info.subactionPath=hand<0?XR_NULL_PATH:hands_[hand];return info;};
    for(unsigned i=0;i<2;++i){
        auto info=get(stick_,i);XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
        if(note(rb::stick+i,xrGetActionStateVector2f(session,&info,&stick),stick.isActive,stick.changedSinceLastSync)){raw.stick[i][0]=stick.currentState.x;raw.stick[i][1]=stick.currentState.y;}
        info=get(trigger_,i);XrActionStateFloat trigger{XR_TYPE_ACTION_STATE_FLOAT};
        if(note(rb::trigger+i,xrGetActionStateFloat(session,&info,&trigger),trigger.isActive,trigger.changedSinceLastSync))raw.trigger[i]=trigger.currentState;
        info=get(squeeze_,i);XrActionStateFloat grip{XR_TYPE_ACTION_STATE_FLOAT};
        if(note(rb::squeeze+i,xrGetActionStateFloat(session,&info,&grip),grip.isActive,grip.changedSinceLastSync))raw.squeeze[i]=grip.currentState;
        info=get(click_,i);XrActionStateBoolean click{XR_TYPE_ACTION_STATE_BOOLEAN};
        if(note(rb::click+i,xrGetActionStateBoolean(session,&info,&click),click.isActive,click.changedSinceLastSync))mark(raw.state,rb::click+i,click.isActive&&click.currentState);
        info=get(gripPose_,i);XrActionStatePose pose{XR_TYPE_ACTION_STATE_POSE};
        if(note(rb::gripPose+i,xrGetActionStatePose(session,&info,&pose),pose.isActive,XR_FALSE)&&pose.isActive){XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            // The runtime's own grip velocities (gyro, no division by dt) in the same locate call;
            // a velocity the runtime does not report VALID is just absent - it never invalidates the pose.
            XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};location.next=&velocity;
            if(XR_SUCCEEDED(xrLocateSpace(gripSpaces_[i],base,time,&location))){const auto& q=location.pose;mark(raw.ok,rb::gripLocated+i,true);
                const float g[7]{q.orientation.x,q.orientation.y,q.orientation.z,q.orientation.w,q.position.x,q.position.y,q.position.z};std::memcpy(raw.grip[i],g,sizeof g);
                raw.gripFlags[i]=std::uint32_t(location.locationFlags);raw.velFlags[i]=std::uint32_t(velocity.velocityFlags);
                const float w[3]{velocity.angularVelocity.x,velocity.angularVelocity.y,velocity.angularVelocity.z},v[3]{velocity.linearVelocity.x,velocity.linearVelocity.y,velocity.linearVelocity.z};
                std::memcpy(raw.angular[i],w,sizeof w);std::memcpy(raw.linear[i],v,sizeof v);}}
    }
    for(unsigned i=0;i<5;++i){auto info=get(face_[i],-1);XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        if(note(rb::face+i,xrGetActionStateBoolean(session,&info,&state),state.isActive,state.changedSinceLastSync))mark(raw.state,rb::face+i,state.isActive&&state.currentState);}
    if(gTriggerTouch!=XR_NULL_HANDLE)for(unsigned i=0;i<2;++i){auto info=get(gTriggerTouch,int(i));XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        if(note(rb::touch+i,xrGetActionStateBoolean(session,&info,&st),st.isActive,st.changedSinceLastSync))mark(raw.state,rb::touch+i,st.isActive&&st.currentState);}
    ir::pad(raw,true);
    bool good=true,anyActive=false;
    const auto ok=[&](unsigned b){return ((raw.ok>>b)&1u)!=0;};
    const auto on=[&](unsigned b){return ((raw.ok>>b)&1u)&&((raw.active>>b)&1u);}; // XR_SUCCEEDED && isActive
    for(unsigned i=0;i<2;++i){
        good=good&&ok(rb::stick+i);anyActive=anyActive||on(rb::stick+i);
        if(on(rb::stick+i)){sample.sticks[2*i]=raw.stick[i][0];sample.sticks[2*i+1]=raw.stick[i][1];}
        good=good&&ok(rb::trigger+i);anyActive=anyActive||on(rb::trigger+i);if(on(rb::trigger+i))sample.triggers[i]=raw.trigger[i];
        good=good&&ok(rb::squeeze+i);anyActive=anyActive||on(rb::squeeze+i);if(on(rb::squeeze+i))sample.grips[i]=raw.squeeze[i];
        good=good&&ok(rb::click+i);anyActive=anyActive||on(rb::click+i);if(on(rb::click+i)&&((raw.state>>(rb::click+i))&1u))sample.buttons|=i?PadRightStick:PadLeftStick;
        if(on(rb::gripPose+i)&&ok(rb::gripLocated+i)){auto& p=sample.grip[i];const float* g=raw.grip[i];const auto flags=XrSpaceLocationFlags(raw.gripFlags[i]);
            p.orientation={g[0],g[1],g[2],g[3]};p.position={g[4],g[5],g[6]};
            p.positionValid=(flags&(XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_POSITION_TRACKED_BIT))==(XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
            p.orientationValid=(flags&(XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT))==(XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
            const auto vf=XrSpaceVelocityFlags(raw.velFlags[i]);
            ::rdrvr::vr::touch::noteVelocity(i,sample.publishedNs,(vf&XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)!=0,raw.angular[i],(vf&XR_SPACE_VELOCITY_LINEAR_VALID_BIT)!=0,raw.linear[i]);}
    }
    const unsigned bits[]{PadA,PadB,PadX,PadY,PadMenu};
    for(unsigned i=0;i<5;++i){good=good&&ok(rb::face+i);anyActive=anyActive||on(rb::face+i);if(on(rb::face+i)&&((raw.state>>(rb::face+i))&1u))sample.buttons|=bits[i];}
    if(gTriggerTouch!=XR_NULL_HANDLE){ // SPIN: touch bits of this sample (a failure = "no touch", never invalidates the sample)
        bool act[2]{},down[2]{};
        for(unsigned i=0;i<2;++i){act[i]=on(rb::touch+i);down[i]=act[i]&&((raw.state>>(rb::touch+i))&1u);}
        ::rdrvr::vr::touch::pending(::rdrvr::vr::touch::readBits(act[0],down[0],act[1],down[1]),sample.publishedNs);
    }
    // An inactive hand/action contributes neutral state. It must not suppress
    // A from the other hand; API failures and complete loss still invalidate.
    sample.active=good&&anyActive;
    if(!validController(sample)){sample.active=false;invalidate();return false;}
    // L3/R3 single clicks vs the recenter chord (vr/VrControls.h StickClickArbiter).
    static_assert(unsigned(PadLeftStick)==::rdrvr::vr::controls::bit::L3&&unsigned(PadRightStick)==::rdrvr::vr::controls::bit::R3);
    const unsigned chord=PadLeftStick|PadRightStick;
    const auto clicks=stickClicks_.sample((sample.buttons&PadLeftStick)!=0,(sample.buttons&PadRightStick)!=0,sample.publishedNs);
    recenter=clicks.recenter;sample.buttons=(sample.buttons&~chord)|(clicks.bits&chord);
    if(manualTheater_) // Never while the VR holsters own a grip (the consumed grips read as a release there)
        theaterToggle=theaterGripChord(theaterChord_,theaterGrips_,sample.grips,sample.publishedNs,gripsBusy_&&gripsBusy_());
    return true;
}
}
