#pragma once
#include "engine/ConservativeWallCones.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

namespace rdrvr::experimental {
// PRIVATE native-list policy, currently NOT hooked into retail. The native
// adapter must supply the post-MAIN-wait list and independent skip permits.
// A geometric hidden result alone is never authority to remove DrawCommon.
class EarlyStaticVisibility {
public:
    using Cones=engine::ConservativeWallCones;
    using Frame=Cones::Frame;
    static constexpr unsigned Capacity=Cones::MaxCandidates;
    static constexpr std::uintptr_t RecordStride=0xb0;
    enum class Family : unsigned {
        StaticCommon, RigidMatrixSet, SkinnedMatrixSet, MixedMatrixSet,
        NativeInstanceList, SpecialOrCreature, Unknown, Count
    };
    enum class Pass : unsigned {LeftDepth,LeftColor,RightDepth,RightColor};
    struct List {
        std::uintptr_t container{},records{};
        unsigned count{};
        bool valid()const noexcept {
            return container&&records&&count&&count<=Capacity&&
                records<=UINTPTR_MAX-RecordStride*count;
        }
        bool operator==(const List&)const=default;
    };
    struct Candidate {
        unsigned index{};
        Family family{Family::Unknown};
        Cones::Raster::Bounds bounds{};
        std::array<Cones::Raster::Errors,2> errors{};
        // Opaque native adapter receipt for complete current chosen-LOD bounds.
        // With a conditional affine bound, the ACTUALLY selected shader must
        // prove that position contract at drawAllowed, together with graph
        // side effects and current pass/query state immediately before a skip.
        // It is NOT a hash of old visibility or an unverified node pointer.
        std::uint64_t permit{};
    };
    struct Authority {
        void* context{};
        bool(*listCurrent)(void*,Frame,List)noexcept{};
        bool(*recordCurrent)(void*,Frame,List,unsigned,Family,std::uint64_t)noexcept{};
        // A temporary active query or unsupported pass retains this call;
        // it does not revoke immutable geometry or poison unrelated passes.
        bool(*drawAllowed)(void*,Frame,List,unsigned,Family,std::uint64_t,Pass)noexcept{};
        // Optional per-call metadata receipt. Its expiry retains this native
        // call without poisoning otherwise valid geometry/source ownership.
        bool(*commitDraw)(void*)noexcept{};
        // Optional stronger publication boundary for a real lexical native
        // Read. Unlike per-call query/image expiry, a revoked frame poisons
        // earlier skips too. Generic adapters retain all original checks.
        bool(*commitFrame)(void*)noexcept{};
    };
    struct Stats {
        std::array<unsigned,unsigned(Family::Count)> admitted{},permitted{},hidden{};
        std::array<unsigned,4> commonSkipped{},instancesSkipped{};
        std::array<unsigned,4> drawDeclined{};
        unsigned selectedSources{},sourcePinned{},unrepresented{},oneEye{},
            invalidCandidates{},lookupMiss{},permitRefused{},invalidations{};
        bool prepared{},poisoned{};
    };
    // Called ONCE after the complete native MAIN wait returns, including its
    // sorting/transparent work, under frozen PRIMARY depth pose. All native
    // instance indices must be decoded FIRST; their records are not ordinary
    // renDrawBucketInfo and may contain stale node/drawable/previous fields.
    bool prepare(Frame f,List l,std::span<const Candidate> candidates,
            std::span<const unsigned> pinnedSourceRecords,Cones& cones,Authority a)noexcept {
        if(std::this_thread::get_id()!=owner_)return false;
        if(active_||stats_.prepared){poison();return false;}
        reset();
        if(!f.valid()||!l.valid()||!a.listCurrent||!a.recordCurrent||!a.drawAllowed||
           candidates.size()>l.count||pinnedSourceRecords.size()>Cones::MaxWalls||
           !cones.ready(f)||!a.listCurrent(a.context,f,l))return false;
        frame_=f;list_=l;authority_=a;cones_=&cones;coneSerial_=cones.buildSerial();
        for(auto index:pinnedSourceRecords){
            if(index>=l.count){reset();return false;}
            if(!entries_[index].source){entries_[index].source=true;++stats_.selectedSources;}
        }
        for(const auto& candidate:candidates){
            if(candidate.index>=l.count||unsigned(candidate.family)>=unsigned(Family::Count)||
               entries_[candidate.index].present){reset();return false;}
            auto& entry=entries_[candidate.index];entry.present=true;entry.family=candidate.family;
            ++stats_.admitted[unsigned(candidate.family)];
            if(entry.source){++stats_.sourcePinned;continue;}
            if(!candidate.permit||!a.recordCurrent(a.context,f,l,candidate.index,candidate.family,candidate.permit)){
                ++stats_.permitRefused;continue;
            }
            // A receipt cannot accidentally opt an unsupported family into a
            // whole-call skip. Skinning/simulation adapters need separate work.
            if(candidate.family!=Family::StaticCommon&&candidate.family!=Family::RigidMatrixSet&&
               candidate.family!=Family::NativeInstanceList){++stats_.permitRefused;continue;}
            ++stats_.permitted[unsigned(candidate.family)];
            entry.permit=candidate.permit;entry.bounds=candidate.bounds;entry.errors=candidate.errors;
            evaluate(entry);
        }
        stats_.unrepresented=l.count-unsigned(candidates.size());
        if(!cones.ready(f)||!a.listCurrent(a.context,f,l)){reset();return false;}
        stats_.prepared=true;return true;
    }
    // Scope only MAIN passes. Shadow and reflection draws must not inherit the
    // mask. Strict pass order prevents reusing one mask for a later scene.
    bool enter(Frame f,Pass p)noexcept {
        if(std::this_thread::get_id()!=owner_)return false;
        if(active_||!stats_.prepared||
           stats_.poisoned||f!=frame_||unsigned(p)!=nextPass_||!current()){
            if(stats_.prepared)poison();return false;
        }
        active_=true;pass_=p;return true;
    }
    bool leave()noexcept {
        if(std::this_thread::get_id()!=owner_)return false;
        if(!active_){poison();return false;}
        const bool valid=current();active_=false;++nextPass_;
        if(!valid)poison();return valid&&!stats_.poisoned;
    }
    bool commonSkip(std::uintptr_t container,std::uintptr_t record)noexcept {
        if(std::this_thread::get_id()!=owner_)return false;
        if(!active_||stats_.poisoned)return false;
        unsigned index{};
        if(!indexOf(container,record,index)){++stats_.lookupMiss;return false;}
        auto& entry=entries_[index];
        if(entry.family!=Family::StaticCommon&&entry.family!=Family::RigidMatrixSet)return false;
        if(!reject(index))return false;
        ++stats_.commonSkipped[unsigned(pass_)];return true;
    }
    // Native InstanceList adapter compacts its own current u16 indices while
    // preserving order and all per-instance data. Never call commonSkip on
    // those records or blindly reuse their stale ordinary-record fields.
    bool instanceHidden(std::uintptr_t container,unsigned index)noexcept {
        if(std::this_thread::get_id()!=owner_)return false;
        if(!active_||stats_.poisoned||container!=list_.container||index>=list_.count||
           entries_[index].family!=Family::NativeInstanceList)return false;
        if(!reject(index))return false;
        ++stats_.instancesSkipped[unsigned(pass_)];return true;
    }
    bool complete()const noexcept {return std::this_thread::get_id()==owner_&&stats_.prepared&&!stats_.poisoned&&!active_&&nextPass_==4;}
    const Stats& stats()const noexcept{return stats_;}
    void reset()noexcept {
        if(std::this_thread::get_id()!=owner_)return;
        // Bounds are read only after a fresh permit was installed. Clear just
        // live record metadata, not a ~1 MiB temporary/all-capacity array per
        // frame. This also bounds stack usage on the native render thread.
        for(unsigned i=0;i<list_.count;++i){auto& entry=entries_[i];
            entry.permit=entry.queriedRevision=0;entry.family=Family::Unknown;
            entry.present=entry.hidden=entry.source=entry.oneEyeCounted=false;
        }
        frame_={};list_={};authority_={};cones_=nullptr;coneSerial_=0;stats_={};
        nextPass_=0;active_=false;
    }
private:
    friend class NativeEarlyFrameController;
    // Scheduling hint only. No native reads, liveness grant or skip decision.
    // The controller may use it to open its existing same-call Read/DrawScope
    // sooner; commonSkip still performs every proof and final commit check.
    bool knownHidden(std::uintptr_t container,std::uintptr_t record)const noexcept {
        if(std::this_thread::get_id()!=owner_||!active_||!stats_.prepared||stats_.poisoned)return false;
        unsigned index{};if(!indexOf(container,record,index))return false;
        const auto& entry=entries_[index];
        return entry.present&&!entry.source&&entry.permit&&entry.hidden&&
            (entry.family==Family::StaticCommon||entry.family==Family::RigidMatrixSet);
    }
    struct Entry {
        Cones::Raster::Bounds bounds{};std::array<Cones::Raster::Errors,2> errors{};
        std::uint64_t permit{},queriedRevision{};Family family{Family::Unknown};
        bool present{},hidden{},source{},oneEyeCounted{};
    };
    std::array<Entry,Capacity> entries_{};
    std::thread::id owner_{std::this_thread::get_id()};
    Frame frame_{};List list_{};Authority authority_{};Cones* cones_{};std::uint64_t coneSerial_{};
    Stats stats_{};unsigned nextPass_{};Pass pass_{};bool active_{};
    bool indexOf(std::uintptr_t container,std::uintptr_t address,unsigned& index)const noexcept {
        if(container!=list_.container||address<list_.records)return false;
        const auto offset=address-list_.records;
        if(offset%RecordStride||offset/RecordStride>=list_.count)return false;
        index=unsigned(offset/RecordStride);return true;
    }
    bool current()const noexcept {
        return cones_&&coneSerial_&&cones_->buildSerial()==coneSerial_&&cones_->ready(frame_)&&authority_.listCurrent&&
            authority_.listCurrent(authority_.context,frame_,list_);
    }
    void evaluate(Entry& entry)noexcept {
        if(!entry.permit||entry.source||entry.hidden||!cones_)return;
        const auto revision=cones_->validSourceRevision();
        if(!revision||revision==entry.queriedRevision)return;
        entry.queriedRevision=revision;
        const auto mask=cones_->hidden(frame_,entry.bounds,entry.errors);
        if((mask==1||mask==2)&&!entry.oneEyeCounted){++stats_.oneEye;entry.oneEyeCounted=true;}
        if(mask==3){entry.hidden=true;++stats_.hidden[unsigned(entry.family)];}
    }
    bool reject(unsigned index)noexcept {
        auto& entry=entries_[index];
        if(!entry.present||entry.source||!entry.permit)return false;
        if(!current()){poison();return false;}
        // A current completed opaque seed may arrive during LEFT depth. Only
        // already-visible records need re-querying after that monotonic
        // addition; known hidden records stay hidden within this build serial.
        evaluate(entry);
        if(!entry.hidden)return false;
        if(!current()||!authority_.recordCurrent(authority_.context,frame_,list_,index,entry.family,entry.permit)){
            poison();return false;
        }
        if(!authority_.drawAllowed(authority_.context,frame_,list_,index,entry.family,entry.permit,pass_)){
            ++stats_.drawDeclined[unsigned(pass_)];return false;
        }
        // The per-call state adapter may inspect native state but must not
        // mutate it. Recheck source/record receipt across that inspection.
        if(!current()||!authority_.recordCurrent(authority_.context,frame_,list_,index,entry.family,entry.permit)){
            poison();return false;
        }
        // The final record callback may use a narrowly qualified current Read.
        // Publish a skip only after the full frame/source owner is checked
        // AFTER that callback, including independent shader/mesh revocation.
        if(!current()){poison();return false;}
        if(authority_.commitFrame&&!authority_.commitFrame(authority_.context)){
            poison();return false;
        }
        if(authority_.commitDraw&&!authority_.commitDraw(authority_.context)){
            ++stats_.drawDeclined[unsigned(pass_)];return false;
        }
        return true;
    }
    void poison()noexcept{stats_.poisoned=true;++stats_.invalidations;}
};
}
