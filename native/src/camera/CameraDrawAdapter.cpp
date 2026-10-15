#include "CameraDrawAdapter.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <limits>
#include <thread>
#include <unordered_map>

namespace rdr95 {
namespace {struct Errno {int value=errno;~Errno(){errno=value;}};}
struct CameraDrawAdapter::Impl {
    using Variant=rdrvr::engine::FrozenCameraPipelineFactory::Variant;
    struct Layout {VkPipelineLayout handle{};std::uint64_t serial{},nativeGeneration{};unsigned count{},required{};std::array<unsigned,MaxSets> dynamic{};};
    struct Pipeline {VkPipeline handle{};Variant value{};std::uint64_t layoutSerial{};unsigned layoutIndex{};};
    struct Set {VkDescriptorSet handle{};unsigned count{};std::array<std::uint32_t,MaxDynamic> offsets{};};
    struct Command {
        VkCommandBuffer handle{};VkCommandPool pool{};VkCommandBufferLevel level{};
        std::uint64_t revision{};bool recording{},unknown{},busy{};
        VkPipeline intended{};VkPipelineLayout layout{};std::uint64_t layoutSerial{};
        unsigned pipelineIndex=UINT32_MAX;std::uint64_t pipelineSerial{};
        std::array<Set,MaxSets> sets{};
    };
    struct Plan {Command* command{};Variant variant{};CameraUniformOwner::Binding camera{};};
    struct LayoutKey {VkPipelineLayout handle{};std::uint64_t generation{};bool operator==(const LayoutKey&)const=default;};
    struct LayoutHash {std::size_t operator()(const LayoutKey& k)const noexcept {
        return std::hash<VkPipelineLayout>{}(k.handle)^(std::hash<std::uint64_t>{}(k.generation)+std::size_t(0x9e3779b9));}};
    std::array<Layout,1024> layouts{};std::array<Pipeline,4096> pipelines{};
    std::unordered_map<VkPipeline,unsigned> pipelineIndex;
    std::unordered_map<LayoutKey,unsigned,LayoutHash> metadataLayoutIndex;
    std::array<Command,MaxCommands> commands{};
    Api api{};CameraUniformOwner* camera{};CameraUniformOwner::Layout cameraLayout{};
    VkDevice device{};std::uint64_t deviceGeneration{},serial{};
    std::thread::id thread{};bool initialized{},backed{};std::atomic<bool> ready{};
    unsigned lastCommand=UINT32_MAX;
    bool owned()const noexcept{return thread==std::this_thread::get_id();}
    bool owner()noexcept{if(owned())return true;ready=false;return false;}
    std::uint64_t next()noexcept{if(serial==UINT64_MAX){ready=false;return 0;}return ++serial;}
    template<class A,class H>static auto* find(A& a,H h)noexcept {for(auto& e:a)if(e.handle==h)return &e;return static_cast<typename A::value_type*>(nullptr);}
    Layout* layout(VkPipelineLayout h)noexcept{return h?find(layouts,h):nullptr;}
    Layout* layout(VkPipelineLayout h,std::uint64_t generation)noexcept {
        const auto it=metadataLayoutIndex.find({h,generation});return it==metadataLayoutIndex.end()?nullptr:&layouts[it->second];
    }
    Pipeline* pipeline(VkPipeline h)noexcept {const auto it=pipelineIndex.find(h);return it==pipelineIndex.end()?nullptr:&pipelines[it->second];}
    Command* command(VkCommandBuffer h)noexcept{
        if(!h)return nullptr;if(lastCommand<commands.size()&&commands[lastCommand].handle==h)return &commands[lastCommand];
        auto* c=find(commands,h);if(c)lastCommand=unsigned(c-commands.data());return c;
    }
    void clear(Command& c)noexcept {
        const auto h=c.handle;const auto p=c.pool;const auto level=c.level;c={};c.handle=h;c.pool=p;c.level=level;c.revision=next();
    }
    void unknown(Command& c)noexcept {c.unknown=true;c.revision=next();}
    void invalidate(VkCommandBuffer h)noexcept {if(auto* c=command(h))unknown(*c);}
    bool metadataMatches(const DrawMetadata& m,const Layout& l)const noexcept {
        return m.nativeLayoutGeneration&&m.nativeLayoutGeneration==l.nativeGeneration&&
            m.setCount==l.count&&m.requiredNativeSets==l.required&&m.dynamicCounts==l.dynamic;
    }
    static bool sameVariant(const Variant& a,const Variant& b)noexcept {
        return a.nativePipeline==b.nativePipeline&&a.pipeline==b.pipeline&&a.layout==b.layout&&a.cameraLayout==b.cameraLayout&&
            a.deviceGeneration==b.deviceGeneration&&a.cameraLayoutGeneration==b.cameraLayoutGeneration&&a.serial==b.serial&&
            a.nativeLayout==b.nativeLayout&&a.nativeSetCount==b.nativeSetCount&&a.requiredNativeSets==b.requiredNativeSets&&a.restoreLayout==b.restoreLayout;
    }
    static bool sameMetadata(const DrawMetadata& a,const DrawMetadata& b)noexcept {
        return sameVariant(a.variant,b.variant)&&a.nativeLayoutGeneration==b.nativeLayoutGeneration&&
            a.setCount==b.setCount&&a.requiredNativeSets==b.requiredNativeSets&&a.dynamicCounts==b.dynamicCounts;
    }
    bool matches(const Pipeline* p,const DrawMetadata& m)const noexcept {
        if(!p||p->layoutIndex>=layouts.size()||!sameVariant(p->value,m.variant))return false;
        const auto& l=layouts[p->layoutIndex];return l.handle==m.variant.nativeLayout&&l.serial==p->layoutSerial&&metadataMatches(m,l);
    }
    Pipeline* importMetadata(VkPipeline h,const DrawMetadata& m,unsigned cachedIndex)noexcept {
        const auto& v=m.variant;
        if(!h||!v||!v.restoreLayout||v.nativePipeline!=h||!v.nativeLayout||!m.nativeLayoutGeneration||m.setCount>MaxSets||
           v.nativeSetCount!=m.setCount||v.requiredNativeSets!=m.requiredNativeSets||
           (m.requiredNativeSets&~((1u<<m.setCount)-1u))||(m.requiredNativeSets&8u)||
           v.deviceGeneration!=deviceGeneration||v.cameraLayout!=cameraLayout.handle||v.cameraLayoutGeneration!=cameraLayout.generation)return nullptr;
        for(unsigned i=0;i<MaxSets;++i)if(m.dynamicCounts[i]>MaxDynamic||
            ((i>=m.setCount||!(m.requiredNativeSets&(1u<<i)))&&m.dynamicCounts[i]))return nullptr;
        // Repeated actual bind uses the stable command index without rescanning
        // thousands of creation entries. New pipeline/layout is cold only.
        Pipeline* p=cachedIndex<pipelines.size()?&pipelines[cachedIndex]:nullptr;
        if(p&&p->handle==h&&p->value.serial==v.serial&&p->layoutIndex<layouts.size()){
            auto& l=layouts[p->layoutIndex];if(l.handle==v.nativeLayout&&l.serial==p->layoutSerial&&metadataMatches(m,l)&&
                sameVariant(p->value,v))return p;
        }
        p=pipeline(h);if(!p)p=find(pipelines,VK_NULL_HANDLE);if(!p)return nullptr;
        if(p->handle==h&&p->value.serial==v.serial){
            // A factory serial denotes one immutable creation. Never attach it
            // to altered metadata merely because a later callback said so.
            if(p->layoutIndex>=layouts.size())return nullptr;const auto& old=layouts[p->layoutIndex];
            if(old.handle!=v.nativeLayout||old.serial!=p->layoutSerial||!metadataMatches(m,old)||
               !sameVariant(p->value,v))return nullptr;
            return p;
        }
        auto* l=layout(v.nativeLayout,m.nativeLayoutGeneration);if(!l)l=find(layouts,VK_NULL_HANDLE);if(!l)return nullptr;
        if(l->handle&& !metadataMatches(m,*l))return nullptr;
        if(l->handle!=v.nativeLayout||!metadataMatches(m,*l)){
            const auto generation=next();if(!generation)return nullptr;
            try{metadataLayoutIndex.emplace(LayoutKey{v.nativeLayout,m.nativeLayoutGeneration},unsigned(l-layouts.data()));}catch(...){return nullptr;}
            *l={v.nativeLayout,generation,m.nativeLayoutGeneration,m.setCount,m.requiredNativeSets,m.dynamicCounts};
        }
        try{pipelineIndex.emplace(h,unsigned(p-pipelines.data()));}catch(...){return nullptr;}
        if(!next())return nullptr;*p={h,v,l->serial,unsigned(l-layouts.data())};return p;
    }
    void bindMetadata(Command& c,VkPipeline h)noexcept {
        const int incoming=errno;const auto before=c.revision,all=serial;const auto commandHandle=c.handle;
        auto* existing=pipeline(h);const unsigned index=existing?unsigned(existing-pipelines.data()):UINT32_MAX;
        DrawMetadata first{};const bool captured=api.lookupDrawMetadata(api.lookupContext,h,first);
        const bool stable=ready&&owned()&&serial==all&&c.handle==commandHandle&&c.revision==before&&c.recording&&!c.unknown&&!c.busy;
        const bool warm=stable&&captured&&matches(existing,first);
        errno=incoming;api.bindPipeline(commandHandle,VK_PIPELINE_BIND_POINT_GRAPHICS,h);Errno preserve;
        // Raw C PFNs do not reenter or race mutation of this command. Both cold
        // and warm paths still check the exact adapter identity after return.
        if(!ready||!owned())return;
        if(serial!=all||c.handle!=commandHandle||c.revision!=before||!c.recording||c.unknown||c.busy){invalidate(commandHandle);return;}
        c.intended=h;c.pipelineIndex=UINT32_MAX;c.pipelineSerial=0;
        if(!stable||!captured){c.revision=next();return;}
        DrawMetadata after=first;
        if(!warm){
            if(!api.lookupDrawMetadata(api.lookupContext,h,after)||!ready||!owned()||serial!=all||c.handle!=commandHandle||c.revision!=before||
                !c.recording||c.unknown||c.busy||!sameMetadata(first,after)){invalidate(commandHandle);return;}
        }
        if(auto* p=importMetadata(h,after,index)){c.pipelineIndex=unsigned(p-pipelines.data());c.pipelineSerial=p->value.serial;}
        c.revision=next();
    }
    void recordSets(Command& c,Layout* l,VkPipelineLayout handle,unsigned first,unsigned count,const VkDescriptorSet* sets,unsigned dynamicCount,const unsigned* offsets)noexcept {
        if(!l||!l->serial||(c.layout&&(c.layout!=handle||c.layoutSerial!=l->serial))||
           !count||first>=MaxSets||count>MaxSets-first||first+count>l->count||!sets||(dynamicCount&&!offsets)){unknown(c);return;}
        unsigned total=0;for(unsigned i=0;i<count;++i){if(!sets[i]||l->dynamic[first+i]>MaxDynamic-total){unknown(c);return;}total+=l->dynamic[first+i];}
        if(dynamicCount!=total){unknown(c);return;}
        unsigned at=0;for(unsigned i=0;i<count;++i){auto& set=c.sets[first+i];set.handle=sets[i];set.count=l->dynamic[first+i];
            if(set.count)std::copy_n(offsets+at,set.count,set.offsets.begin());at+=set.count;}
        c.layout=handle;c.layoutSerial=l->serial;c.revision=next();
    }
    void bindMetadataSets(Command& c,VkPipelineLayout handle,unsigned first,unsigned count,const VkDescriptorSet* sets,unsigned n,const unsigned* offsets)noexcept {
        const int incoming=errno;const auto commandHandle=c.handle,revisionHandle=c.handle;
        const auto all=serial,before=c.revision;std::uint64_t generation{},after{};
        const bool known=api.lookupLayoutGeneration(api.lookupContext,handle,generation);
        const bool stable=ready&&owned()&&serial==all&&c.handle==revisionHandle&&c.revision==before&&c.recording&&!c.unknown&&!c.busy;
        const bool bounded=count&&count<=MaxSets&&first<MaxSets&&count<=MaxSets-first&&sets&&n<=MaxDynamic&&(!n||offsets);
        std::array<VkDescriptorSet,MaxSets> capturedSets{};std::array<unsigned,MaxDynamic> capturedOffsets{};
        if(stable&&known&&bounded){std::copy_n(sets,count,capturedSets.begin());if(n)std::copy_n(offsets,n,capturedOffsets.begin());}
        errno=incoming;api.bindDescriptors(commandHandle,VK_PIPELINE_BIND_POINT_GRAPHICS,handle,first,count,sets,n,offsets);Errno preserve;
        if(!ready||!owned())return;
        if(serial!=all||c.handle!=revisionHandle||c.revision!=before||!c.recording||c.unknown||c.busy){invalidate(commandHandle);return;}
        if(!stable||!known||!bounded||!generation||!api.lookupLayoutGeneration(api.lookupContext,handle,after)||
            !ready||!owned()||serial!=all||c.handle!=revisionHandle||c.revision!=before||!c.recording||c.unknown||c.busy||generation!=after){invalidate(commandHandle);return;}
        recordSets(c,layout(handle,generation),handle,first,count,capturedSets.data(),n,n?capturedOffsets.data():nullptr);
    }
    OwnedBindResult bindOwnedSets(Command& c,VkPipeline intended,VkPipelineLayout restore,unsigned first,unsigned count,
                                 const VkDescriptorSet* sets,unsigned n,const unsigned* offsets)noexcept {
        const int incoming=errno;const auto commandHandle=c.handle;
        const auto before=c.revision,all=serial;const auto index=c.pipelineIndex;
        auto decline=[&](){errno=incoming;return OwnedBindResult::Declined;};
        if(!intended||!restore||c.intended!=intended||index>=pipelines.size()||!count||count>MaxSets||
           first>=MaxSets||count>MaxSets-first||!sets||n>MaxDynamic||(n&&!offsets))return decline();
        const auto* p=&pipelines[index];
        if(p->handle!=intended||p->value.serial!=c.pipelineSerial||p->layoutIndex>=layouts.size())return decline();
        const auto expected=p->value;const auto layoutIndex=p->layoutIndex;const auto layoutSerial=p->layoutSerial;
        DrawMetadata beforeMetadata{};
        const bool captured=api.lookupDrawMetadata(api.lookupContext,intended,beforeMetadata);
        auto stable=[&]()noexcept{return ready&&owned()&&serial==all&&c.handle==commandHandle&&c.revision==before&&
            c.recording&&!c.unknown&&!c.busy&&c.intended==intended&&c.pipelineIndex==index&&c.pipelineSerial==expected.serial;};
        if(!stable()||!captured||!matches(&pipelines[index],beforeMetadata)||!sameVariant(expected,beforeMetadata.variant)||
           beforeMetadata.variant.restoreLayout!=restore||beforeMetadata.variant.deviceGeneration!=deviceGeneration)return decline();
        auto& l=layouts[layoutIndex];
        if(l.serial!=layoutSerial||l.handle!=expected.nativeLayout||
           (c.layout&&(c.layout!=l.handle||c.layoutSerial!=l.serial))||first+count>l.count)return decline();
        unsigned total{};for(unsigned i=0;i<count;++i){if(!sets[i]||l.dynamic[first+i]>MaxDynamic-total)return decline();total+=l.dynamic[first+i];}
        if(total!=n)return decline();
        std::array<VkDescriptorSet,MaxSets> copiedSets{};std::array<unsigned,MaxDynamic> copiedOffsets{};
        std::copy_n(sets,count,copiedSets.begin());if(n)std::copy_n(offsets,n,copiedOffsets.begin());
        errno=incoming;api.bindDescriptors(commandHandle,VK_PIPELINE_BIND_POINT_GRAPHICS,restore,first,count,sets,n,offsets);Errno preserve;
        auto lost=[&](){if(owned())invalidate(commandHandle);return OwnedBindResult::BoundUntracked;};
        if(!stable())return lost();
        DrawMetadata after{};
        if(!api.lookupDrawMetadata(api.lookupContext,intended,after)||!stable()||!sameMetadata(beforeMetadata,after)||
           !matches(&pipelines[index],after))return lost();
        // Store the historical native identity, never register the owned handle
        // as a fabricated native creation. Only an exact same-layout subset is
        // supported; general Vulkan layout compatibility is deliberately absent.
        recordSets(c,&layouts[layoutIndex],expected.nativeLayout,first,count,copiedSets.data(),n,n?copiedOffsets.data():nullptr);
        return c.unknown?OwnedBindResult::BoundUntracked:OwnedBindResult::Bound;
    }
    struct PacketPrepared {PacketTuple tuple;DrawMetadata metadata;};
    struct PacketState {
        std::array<PacketPrepared,MaxPacketParts+1> values{};
        Command* command{};VkCommandBuffer handle{};
        std::uint64_t serial{},commandRevision{},adapterSerial{},metadataRevision{};
        unsigned count{},next{};bool active{},fault{},drawing{};
        CameraUniformOwner::RecordingObservation observation;CameraUniformOwner::Binding camera;
        void* lifetime{};bool(*lifetimeCurrent)(void*)noexcept{};
    } packet;
    bool packetSequence{}; // Set only by the private owned compound entry.
    std::uint64_t packetSerial{};
    bool packetCurrent(std::uint64_t value,bool allowFault=false)noexcept {
        if(!owned())return false;
        auto& p=packet;auto* c=p.command;
        // Commands live in a fixed array. No callback may destroy this adapter.
        // The serial binds the exact plan; re-entry cannot retag it in place.
        const auto same=[&]()noexcept {
            return owned()&&ready&&p.active&&(allowFault||!p.fault)&&p.serial==value&&c&&p.command==c&&
                c->handle==p.handle&&c->revision==p.commandRevision&&serial==p.adapterSerial&&
                c->recording&&!c->unknown&&c->busy;
        };
        if(!same()||!p.lifetimeCurrent||!api.metadataRevision)return false;
        if(api.metadataRevision(api.lookupContext)!=p.metadataRevision||!same())return false;
        if(!p.lifetimeCurrent(p.lifetime)||!same()||!p.observation.current())return false;
        if(!p.lifetimeCurrent(p.lifetime)||!same())return false;
        if(api.metadataRevision(api.lookupContext)!=p.metadataRevision||!same())return false;
        return p.observation.current()&&same();
    }
    bool packetTuple(VkPipeline intended,const PacketTuple& t,DrawMetadata& m)noexcept {
        if(!intended||t.pipeline!=intended||!t.restoreLayout||(t.setMask&~127u)||
           !api.lookupDrawMetadata(api.lookupContext,intended,m))return false;
        const auto& v=m.variant;
        if(!v||(t.expectedVariantSerial&&t.expectedVariantSerial!=v.serial)||
           (t.expectedNativeLayoutGeneration&&t.expectedNativeLayoutGeneration!=m.nativeLayoutGeneration)||
           v.nativePipeline!=intended||v.restoreLayout!=t.restoreLayout||v.deviceGeneration!=deviceGeneration||
           v.cameraLayout!=cameraLayout.handle||v.cameraLayoutGeneration!=cameraLayout.generation||
           !m.nativeLayoutGeneration||m.setCount>MaxSets||m.setCount!=v.nativeSetCount||
           m.requiredNativeSets!=v.requiredNativeSets||(m.requiredNativeSets&~t.setMask)||
           (t.setMask&~((1u<<m.setCount)-1u)))return false;
        // A native bind may include its empty set 3. The captured factory
        // definition, never a descriptor handle guess, proves it is empty.
        // Owned camera overrides it for each draw; caller restores incoming
        // native sets (including empty 3) in ascending set order afterwards.
        if((t.setMask&8u)&&(m.setCount<=3||(m.requiredNativeSets&8u)||m.dynamicCounts[3]))return false;
        for(unsigned i=0;i<MaxSets;++i){const auto& d=t.descriptors[i];
            if(t.setMask&(1u<<i)){if(!d.handle||d.count>MaxPacketOffsets||d.count!=m.dynamicCounts[i])return false;}
            else if(d.handle||d.count)return false;
        }
        return true;
    }
    bool preparePacket(VkCommandBuffer h,std::span<const PacketTuple> tuples,const PacketTuple& incoming,
        const Request& request,void* lifetime,bool(*current)(void*)noexcept)noexcept {
        if(!owned()||!ready||packet.active||!current||!api.metadataRevision||!api.lookupDrawMetadata||
           !api.bindPipeline||!api.bindDescriptors||!api.drawIndexed||tuples.empty()||tuples.size()>MaxPacketParts||packetSerial==UINT64_MAX)return false;
        auto* c=command(h);if(!c||!c->recording||c->unknown||c->busy||!c->intended||c->pipelineIndex>=pipelines.size())return false;
        const auto revision=c->revision,all=serial,preparation=packetSerial;
        const auto same=[&]()noexcept{return ready&&owned()&&!packet.active&&packetSerial==preparation&&
            c->handle==h&&c->revision==revision&&serial==all&&c->recording&&!c->unknown&&!c->busy;};
        const auto metadata=api.metadataRevision(api.lookupContext);
        if(!metadata||!same()||!current(lifetime)||!same())return false;
        auto unchanged=[&]()noexcept{return same()&&api.metadataRevision(api.lookupContext)==metadata&&same()&&
            current(lifetime)&&same();};
        auto& p=packet;
        p.count=unsigned(tuples.size());p.next=0;p.fault=false;p.drawing=false;
        p.values[MaxPacketParts].tuple=incoming;
        for(unsigned i=0;i<p.count;++i)p.values[i].tuple=tuples[i];
        auto& saved=p.values[MaxPacketParts];
        if(!packetTuple(c->intended,saved.tuple,saved.metadata)||!unchanged()||
           !matches(&pipelines[c->pipelineIndex],saved.metadata)||c->pipelineSerial!=saved.metadata.variant.serial)return false;
        auto* l=&layouts[pipelines[c->pipelineIndex].layoutIndex];
        if(c->layout&&(c->layout!=l->handle||c->layoutSerial!=l->serial))return false;
        unsigned actualMask{};
        for(unsigned i=0;i<MaxSets;++i)if(c->sets[i].handle){actualMask|=1u<<i;const auto& d=saved.tuple.descriptors[i];const auto& actual=c->sets[i];
            if(d.handle!=actual.handle||d.count!=actual.count||d.count>MaxPacketOffsets||
               !std::equal(d.offsets.begin(),d.offsets.begin()+d.count,actual.offsets.begin()))return false;}
        if(actualMask!=saved.tuple.setMask)return false;
        for(unsigned i=0;i<p.count;++i){auto& part=p.values[i];
            if(!packetTuple(part.tuple.pipeline,part.tuple,part.metadata)||!unchanged())return false;
            // Initial subset: same immutable historical layout definition.
            if(part.metadata.variant.nativeLayout!=saved.metadata.variant.nativeLayout||
               part.metadata.nativeLayoutGeneration!=saved.metadata.nativeLayoutGeneration||
               part.metadata.setCount!=saved.metadata.setCount||part.metadata.dynamicCounts!=saved.metadata.dynamicCounts)return false;
        }
        CameraUniformOwner::Binding binding{};
        CameraUniformOwner::RecordingObservation observation;
        if(!camera->layoutCurrent(cameraLayout)||!request.camera.observeForRecording(binding,observation)||binding.device!=device||binding.command!=h||
           binding.layout!=cameraLayout.handle||binding.deviceGeneration!=cameraLayout.generation||binding.range!=CameraUniformOwner::PayloadBytes||
           binding.key!=request.expected||!unchanged()||!observation.current())return false;
        p.command=c;p.handle=h;p.commandRevision=revision;p.adapterSerial=all;p.metadataRevision=metadata;p.observation=observation;p.camera=binding;
        p.lifetime=lifetime;p.lifetimeCurrent=current;p.serial=++packetSerial;p.active=true;c->busy=true;
        if(!packetCurrent(p.serial)){p.active=false;c->busy=false;return false;}return true;
    }
    bool packetDraw(std::uint64_t serialValue,unsigned part,unsigned count,unsigned instances,unsigned first,int vertex,unsigned base)noexcept {
        const int incoming=errno;auto& p=packet;
        if(!packetCurrent(serialValue)||p.drawing||part!=p.next||part>=p.count){if(p.active&&p.serial==serialValue)p.fault=true;errno=incoming;return false;}
        p.drawing=true;const auto& value=p.values[part];const auto& tuple=value.tuple;const auto& variant=value.metadata.variant;
        bool drew=false;int result=incoming;
        api.bindPipeline(p.handle,VK_PIPELINE_BIND_POINT_GRAPHICS,variant.pipeline);
        if(packetCurrent(serialValue))api.bindDescriptors(p.handle,VK_PIPELINE_BIND_POINT_GRAPHICS,variant.layout,3,1,&p.camera.descriptor,0,nullptr);
        for(unsigned i=4;i<MaxSets&&packetCurrent(serialValue);++i)if(tuple.setMask&(1u<<i)){const auto& d=tuple.descriptors[i];
            api.bindDescriptors(p.handle,VK_PIPELINE_BIND_POINT_GRAPHICS,variant.layout,i,1,&d.handle,d.count,d.count?d.offsets.data():nullptr);}
        if(packetCurrent(serialValue)){errno=incoming;api.drawIndexed(p.handle,count,instances,first,vertex,base);result=errno;drew=true;}
        if(!packetCurrent(serialValue))p.fault=true;
        // A fault latch alone permits best-effort restore, but a lost command,
        // MAIN scope, or creation incarnation does not. Never use a possibly
        // freed/re-recorded native handle just to attempt restoration.
        if(!packetSequence&&packetCurrent(serialValue,true))api.bindPipeline(p.handle,VK_PIPELINE_BIND_POINT_GRAPHICS,tuple.pipeline);
        for(unsigned i=4;i<MaxSets&&!packetSequence&&packetCurrent(serialValue,true);++i)if(tuple.setMask&(1u<<i)){const auto& d=tuple.descriptors[i];
            api.bindDescriptors(p.handle,VK_PIPELINE_BIND_POINT_GRAPHICS,tuple.restoreLayout,i,1,&d.handle,d.count,d.count?d.offsets.data():nullptr);}
        p.drawing=false;
        if(!packetCurrent(serialValue))p.fault=true;
        if(drew&&!p.fault)++p.next;errno=result;return drew&&!p.fault;
    }

