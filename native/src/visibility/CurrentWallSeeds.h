#pragma once
#include "engine/ConservativeWallCones.h"
#include <array>
#include <cstdint>
#include <span>
#include <thread>

namespace rdrvr::experimental {
// PRIVATE bridge for actual successful native LEFT-depth witnesses. Selection
// is only a cold-geometry hint. A source is published only AFTER the original
// DrawCommon returns successfully with matching target/resource/pose authority.
// Native order, shaders, material selection and rendering are not emulated.
class CurrentWallSeeds {
public:
    using Cones=engine::ConservativeWallCones;
    using Frame=Cones::Frame;
    struct Selection {
        std::uint64_t object{},geometry{},geometryGeneration{},record{};
        std::uint32_t patch{};
        // These are only current-record selection hints. Material, chosen LOD,
        // transform and actual opaque pipeline come from the NEW draw witness.
        static Selection from(const Cones::SourceKey& k)noexcept{return{k.object,k.geometry,k.geometryGeneration,k.record,k.patch};}
        bool matches(const Cones::SourceKey& k)const noexcept {
            return object==k.object&&geometry==k.geometry&&geometryGeneration==k.geometryGeneration&&record==k.record&&patch==k.patch;
        }
    };
    struct Stats {unsigned observed{},accepted{},unselected{},unfinished{},invalidated{},overflow{},nested{};};
    class Draw;
    bool begin(Frame f,Cones& cones,std::span<const Selection> selected)noexcept {
        if(std::this_thread::get_id()!=owner_||active_||selected.size()>Cones::MaxWalls||!cones.ready(f))return false;
        frame_=f;cones_=&cones;serial_=cones.buildSerial();count_=unsigned(selected.size());
        stats_={};published_={};
        for(unsigned i=0;i<count_;++i)selected_[i]=selected[i];
        return true;
    }
    const Stats& stats()const noexcept{return stats_;}
    class Draw {
        friend class CurrentWallSeeds;
        CurrentWallSeeds* owner_{};Draw* previous_{};
        std::uint64_t record_{};
        unsigned count_{};
        bool completed_{},qualified_{};
    public:
        Draw(CurrentWallSeeds& o,std::uint64_t record,bool actualMainLeftDepth)noexcept {
            if(std::this_thread::get_id()!=o.owner_)return;
            owner_=&o;previous_=o.active_;o.active_=this;record_=record;
            qualified_=actualMainLeftDepth&&record&&!previous_&&o.current();
            if(qualified_){
                bool selected=false;
                for(unsigned i=0;i<o.count_;++i)selected|=o.selected_[i].record==record;
                qualified_=selected;
            }
            if(previous_)++o.stats_.nested;
        }
        Draw(const Draw&)=delete;Draw& operator=(const Draw&)=delete;
        ~Draw(){if(owner_){owner_->stats_.unfinished+=!completed_?count_:0;owner_->active_=previous_;}}
        // Called by the exact successful primitive continuation. Source owns
        // copied geometry/transform/error data, never a borrowed native pointer.
        // Passing a prepared-but-not-emitted primitive here is an adapter bug.
        bool observe(const Cones::Source& s)noexcept {
            if(!owner_||std::this_thread::get_id()!=owner_->owner_||owner_->active_!=this||completed_||!qualified_)return false;
            auto& o=*owner_;++o.stats_.observed;
            if(!o.current()||s.key.record!=record_){++o.stats_.invalidated;return false;}
            unsigned slot=o.count_;
            for(unsigned i=0;i<o.count_;++i)if(o.selected_[i].matches(s.key)){slot=i;break;}
            if(slot==o.count_){++o.stats_.unselected;return false;}
            if(o.published_[slot])return false;
            for(unsigned i=0;i<count_;++i)if(o.pendingSlot_[i]==slot)return false;
            if(count_>=o.pending_.size()){++o.stats_.overflow;return false;}
            o.pending_[count_]=s;o.pendingSlot_[count_++]=slot;return true;
        }
        // Invoke only after original DrawCommon and its mandatory callbacks
        // returned. Exception/unwind, suppressed primitive or invalid native
        // scope must leave complete uncalled or pass false.
        bool complete(bool nativeScopeGood)noexcept {
            if(!owner_||std::this_thread::get_id()!=owner_->owner_||owner_->active_!=this||completed_)return false;
            completed_=true;auto& o=*owner_;
            if(!qualified_||!nativeScopeGood||!o.current()){
                o.stats_.invalidated+=count_;return false;
            }
            bool any=false;
            for(unsigned i=0;i<count_;++i){
                // append revalidates current source authority independently;
                // completion itself never upgrades an old material witness.
                if(o.cones_->append(o.frame_,o.pending_[i])){
                    o.published_[o.pendingSlot_[i]]=true;++o.stats_.accepted;any=true;
                }else ++o.stats_.invalidated;
            }
            return any;
        }
    };
private:
    std::thread::id owner_{std::this_thread::get_id()};
    Frame frame_{};Cones* cones_{};std::uint64_t serial_{};Draw* active_{};
    std::array<Selection,Cones::MaxWalls> selected_{};
    // Only the single qualified top-level draw writes this durable buffer.
    // Do not clear 32 copied polygons on the stack for every DrawCommon.
    std::array<Cones::Source,Cones::MaxWalls> pending_{};
    std::array<unsigned,Cones::MaxWalls> pendingSlot_{};
    std::array<bool,Cones::MaxWalls> published_{};unsigned count_{};Stats stats_{};
    bool current()const noexcept {return cones_&&serial_&&cones_->buildSerial()==serial_&&cones_->ready(frame_);}
};
}
