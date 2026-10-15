#pragma once
#include "ControllerInput.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace rdrvr::runtime {
// Uses the existing left menu action: tap pauses the game, hold opens graphics.
// Consumed input is published neutral. Closing waits for physical neutral so a
// held trigger cannot leak into gameplay. No extra sync or synthetic tracking.
class GraphicsMenuInput {
    bool open_{},armed_{},menuHeld_{},suppressRelease_{},releaseGuard_{};
    std::uint32_t previousButtons_{};
    std::uint64_t pressNs_{},pauseUntil_{},lastNs_{},nextNav_{};
    int nav_{};unsigned selected_{};std::array<bool,2> triggerHeld_{true,true}; // Per trigger (armed only after it was released)
    int ignoreGrip_{-1}; // Hand whose grip the close guard ignores (it holds a VR weapon), -1 none
public:
    // The hand that holds a VR weapon keeps its grip squeezed through the menu; closing must not wait for it to open
    // (the holsters own that grip; its trigger still has to be released).
    void ignoreGrip(int hand) noexcept {ignoreGrip_=hand==0||hand==1?hand:-1;}
    // 23 rows (the panel's 25-line maximum with the title/FPS line and one footer line). SUN SHADOWS absorbs
    // SHADOW UPDATE, GRASS absorbs GRASS DISTANCE. Shadows/ShadowUpdate/GrassDistance stay as aliases for old callers.
    // No CLOSE row: B closes the menu, as does holding the menu button.
    // heat01: + HEAT WARNING after PROFILER (24 rows, 26 panel lines: PerfPanel grew to 430 px).
    // boost01: + GPU BOOST after HEAT WARNING (25 rows, 27 panel lines: PerfPanel 446 px).
    // w02: + WATER after GRASS (26 rows; the panel stays 27 lines: GraphicsMenu pads one hidden row less).
    enum Row : unsigned {Resolution,SunShadows,ShadowMap,CascadeCull,LampShadows,Grass,Water,EdgeMargin,Occlusion,SmallObjects,Foveation,FoveationApi,
TextureFilter,MipBias,Sharpen,AntiAliasing,PostFx,FrameRate,SpaceWarp,XrQueue,Profiler,HeatWarning,GpuBoost,StereoTest,Teleport,Defaults,Count,
Shadows=SunShadows,ShadowUpdate=SunShadows,GrassDistance=Grass};
    // page = -1 (X) / +1 (Y) while open; the owner (GraphicsMenu) switches pages and calls select().
    // back = B inside a submenu (update(..., bBack=true)): the owner returns to the ROOT page instead of closing.
    struct Event {bool consume{},opened{},closed{},changed{};int adjust{};bool activate{};int page{};bool back{};int triggerStep{};}; // +1 right trigger, -1 left
    bool open()const noexcept{return open_;}
    unsigned selected()const noexcept{return selected_;}
    void select(unsigned row) noexcept {selected_=row;} // A page switch restores that page's row (stick repeat state kept)
    void invalidate() noexcept {open_=armed_=menuHeld_=suppressRelease_=releaseGuard_=false;previousButtons_=0;pressNs_=pauseUntil_=lastNs_=nextNav_=0;nav_=0;}
    // rows = row count of the current page (GRAPHICS = Count); up/down wrap at it.
    // bBack = the current page is a submenu, so B means BACK (out.back) and the menu stays open.
    Event update(ControllerSample& sample,unsigned rows=Count,bool bBack=false) noexcept {
        if(!rows)rows=1;
        Event out{};
        if(!validController(sample)||sample.publishedNs<=lastNs_){invalidate();return out;}
        const auto now=sample.publishedNs;lastNs_=now;
        const auto buttons=sample.buttons;const bool menu=(buttons&PadMenu)!=0;
        const auto pressed=buttons&~previousButtons_;previousButtons_=buttons;
        if(releaseGuard_){
            bool neutral=buttons==0;
            for(float v:sample.sticks)neutral=neutral&&std::abs(v)<=.15f;
            for(float v:sample.triggers)neutral=neutral&&v<=.15f;
            for(unsigned h=0;h<2;++h)if(int(h)!=ignoreGrip_)neutral=neutral&&sample.grips[h]<=.15f;
            if(neutral){releaseGuard_=menuHeld_=suppressRelease_=false;pressNs_=pauseUntil_=0;}
            out.consume=true;return out;
        }
        if(!armed_){if(!menu&&!(buttons&(PadA|PadB)))armed_=true;return out;}
        if(menu&&!menuHeld_){menuHeld_=true;pressNs_=now;pauseUntil_=0;}
        if(menuHeld_&&menu){
            out.consume=true;
            if(!suppressRelease_&&now-pressNs_>=600'000'000){
                open_=!open_;out.opened=open_;out.closed=!open_;out.changed=true;
                if(out.closed){releaseGuard_=true;menuHeld_=false;}
                suppressRelease_=true;nav_=0;nextNav_=0;triggerHeld_={true,true};
            }
            return out;
        }
        if(menuHeld_&&!menu){
            menuHeld_=false;
            if(!suppressRelease_&&!open_)pauseUntil_=now+120'000'000;
            suppressRelease_=false;
            // Do not activate a row using the button that opened the menu.
            if(open_){out.consume=true;return out;}
        }
        if(!open_){if(now<pauseUntil_)sample.buttons|=PadMenu;return out;}
        out.consume=true;
        if((pressed&PadB)&&bBack){out.back=out.changed=true;nav_=0;nextNav_=0;return out;}
        if(pressed&PadB){open_=false;releaseGuard_=true;out.closed=out.changed=true;return out;}
        if(selected_>=rows){selected_=rows-1;out.changed=true;} // A page shrank under the mark
        if(pressed&(PadX|PadY)){out.page=(pressed&PadY)?1:-1;out.changed=true;return out;} // X = previous page, Y = next
        // Either stick navigates (the stronger one): a hand holding a weapon can leave its stick alone.
        const bool right=std::abs(sample.sticks[2])+std::abs(sample.sticks[3])>std::abs(sample.sticks[0])+std::abs(sample.sticks[1]);
        const float x=sample.sticks[right?2:0],y=sample.sticks[right?3:1];
        // (user: values changed by accident while scrolling down): sideways only on a clear push (|x| >= .8 and > 2 |y|)
        // GraphicsMenu drops it on the settings pages anyway (A / trigger cycle the value there).
        const int direction=std::abs(y)>=.55f&&std::abs(y)>=std::abs(x)?(y>0?1:2):std::abs(x)>=.8f&&std::abs(x)>2.f*std::abs(y)?(x>0?3:4):0;
        if(!direction){nav_=0;nextNav_=0;}
        else if(direction!=nav_||now>=nextNav_){
            const bool first=direction!=nav_;nav_=direction;nextNav_=now+(first?350'000'000:150'000'000);
            if(direction==1)selected_=(selected_+rows-1)%rows;
            else if(direction==2)selected_=(selected_+1)%rows;
            else out.adjust=direction==3?1:-1;
            out.changed=true;
        }
        if(pressed&PadA)out.activate=out.changed=true;
        // right trigger = one step forward, left trigger = one step back (edge; released (<= .3) in between)
        for(unsigned h=0;h<2;++h){const float t=sample.triggers[h];
            if(t<=.3f)triggerHeld_[h]=false;
            else if(t>=.7f&&!triggerHeld_[h]){triggerHeld_[h]=true;out.triggerStep=h?1:-1;out.changed=true;}}
        return out;
    }
};
}