    bool preflight(VkCommandBuffer handle,const Request* request,Plan& plan)noexcept {
        if(!owner()||!request||!ready)return false;
        auto* c=command(handle);if(!c||!c->recording||c->unknown||c->busy||!c->intended||!c->revision)return false;
        const auto revision=c->revision;const auto intended=c->intended;
        const auto index=c->pipelineIndex;if(index>=pipelines.size())return false;
        auto* p=&pipelines[index];if(p->handle!=intended||p->value.serial!=c->pipelineSerial)return false;
        const auto expected=p->value;const auto layoutSerial=p->layoutSerial;const auto layoutIndex=p->layoutIndex;
        Variant actual{};DrawMetadata metadata{};
        if(api.lookupDrawMetadata){if(!api.lookupDrawMetadata(api.lookupContext,intended,metadata))return false;actual=metadata.variant;}
        else if(!api.lookup(api.lookupContext,intended,actual))return false;
        // Lookup is the only external callback. Revalidate exact command and
        // both creation incarnations after it; no callback can retag a plan.
        if(!ready||!owned()||c->revision!=revision||!c->recording||c->unknown||c->busy||c->intended!=intended)return false;
        if(layoutIndex>=layouts.size())return false;p=&pipelines[index];auto* l=&layouts[layoutIndex];
        if(p->handle!=intended||p->value.serial!=expected.serial||p->layoutSerial!=layoutSerial||p->layoutIndex!=layoutIndex||
           l->handle!=expected.nativeLayout||!layoutSerial||l->serial!=layoutSerial||
           actual.serial!=expected.serial||actual.pipeline!=expected.pipeline||actual.layout!=expected.layout||
           actual.restoreLayout!=expected.restoreLayout||!actual.restoreLayout||
           actual.nativePipeline!=intended||actual.nativeLayout!=expected.nativeLayout||
           actual.deviceGeneration!=deviceGeneration||actual.cameraLayout!=cameraLayout.handle||
           actual.cameraLayoutGeneration!=cameraLayout.generation||actual.nativeSetCount!=l->count||
           actual.requiredNativeSets!=l->required||!actual.pipeline||!actual.layout)return false;
        if(api.lookupDrawMetadata&&!metadataMatches(metadata,*l))return false;
        // Even with no required native sets, a previously bound suffix must
        // have this exact layout before it can be restored without disturbance.
        if(c->layout&&(c->layout!=l->handle||c->layoutSerial!=l->serial))return false;
        for(unsigned i=0;i<MaxSets;++i) {
            if((l->required&(1u<<i))&&!c->sets[i].handle)return false;
            if(c->sets[i].handle&&(i>=l->count||c->sets[i].count!=l->dynamic[i]))return false;
        }
        CameraUniformOwner::Binding b{};
        if(!camera->layoutCurrent(cameraLayout)||!request->camera.resolveForRecording(b)||
           b.device!=device||b.command!=handle||b.layout!=cameraLayout.handle||
           b.deviceGeneration!=cameraLayout.generation||b.range!=CameraUniformOwner::PayloadBytes||
           b.key!=request->expected)return false;
        plan={c,actual,b};return true;
    }
    void suffix(const Plan& p,VkPipelineLayout layout)noexcept {
        // Ascending indices: rebinding a lower incompatible prefix can disturb
        // the higher ones. Include every captured suffix, not only shader-used.
        for(unsigned i=4;i<MaxSets;++i){const auto& s=p.command->sets[i];if(s.handle)
            api.bindDescriptors(p.command->handle,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,i,1,&s.handle,s.count,s.count?s.offsets.data():nullptr);}
    }
    void enter(Plan& p)noexcept {
        p.command->busy=true;
        api.bindPipeline(p.command->handle,VK_PIPELINE_BIND_POINT_GRAPHICS,p.variant.pipeline);
        api.bindDescriptors(p.command->handle,VK_PIPELINE_BIND_POINT_GRAPHICS,p.variant.layout,3,1,&p.camera.descriptor,0,nullptr);
        suffix(p,p.variant.layout);
    }
    void leave(Plan& p)noexcept {
        api.bindPipeline(p.command->handle,VK_PIPELINE_BIND_POINT_GRAPHICS,p.variant.nativePipeline);
        // Native pipeline layouts may legally be destroyed after creation.
        // Only factory-owned retained compatible layout is used for restores.
        suffix(p,p.variant.restoreLayout);p.command->busy=false;
    }
};
CameraDrawAdapter::CameraDrawAdapter():impl_(std::make_unique<Impl>()){}
CameraDrawAdapter::~CameraDrawAdapter()=default;
bool CameraDrawAdapter::bindOriginals(Api api)noexcept {
    Errno preserve;auto& s=*impl_;
    if(!api.bindPipeline||!api.bindDescriptors||!api.draw||!api.drawIndexed||(!api.lookup&&!api.lookupDrawMetadata)||
       (api.lookupDrawMetadata&&!api.lookupLayoutGeneration))return false;
    if(s.backed)return s.api.bindPipeline==api.bindPipeline&&s.api.bindDescriptors==api.bindDescriptors&&
        s.api.draw==api.draw&&s.api.drawIndexed==api.drawIndexed&&s.api.lookupContext==api.lookupContext&&s.api.lookup==api.lookup&&
        s.api.lookupDrawMetadata==api.lookupDrawMetadata&&s.api.lookupLayoutGeneration==api.lookupLayoutGeneration&&s.api.metadataRevision==api.metadataRevision;
    s.api=api;s.backed=true;return true;
}
bool CameraDrawAdapter::initialize(VkDevice d,std::uint64_t generation,Api api,CameraUniformOwner& owner)noexcept {
    Errno preserve;auto& s=*impl_;CameraUniformOwner::Layout layout;
    if(s.initialized||!d||!generation||!bindOriginals(api)||
       !owner.layout(layout)||layout.device!=d)return false;
    try{s.pipelineIndex.reserve(s.pipelines.size());s.metadataLayoutIndex.reserve(s.layouts.size());}catch(...){return false;}
    s.device=d;s.deviceGeneration=generation;s.api=api;s.camera=&owner;s.cameraLayout=layout;
    s.thread=std::this_thread::get_id();s.initialized=s.ready=true;return true;
}
bool CameraDrawAdapter::layoutCreated(VkPipelineLayout h,const VkPipelineLayoutCreateInfo& info,std::span<const VkDescriptorSetLayoutCreateInfo> sets)noexcept {
    Errno preserve;auto& s=*impl_;if(!s.ready||!s.owner()||!h||s.api.lookupDrawMetadata)return false;
    if(auto* old=s.layout(h)){old->serial=0;return false;} // duplicate live handle cannot prove an incarnation
    if(info.sType!=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO||info.pNext||info.flags||info.setLayoutCount>MaxSets||
       info.setLayoutCount!=sets.size()||(info.setLayoutCount&&!info.pSetLayouts))return false;
    Impl::Layout value{};value.handle=h;value.count=info.setLayoutCount;
    for(unsigned i=0;i<sets.size();++i){const auto& set=sets[i];
        if(!info.pSetLayouts[i]||set.sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO||set.pNext||
           (set.flags&~VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT)||set.bindingCount>1024||
           (set.bindingCount&&!set.pBindings)|| (i==3&&(set.bindingCount||set.flags)))return false;
        if(set.bindingCount)value.required|=1u<<i;
        for(unsigned j=0;j<set.bindingCount;++j){const auto& b=set.pBindings[j];if(!b.descriptorCount)return false;
            for(unsigned k=0;k<j;++k)if(set.pBindings[k].binding==b.binding)return false;
            if(b.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||b.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC){
                if(b.descriptorCount>MaxDynamic-value.dynamic[i])return false;value.dynamic[i]+=b.descriptorCount;}}
    }
    auto* target=Impl::find(s.layouts,VK_NULL_HANDLE);if(!target)return false;
    value.serial=s.next();if(!value.serial)return false;*target=value;return true;
}
void CameraDrawAdapter::layoutDestroyed(VkPipelineLayout h)noexcept {Errno preserve;auto& s=*impl_;if(s.api.lookupDrawMetadata)return;if(s.owner())if(auto* l=s.layout(h)){*l={};s.next();}}
bool CameraDrawAdapter::pipelineCreated(VkPipeline h)noexcept {
    Errno preserve;auto& s=*impl_;if(!s.ready||!s.owner()||!h||s.api.lookupDrawMetadata)return false;
    if(auto* old=s.pipeline(h)){old->layoutSerial=0;return false;}
    const auto revision=s.serial;
    Impl::Variant v{};if(!s.api.lookup(s.api.lookupContext,h,v)||!s.ready||!s.owner()||s.serial!=revision)return false;
    auto* l=s.layout(v.nativeLayout);if(!l||!l->serial||v.nativePipeline!=h||v.deviceGeneration!=s.deviceGeneration||
       v.cameraLayout!=s.cameraLayout.handle||v.cameraLayoutGeneration!=s.cameraLayout.generation||
       v.nativeSetCount!=l->count||v.requiredNativeSets!=l->required||!v||!v.restoreLayout)return false;
    auto* p=Impl::find(s.pipelines,VK_NULL_HANDLE);if(!p)return false;
    try{s.pipelineIndex.emplace(h,unsigned(p-s.pipelines.data()));}catch(...){return false;}
    if(!s.next())return false;*p={h,v,l->serial,unsigned(l-s.layouts.data())};return true;
}
void CameraDrawAdapter::pipelineDestroyed(VkPipeline h)noexcept {Errno preserve;auto& s=*impl_;if(s.owner())if(auto* p=s.pipeline(h)){s.pipelineIndex.erase(h);*p={};s.next();}}
void CameraDrawAdapter::commandAllocated(VkDevice d,VkCommandPool p,VkCommandBuffer h,VkCommandBufferLevel level,VkResult result)noexcept {
    Errno preserve;auto& s=*impl_;if(!s.ready||!s.owner()||d!=s.device||result!=VK_SUCCESS||!p||!h)return;
    if(auto* old=s.command(h)){s.unknown(*old);return;}auto* c=Impl::find(s.commands,VK_NULL_HANDLE);if(!c)return;
    c->handle=h;c->pool=p;c->level=level;c->revision=s.next();
}
void CameraDrawAdapter::commandBegun(VkCommandBuffer h,VkResult result)noexcept {
    Errno preserve;auto& s=*impl_;if(!s.ready||!s.owner())return;if(auto* c=s.command(h)){
        if(result==VK_SUCCESS){s.clear(*c);c->recording=c->level==VK_COMMAND_BUFFER_LEVEL_PRIMARY;}else {c->recording=false;s.unknown(*c);}}
}
void CameraDrawAdapter::commandEnded(VkCommandBuffer h,VkResult)noexcept {Errno preserve;auto& s=*impl_;if(s.owner())if(auto* c=s.command(h)){c->recording=false;s.unknown(*c);}}
void CameraDrawAdapter::commandReset(VkCommandBuffer h,VkResult result)noexcept {Errno preserve;auto& s=*impl_;if(s.owner())if(auto* c=s.command(h)){if(result==VK_SUCCESS)s.clear(*c);else {c->recording=false;s.unknown(*c);}}}
void CameraDrawAdapter::commandFreed(VkDevice d,VkCommandPool p,VkCommandBuffer h)noexcept {Errno preserve;auto& s=*impl_;if(s.owner()&&d==s.device)if(auto* c=s.command(h);c&&c->pool==p)*c={};}
void CameraDrawAdapter::poolReset(VkDevice d,VkCommandPool p,VkResult result)noexcept {Errno preserve;auto& s=*impl_;if(s.owner()&&d==s.device)for(auto& c:s.commands)if(c.handle&&c.pool==p){if(result==VK_SUCCESS)s.clear(c);else {c.recording=false;s.unknown(c);}}}
void CameraDrawAdapter::poolDestroyed(VkDevice d,VkCommandPool p)noexcept {Errno preserve;auto& s=*impl_;if(s.owner()&&d==s.device)for(auto& c:s.commands)if(c.handle&&c.pool==p)c={};}
void CameraDrawAdapter::commandUnknown(VkCommandBuffer h)noexcept {Errno preserve;auto& s=*impl_;if(s.owner())if(auto* c=s.command(h))s.unknown(*c);}
void CameraDrawAdapter::disarm()noexcept {Errno preserve;auto& s=*impl_;s.ready=false;}
void CameraDrawAdapter::bindPipeline(VkCommandBuffer h,VkPipelineBindPoint point,VkPipeline pipeline)noexcept {
    const int incoming=errno;auto& s=*impl_;if(!s.api.bindPipeline)return;
    if(s.api.lookupDrawMetadata&&s.ready&&s.owner()&&point==VK_PIPELINE_BIND_POINT_GRAPHICS)
        if(auto* c=s.command(h);c&&c->recording&&!c->unknown&&!c->busy){errno=incoming;s.bindMetadata(*c,pipeline);return;}
    errno=incoming;s.api.bindPipeline(h,point,pipeline);Errno preserve;
    if(!s.ready||!s.owner()||point!=VK_PIPELINE_BIND_POINT_GRAPHICS)return;
    if(auto* c=s.command(h);c&&c->recording){
        if(s.api.lookupDrawMetadata){
            c->intended=pipeline;c->pipelineIndex=UINT32_MAX;c->pipelineSerial=0;c->revision=s.next();
            return;
        }
        if(c->intended!=pipeline||c->pipelineIndex>=s.pipelines.size()||s.pipelines[c->pipelineIndex].handle!=pipeline||
           s.pipelines[c->pipelineIndex].value.serial!=c->pipelineSerial){
            auto* p=s.pipeline(pipeline);c->pipelineIndex=p?unsigned(p-s.pipelines.data()):UINT32_MAX;c->pipelineSerial=p?p->value.serial:0;
        }
        c->intended=pipeline;c->revision=s.next();}
}
void CameraDrawAdapter::bindDescriptors(VkCommandBuffer h,VkPipelineBindPoint point,VkPipelineLayout layout,std::uint32_t first,std::uint32_t count,const VkDescriptorSet* sets,std::uint32_t dynamicCount,const std::uint32_t* offsets)noexcept {
    const int incoming=errno;auto& s=*impl_;if(!s.api.bindDescriptors)return;
    if(s.api.lookupDrawMetadata&&s.ready&&s.owner()&&point==VK_PIPELINE_BIND_POINT_GRAPHICS)
        if(auto* c=s.command(h);c&&c->recording&&!c->unknown&&!c->busy){errno=incoming;s.bindMetadataSets(*c,layout,first,count,sets,dynamicCount,offsets);return;}
    errno=incoming;s.api.bindDescriptors(h,point,layout,first,count,sets,dynamicCount,offsets);Errno preserve;
    if(!s.ready||!s.owner()||point!=VK_PIPELINE_BIND_POINT_GRAPHICS)return;
    auto* c=s.command(h);if(!c||!c->recording||c->unknown)return;
    s.recordSets(*c,s.layout(layout),layout,first,count,sets,dynamicCount,offsets);
}
CameraDrawAdapter::OwnedBindResult CameraDrawAdapter::bindOwnedDescriptors(VkCommandBuffer h,VkPipeline intended,VkPipelineLayout restore,
        std::uint32_t first,std::uint32_t count,const VkDescriptorSet* sets,std::uint32_t n,const std::uint32_t* offsets)noexcept {
    Errno preserve;auto& s=*impl_;
    if(!s.ready||!s.owned()||!s.api.lookupDrawMetadata)return OwnedBindResult::Declined;
    auto* c=s.command(h);if(!c||!c->recording||c->unknown||c->busy)return OwnedBindResult::Declined;
    const auto result=s.bindOwnedSets(*c,intended,restore,first,count,sets,n,offsets);
    preserve.value=errno;return result;
}
bool CameraDrawAdapter::IndexedPacketPlan::current()const noexcept {
    Errno preserve;return owner_&&owner_->impl_->packetCurrent(serial_)&&!owner_->impl_->packet.drawing;
}
void CameraDrawAdapter::IndexedPacketPlan::markFault()noexcept {
    Errno preserve;if(owner_&&owner_->impl_->owned()&&owner_->impl_->packet.active&&owner_->impl_->packet.serial==serial_)owner_->impl_->packet.fault=true;
}
bool CameraDrawAdapter::IndexedPacketPlan::drawIndexed(unsigned part,std::uint32_t count,std::uint32_t instances,std::uint32_t first,std::int32_t vertex,std::uint32_t base)noexcept {
    return owner_&&owner_->impl_->packetDraw(serial_,part,count,instances,first,vertex,base);
}
CameraDrawAdapter::PacketResult CameraDrawAdapter::withIndexedPacketPlan(VkCommandBuffer h,std::span<const PacketTuple> parts,const PacketTuple& incoming,
    const Request& request,void* lifetime,bool(*current)(void*)noexcept,void* context,bool(*emit)(void*,IndexedPacketPlan&)noexcept)noexcept {
    return withIndexedPlanImpl(h,parts,incoming,request,lifetime,current,context,emit,false);
}
CameraDrawAdapter::PacketResult CameraDrawAdapter::withIndexedPlanImpl(VkCommandBuffer h,std::span<const PacketTuple> parts,const PacketTuple& incoming,
    const Request& request,void* lifetime,bool(*current)(void*)noexcept,void* context,bool(*emit)(void*,IndexedPacketPlan&)noexcept,bool sequence)noexcept {
    Errno preserve;auto&s=*impl_;if(!emit||!s.preparePacket(h,parts,incoming,request,lifetime,current))return PacketResult::Declined;
    s.packetSequence=sequence;
    IndexedPacketPlan plan(this,s.packet.serial);const bool accepted=emit(context,plan);
    const bool completed=accepted&&s.packet.next==s.packet.count&&plan.current();
    auto* command=s.packet.command;s.packet.active=false;s.packet.lifetime=nullptr;s.packet.lifetimeCurrent=nullptr;command->busy=false;
    if(!completed)s.unknown(*command);
    return completed?PacketResult::Completed:PacketResult::EmittedFaulted;
}

bool CameraDrawAdapter::draw(VkCommandBuffer h,std::uint32_t n,std::uint32_t instances,std::uint32_t first,std::uint32_t base,const Request* request)noexcept {
    auto& s=*impl_;if(!s.api.draw)return false;const int incoming=errno;Impl::Plan p;const bool use=s.preflight(h,request,p);
    if(use)s.enter(p);errno=incoming;s.api.draw(h,n,instances,first,base);const int result=errno;if(use)s.leave(p);errno=result;return use;
}
bool CameraDrawAdapter::drawIndexed(VkCommandBuffer h,std::uint32_t n,std::uint32_t instances,std::uint32_t first,std::int32_t vertex,std::uint32_t base,const Request* request)noexcept {
    auto& s=*impl_;if(!s.api.drawIndexed)return false;const int incoming=errno;Impl::Plan p;const bool use=s.preflight(h,request,p);
    if(use)s.enter(p);errno=incoming;s.api.drawIndexed(h,n,instances,first,vertex,base);const int result=errno;if(use)s.leave(p);errno=result;return use;
}
} // namespace rdr95
