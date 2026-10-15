// Included inside rdrvr::engine after private record definitions.
bool IndexedCommandState::importReplayMetadata(const FrozenCameraCreationRegistry::ReplayMetadata& in)noexcept {
    Errno keep;
    if(!coldAccess()||failed_.load(std::memory_order_acquire))return false;
    const auto& v=in.draw.variant;
    if(!v||!v.nativePipeline||!v.nativeLayout||!v.cameraLayout||!v.cameraLayoutGeneration||v.deviceGeneration!=deviceGeneration_||!in.draw.nativeLayoutGeneration||
       v.nativeLayout==v.restoreLayout||in.flags||!in.renderPass||
       !(in.stages&VK_SHADER_STAGE_VERTEX_BIT)||(in.stages&~stages)||
       in.setCount>maxSets||in.setCount!=in.draw.setCount||in.setCount!=v.nativeSetCount||
       in.pushCount>2||in.dynamicCount>in.dynamics.size()||(in.vertexBindingMask&~((1u<<maxBindings)-1)))return false;
    // A copied old export cannot resurrect a destruction already observed by
    // this mirror. Actual recreation must bring a strictly newer variant.
    for(unsigned i=0;i<cold_->pipelineCount;++i){const auto& old=cold_->pipelines[i];
        if(old.handle==v.nativePipeline&&old.variant.serial&&
           (old.variant.serial>v.serial||(old.variant.serial==v.serial&&!old.alive)))return false;
    }
    auto n=std::unique_ptr<Layout>(new(std::nothrow) Layout);
    if(!n)return false;
    n->handle=v.nativeLayout;n->externalGeneration=in.draw.nativeLayoutGeneration;
    n->origin=Layout::Origin::HistoricalNative;n->alive=false;n->count=in.setCount;n->pushCount=in.pushCount;
    unsigned usedStages=0;
    for(unsigned i=0;i<in.pushCount;++i){const auto& p=in.pushes[i];
        if(!p.stageFlags||(p.stageFlags&~stages)||(usedStages&p.stageFlags)||!p.size||(p.offset%4)||(p.size%4)||p.offset>=maxPushBytes||p.size>maxPushBytes-p.offset)return false;
        usedStages|=p.stageFlags;n->pushes[i]=p;
    }
    std::sort(n->pushes.begin(),n->pushes.begin()+n->pushCount,[](const auto& a,const auto& b){return a.stageFlags<b.stageFlags;});
    for(unsigned i=0;i<in.setCount;++i){const auto& source=in.sets[i];auto& d=n->sets[i];
        if(!source.native||!source.generation||source.flags||source.count>d.bindings.size())return false;
        n->nativeSets[i]=source.native;n->nativeSetGenerations[i]=source.generation;d.count=source.count;d.flags=source.flags;
        for(unsigned j=0;j<source.count;++j){const auto& a=source.bindings[j];auto& b=d.bindings[j];
            if(!a.count||(a.flags&~VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT)||(!source.hasBindingFlags&&a.flags)||
               (j&&source.bindings[j-1].binding>=a.binding))return false;
            b.binding=a.binding;b.count=a.count;b.type=a.type;b.stages=a.stages;b.flags=a.flags;
            if(a.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||a.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC){
                if(a.count>maxOffsets-d.dynamic)return false;d.dynamic+=a.count;
            }
        }
        if(d.dynamic!=in.draw.dynamicCounts[i])return false;
    }
    if(n->required()!=in.draw.requiredNativeSets||n->required()!=v.requiredNativeSets)return false;
    for(unsigned i=in.setCount;i<maxSets;++i)if(in.draw.dynamicCounts[i])return false;
    Pipeline p{};p.handle=v.nativePipeline;p.pass=in.renderPass;p.subpass=in.subpass;p.vertices=in.vertexBindingMask;p.variant=v;
    for(unsigned i=0;i<in.dynamicCount;++i){const auto bits=dynamic(in.dynamics[i]);if(!bits||(p.dynamic&bits))return false;p.dynamic|=bits;}
    const auto sameVariant=[](const FrozenCameraPipelineFactory::Variant& a,const FrozenCameraPipelineFactory::Variant& b)noexcept {
        return a.nativePipeline==b.nativePipeline&&a.pipeline==b.pipeline&&a.layout==b.layout&&a.cameraLayout==b.cameraLayout&&
            a.deviceGeneration==b.deviceGeneration&&a.cameraLayoutGeneration==b.cameraLayoutGeneration&&a.serial==b.serial&&
            a.nativeLayout==b.nativeLayout&&a.nativeSetCount==b.nativeSetCount&&a.requiredNativeSets==b.requiredNativeSets&&a.restoreLayout==b.restoreLayout;
    };
    const auto sameNative=[&](const Layout& l)noexcept{return l.origin==Layout::Origin::HistoricalNative&&l.handle==n->handle&&
        l.externalGeneration==n->externalGeneration&&l.nativeSets==n->nativeSets&&l.nativeSetGenerations==n->nativeSetGenerations&&l.same(*n);};
    if(const auto* old=pipeline(p.handle);old&&old->variant.serial==v.serial){
        return sameVariant(old->variant,v)&&sameNative(*old->native)&&old->restore->alive&&old->restore->handle==v.restoreLayout&&
            old->restore->origin==Layout::Origin::OwnedRestore&&old->restore->externalGeneration==v.serial&&old->restore->same(*n)&&
            old->pass==p.pass&&old->subpass==p.subpass&&old->vertices==p.vertices&&old->dynamic==p.dynamic;
    }
    const Layout* native=historicalLayout(n->handle,n->externalGeneration);
    if(native&&!sameNative(*native))return false;
    const Layout* restore=nullptr;
    for(unsigned i=0;i<cold_->layoutCount;++i){const auto& l=*cold_->layouts[i];
        if(l.origin==Layout::Origin::OwnedRestore&&l.handle==v.restoreLayout){
            if(!l.alive||l.externalGeneration!=v.serial||!l.same(*n))return false;restore=&l;
        }
    }
    const unsigned added=(native?0u:1u)+(restore?0u:1u);
    if(cold_->pipelineCount==cold_->pipelines.size()||added>cold_->layouts.size()-cold_->layoutCount||cold_->serial>UINT64_MAX-added-1)return false;
    std::unique_ptr<Layout> owned;
    if(!restore){owned.reset(new(std::nothrow) Layout(*n));if(!owned)return false;
        owned->handle=v.restoreLayout;owned->origin=Layout::Origin::OwnedRestore;owned->externalGeneration=v.serial;owned->alive=true;
    }
    // All validation and allocations finish before the first publication. None
    // of these historical copies is inserted into the native-creation lookup.
    if(!native){n->serial=++cold_->serial;native=n.get();cold_->layouts[cold_->layoutCount++]=std::move(n);}
    if(!restore){owned->serial=++cold_->serial;restore=owned.get();cold_->layouts[cold_->layoutCount++]=std::move(owned);}
    for(unsigned i=0;i<cold_->pipelineCount;++i)if(cold_->pipelines[i].handle==p.handle)cold_->pipelines[i].alive=false;
    p.native=native;p.restore=restore;p.serial=++cold_->serial;p.alive=true;cold_->pipelines[cold_->pipelineCount++]=p;
    return true;
}
