#include "DynamicUniformDescriptors.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <sstream>
#include <ctime>
#include <unordered_map>
#include <vector>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace rdrvr::engine {
namespace {
template<class T>T read(const void* p,std::size_t n=0)noexcept{T v{};std::memcpy(&v,static_cast<const unsigned char*>(p)+n,sizeof v);return v;}
struct Binding {std::uint32_t slot{};bool partial{};VkShaderStageFlags stages{};};
// cpu02: slotMask/partialMask = bindings[0,count) as bits (slots are unique and sorted, so binding i is the i-th set bit).
struct Layout {std::uint64_t generation{};VkDescriptorSetLayout handle{};unsigned count{};std::uint16_t slotMask{},partialMask{};std::array<Binding,16> bindings{};};
struct Key {
    std::uint64_t generation{},bufferGeneration{};VkBuffer buffer{};std::array<std::uint32_t,16> ranges{};
    bool operator==(const Key& b)const noexcept{return generation==b.generation&&bufferGeneration==b.bufferGeneration&&buffer==b.buffer&&ranges==b.ranges;}
};
struct KeyHash {std::size_t operator()(const Key& k)const noexcept{auto h=std::uint64_t(reinterpret_cast<std::uintptr_t>(k.buffer))^(k.generation*0x9e3779b97f4a7c15ull)^(k.bufferGeneration*0xd6e8feb86659fd93ull);
        // Hash every range without a serial multiply dependency per slot.
        // Full Key equality remains the descriptor-reuse admission condition.
        for(unsigned i=0;i<8;++i){std::uint64_t word{};std::memcpy(&word,k.ranges.data()+i*2,sizeof word);
            const auto shift=i*7;h^=shift?((word<<shift)|(word>>(64-shift))):word;}
        h^=h>>30;h*=0xbf58476d1ce4e5b9ull;h^=h>>27;h*=0x94d049bb133111ebull;return std::size_t(h^(h>>31));}};
// raw is the layout every reader uses. layout owns it in the normal mode; in fast mode (debug.rdrvr.dud.fast)
// layout stays empty and the thread's FastThread pins keep it alive (no refcount atomics per draw).
// cpu02: count = raw->count, copied so bind does not read the Layout heap object (dud.lean bit 1).
struct Pending {VkDescriptorSet set{};std::shared_ptr<const Layout> layout;const Layout* raw{};std::array<std::uint32_t,16> offsets{},ranges{};std::uint32_t matrixRange{},matrixOffset{};std::uint8_t count{};};
// Allocation-free open-addressing tables (linear probing, power-of-two
// capacity, growth only by doubling). A clear is a generation bump: entries that
// carry an older generation read as empty, so forget() walks nothing.
// The per-command tables are touched only by the thread recording that command
// (Vulkan external synchronization); OwnedSets is shared under Impl::mutex.
inline std::size_t setHash(VkDescriptorSet s)noexcept{auto h=std::uint64_t(reinterpret_cast<std::uintptr_t>(s));h^=h>>17;h*=0x9e3779b97f4a7c15ull;return std::size_t(h>>24);}
struct SetTable {
    struct Entry {VkDescriptorSet set{};std::uint32_t gen{};};
    std::vector<Entry> slots;std::uint32_t gen{1},count{};
    // cpu02 (dud.lean bit 4): one bit per (hash>>12)&4095 of every set inserted since the last clear, so a clear bit
    // proves "not owned" without touching the slot array (no false negatives; kept up to date even while unused).
    std::array<std::uint64_t,64> filter{};
    static unsigned filterBit(std::size_t h)noexcept{return unsigned(h>>12)&4095u;}
    void clear()noexcept{count=0;filter={};if(++gen==0){for(auto& e:slots)e.gen=0;gen=1;}}
    bool contains(VkDescriptorSet s,bool filtered)const noexcept{
        if(!count)return false;const auto h=setHash(s);
        if(filtered){const auto b=filterBit(h);if(!((filter[b>>6]>>(b&63))&1u))return false;}
        const auto mask=slots.size()-1;
        for(auto i=h&mask;;i=(i+1)&mask){const auto& e=slots[i];if(e.gen!=gen)return false;if(e.set==s)return true;}
    }
    void insert(VkDescriptorSet s){
        if(std::size_t(count+1)*2>slots.size())grow();const auto mask=slots.size()-1;
        {const auto b=filterBit(setHash(s));filter[b>>6]|=std::uint64_t(1)<<(b&63);}
        for(auto i=setHash(s)&mask;;i=(i+1)&mask){auto& e=slots[i];if(e.gen!=gen){e={s,gen};++count;return;}if(e.set==s)return;}
    }
    void grow(){
        std::vector<Entry> next(slots.empty()?2048:slots.size()*2);const auto mask=next.size()-1;
        for(const auto& e:slots)if(e.gen==gen){auto i=setHash(e.set)&mask;while(next[i].gen==gen)i=(i+1)&mask;next[i]=e;}
        slots.swap(next);
    }
};
struct KeyTable {
    struct Entry {std::uint64_t hash{};std::uint32_t gen{};VkDescriptorSet set{};Key key{};};
    std::vector<Entry> slots;std::uint32_t gen{1},count{};
    void clear()noexcept{count=0;if(++gen==0){for(auto& e:slots)e.gen=0;gen=1;}}
    bool empty()const noexcept{return !count;}
    std::size_t size()const noexcept{return count;}
    // Full Key equality remains the descriptor-reuse admission condition.
    VkDescriptorSet find(const Key& k,std::uint64_t h)const noexcept{
        if(!count)return {};const auto mask=slots.size()-1;
        for(auto i=std::size_t(h)&mask;;i=(i+1)&mask){const auto& e=slots[i];if(e.gen!=gen)return {};if(e.hash==h&&e.key==k)return e.set;}
    }
    void insert(const Key& k,std::uint64_t h,VkDescriptorSet s){
        if(std::size_t(count+1)*2>slots.size())grow();const auto mask=slots.size()-1;
        for(auto i=std::size_t(h)&mask;;i=(i+1)&mask){auto& e=slots[i];
            if(e.gen!=gen){e.hash=h;e.gen=gen;e.set=s;e.key=k;++count;return;}if(e.hash==h&&e.key==k)return;}
    }
    void grow(){
        std::vector<Entry> next(slots.empty()?1024:slots.size()*2);const auto mask=next.size()-1;
        for(const auto& e:slots)if(e.gen==gen){auto i=std::size_t(e.hash)&mask;while(next[i].gen==gen)i=(i+1)&mask;next[i]=e;}
        slots.swap(next);
    }
};
// Every owned set of every command, for the foreign-bind check (anyOwned). An
// entry is live while its owner stamp is unchanged; forget() retires a whole
// command in O(1) by bumping that stamp. Retired entries keep probe chains intact
// until a rebuild drops them (triggered at 50% slot usage).
struct OwnedSets {
    struct Entry {VkDescriptorSet set{};std::uint32_t owner{},stamp{};};
    std::vector<Entry> slots;std::size_t used{};std::vector<std::uint32_t> stamps,freeOwners;
    std::uint32_t acquire(){
        if(!freeOwners.empty()){const auto i=freeOwners.back();freeOwners.pop_back();return i;}
        stamps.push_back(1);freeOwners.reserve(stamps.size());return std::uint32_t(stamps.size()-1);
    }
    void retire(std::uint32_t owner)noexcept{if(owner<stamps.size())++stamps[owner];}
    void release(std::uint32_t owner)noexcept{retire(owner);if(owner<stamps.size()&&freeOwners.size()<freeOwners.capacity())freeOwners.push_back(owner);}
    void clear()noexcept{slots.clear();stamps.clear();freeOwners.clear();used=0;}
    bool live(const Entry& e)const noexcept{return e.set&&e.owner<stamps.size()&&stamps[e.owner]==e.stamp;}
    bool contains(VkDescriptorSet s)const noexcept{
        if(!used)return false;const auto mask=slots.size()-1;
        for(auto i=setHash(s)&mask;;i=(i+1)&mask){const auto& e=slots[i];if(!e.set)return false;if(e.set==s)return live(e);}
    }
    void insert(VkDescriptorSet s,std::uint32_t owner){
        if(owner>=stamps.size())return;
        if((used+1)*2>slots.size())rebuild();const auto mask=slots.size()-1;
        for(auto i=setHash(s)&mask;;i=(i+1)&mask){auto& e=slots[i];
            if(!e.set){e={s,owner,stamps[owner]};++used;return;}
            if(!live(e)){e={s,owner,stamps[owner]};return;}
            if(e.set==s)return;}
    }
    void rebuild(){
        std::size_t alive{};for(const auto& e:slots)if(live(e))++alive;
        std::size_t n=1024;while(n<alive*4)n*=2;
        std::vector<Entry> next(n);const auto mask=n-1;
        for(const auto& e:slots)if(live(e)){auto i=setHash(e.set)&mask;while(next[i].set)i=(i+1)&mask;next[i]=e;}
        slots.swap(next);used=alive;
    }
};
struct Command {
    VkDevice device{};VkCommandPool commandPool{};VkCommandBuffer command{};
    std::vector<VkDescriptorPool> pools;std::size_t poolIndex{};unsigned usedInPool{};bool active{},recording{},needsDescriptorBind{true};std::uint64_t generation{};
    std::uint64_t captureAllocation{},captureRecording{},captureSerial{};
    bool retainable92{};
    std::uint64_t retirement92{},bufferEpoch92{},bufferGeneration92{};
    VkBuffer lastBuffer92{};VkDeviceSize lastBufferSize92{};
    KeyTable cache;SetTable ownedIndex;std::uint32_t ownedCount{},owner{UINT32_MAX};
    // Per-layout memo of the last (buffer, ranges) -> set resolved in this
    // recording: one 88-byte compare replaces the Key hash + table probe for the
    // consecutive same-shader draws DrawCommon issues. Stamped with the recording
    // generation, so a reset/forget invalidates every entry without touching it.
    struct Memo {std::uint64_t stamp{},layoutGeneration{},bufferGeneration{};VkBuffer buffer{};std::array<std::uint32_t,16> ranges{};VkDescriptorSet set{};};
    std::array<Memo,64> memo{};std::uint64_t memoHits{};
};
std::atomic<std::uint64_t> nextInstance{1};
// Fast mode (debug.rdrvr.dud.fast): per-thread caches handing out raw pointers. Same validity contract as
// Impl::layout()/command() (one instance + epoch; a miss reads the map under the mutex). The strong
// references live in pins/commands (current epoch) and retired* (older entries); retired* are released
// only by the next OUTERMOST DrawScope of this thread, when no State/Pending of this thread exists.
inline std::size_t layoutSlot(VkDescriptorSetLayout l)noexcept{auto h=std::uint64_t(reinterpret_cast<std::uintptr_t>(l));h^=h>>29;h*=0x9e3779b97f4a7c15ull;return std::size_t(h>>32);}
struct FastThread {
    static constexpr std::size_t maxLayoutSlots=std::size_t(1)<<16;
    // Open addressing (linear probing, power-of-two, load <= 1/2). handle 0 = empty slot (never a
    // converted layout); layout null = cached "not converted" answer, exactly like the 64-entry cache.
    // cpu02 (dud.lean bit 1): the layout's immutable per-draw fields ride in the slot (32 B), saving the dependent Layout heap read.
    struct LayoutSlot {VkDescriptorSetLayout handle{};const Layout* layout{};std::uint64_t generation{};std::uint16_t slotMask{},partialMask{};};
    std::uint64_t layoutInstance{},layoutEpoch{};std::vector<LayoutSlot> layouts;std::size_t layoutCount{};
    std::vector<std::shared_ptr<const Layout>> pins,retiredLayouts;
    struct CommandSlot {VkCommandBuffer handle{};std::shared_ptr<Command> value;bool used{};};
    std::uint64_t commandInstance{},commandEpoch{};std::array<CommandSlot,4> commands{};unsigned nextCommand{};
    std::vector<std::shared_ptr<Command>> retiredCommands;
    bool retired()const noexcept{return !retiredLayouts.empty()||!retiredCommands.empty();}
    void release()noexcept{retiredLayouts.clear();retiredCommands.clear();}
    // Throws only before changing anything (allocation of the retired list).
    void flushLayouts(){
        if(retiredLayouts.empty())retiredLayouts.swap(pins);
        else{retiredLayouts.insert(retiredLayouts.end(),std::make_move_iterator(pins.begin()),std::make_move_iterator(pins.end()));pins.clear();}
        std::fill(layouts.begin(),layouts.end(),LayoutSlot{});layoutCount=0;
    }
    void growLayouts(){
        std::vector<LayoutSlot> next(layouts.empty()?1024:layouts.size()*2);const auto mask=next.size()-1;
        for(const auto& e:layouts)if(e.handle){auto i=layoutSlot(e.handle)&mask;while(next[i].handle)i=(i+1)&mask;next[i]=e;}
        layouts.swap(next);
    }
    // Called only after a lookup miss in the current table. The pin is stored before the slot publishes the raw pointer.
    LayoutSlot insertLayout(VkDescriptorSetLayout handle,std::shared_ptr<const Layout> value){
        if((layoutCount+1)*2>layouts.size()){if(layouts.size()>=maxLayoutSlots)flushLayouts();else growLayouts();}
        const Layout* raw=value.get();if(value)pins.push_back(std::move(value));
        const auto mask=layouts.size()-1;
        for(auto i=layoutSlot(handle)&mask;;i=(i+1)&mask){auto& e=layouts[i];
            if(!e.handle){e=raw?LayoutSlot{handle,raw,raw->generation,raw->slotMask,raw->partialMask}:LayoutSlot{handle,nullptr};++layoutCount;return e;}}
    }
};
// Frees a thread's FastThread at thread exit (touched once per thread, when fast mode first allocates).
struct FastOwner {FastThread* fast{};FastThread** slot{};~FastOwner(){if(slot&&*slot==fast)*slot=nullptr;delete fast;}};
thread_local FastOwner fastOwner;
}
struct DynamicUniformDescriptors::Tls {
    LayoutScope* layoutScope{};DrawScope* drawScope{};std::uint64_t drawActivity{};
    struct LastContext {std::uint64_t instance{},generation{};void* context{};const Command* command{};} last;
    // debug.rdrvr.dud.fast state of this thread. fastOn implies fast!=nullptr. Counts are kept here
    // (single writer, plain adds) while fastOn and published to the owner's atomics by Impl::tick().
    // cpu02: lean = debug.rdrvr.dud.lean bits in effect (0 unless fastOn), verify = debug.rdrvr.dud.verify (only with lean).
    FastThread* fast{};bool fastOn{};std::uint8_t lean{};bool verify{};std::uint32_t fastCountdown{};std::uint64_t fastNextNs{};
    std::uint64_t countInstance{},calls{},hits{},binds{},offsets{},scopes{},layoutHits{};
};
thread_local DynamicUniformDescriptors::Tls DynamicUniformDescriptors::tls_;
struct DynamicUniformDescriptors::Impl {
    CaptureObserver observer{};std::uint64_t nextCaptureAllocation{};std::atomic<std::uint64_t> captureEpoch{1};
    Api api{};Limits limits{};VkDevice device{};std::atomic<bool> ready{};
    mutable std::mutex mutex;
    std::unordered_map<VkDescriptorSetLayout,std::shared_ptr<const Layout>> layouts;
    std::unordered_map<VkCommandBuffer,std::shared_ptr<Command>> commands;
    OwnedSets ownedSets;
    struct BufferLife92 {std::uint64_t generation{};VkDeviceSize size{};};
    std::unordered_map<VkBuffer,BufferLife92> buffers92;
    bool bufferCoverage92{};
    std::uint64_t nextBuffer92{},retirement92{1};
    std::atomic<std::uint64_t> bufferEpoch92{1};
    std::atomic<std::uint64_t> retainedRecordings92{};
    // Proof counters: retention engages only when the frame ring buffer is a
    // tracked uniform buffer (bufferTracked>0), which needs the routed
    // vkCreateBuffer proc to be live (bufferRoute>0). retainedSets/retainedRecordings92
    // is the average cache carried across a reset; forgets counts every cache drop.
    std::atomic<std::uint64_t> bufferRoute{},bufferTracked{},bufferRetired{},forgets{},forgetsBound{},retainedSets{},poolsDestroyed{},ownedPeak{},memoHits{};
    static constexpr std::size_t maxOwned=4096,maxPools=16;
    // cpu02: the hit path (5 calls per draw) is inlined; the lock + map lookup stays out of line.
    [[gnu::always_inline]] BufferLife92 bufferLife92(Command& c,VkBuffer buffer){
        const auto epoch=bufferEpoch92.load(std::memory_order_acquire);
        if(c.bufferEpoch92==epoch&&c.lastBuffer92==buffer)[[likely]]return {c.bufferGeneration92,c.lastBufferSize92};
        return bufferLife92Miss(c,buffer,epoch);
    }
    [[gnu::noinline]] BufferLife92 bufferLife92Miss(Command& c,VkBuffer buffer,std::uint64_t epoch){
        std::lock_guard lock(mutex);
        const auto found=buffers92.find(buffer);
        const auto life=bufferCoverage92&&found!=buffers92.end()?found->second:BufferLife92{};
        c.bufferEpoch92=epoch;c.lastBuffer92=buffer;c.bufferGeneration92=life.generation;c.lastBufferSize92=life.size;
        return life;
    }
    void disableBuffers92()noexcept{
        bufferCoverage92=false;buffers92.clear();
        if(retirement92!=UINT64_MAX)++retirement92;
        bufferEpoch92.fetch_add(1,std::memory_order_release);
    }
    std::uint64_t generation{};
    const std::uint64_t instance{nextInstance.fetch_add(1,std::memory_order_relaxed)};
    std::atomic<std::uint64_t> layoutEpoch{1},commandEpoch{1};
    std::atomic<std::uint64_t> converted{},nativeLayouts{},calls{},hits{},writes{},binds{},offsets{},faults{},resets{},poolsCreated{};
    std::shared_ptr<Command> command(VkCommandBuffer handle){
        struct Cache{std::uint64_t instance{},epoch{};VkCommandBuffer handle{};std::shared_ptr<Command> value;};
        static thread_local Cache cached;const auto epoch=commandEpoch.load(std::memory_order_acquire);
        if(cached.instance==instance&&cached.epoch==epoch&&cached.handle==handle)return cached.value;
        std::lock_guard lock(mutex);auto it=commands.find(handle);auto value=it==commands.end()?std::shared_ptr<Command>{}:it->second;
        cached={instance,epoch,handle,value};return value;
    }
    std::shared_ptr<const Layout> layout(VkDescriptorSetLayout handle){
        struct Entry{VkDescriptorSetLayout handle{};std::shared_ptr<const Layout> value;};
        struct Cache{std::uint64_t instance{},epoch{};std::array<Entry,64> entries;};
        static thread_local Cache cached;const auto epoch=layoutEpoch.load(std::memory_order_acquire);
        if(cached.instance!=instance||cached.epoch!=epoch){cached.entries={};cached.instance=instance;cached.epoch=epoch;}
        auto h=std::uint64_t(reinterpret_cast<std::uintptr_t>(handle));h^=h>>29;h*=0x9e3779b97f4a7c15ull;
        auto& entry=cached.entries[(h>>58)&63];if(entry.handle==handle)return entry.value;
        std::lock_guard lock(mutex);auto it=layouts.find(handle);entry={handle,it==layouts.end()?std::shared_ptr<const Layout>{}:it->second};return entry.value;
    }
    // Fast mode (debug.rdrvr.dud.fast). Busy scenes meet more layouts per frame than the 64-entry direct-mapped
    // cache above holds (3425 converted): ~46 % of nativeSet samples were its miss path (mutex + map +
    // shared_ptr atomics). This table keeps every layout the thread met in the epoch and returns a raw pointer.
    std::atomic<std::uint64_t> fastScopes{},fastLayoutHits{},fastLayoutMisses{},fastLayoutFlushes{},fastCommandMisses{};
    std::atomic<bool> fastOn{};
    // cpu02: debug.rdrvr.dud.lean bits (unset = all; any bit is off while dud.fast is off). verify* count only with debug.rdrvr.dud.verify=1:
    // checks run, Key/offsets of the lean path != the Layout path, Pending.count != raw->count, filter said "not owned" for an owned set.
    static constexpr unsigned leanLayout=1,leanOwned=4,leanAll=leanLayout|leanOwned;
    std::atomic<unsigned> leanOn{};std::atomic<bool> verifyOn{};
    std::atomic<std::uint64_t> verifyChecks{},verifyKeyMismatches{},verifyCountMismatches{},verifyFilterMismatches{};
    [[gnu::cold]] void verifyFilter(const SetTable& owned,VkDescriptorSet d)noexcept{
        verifyChecks.fetch_add(1,std::memory_order_relaxed);if(owned.contains(d,false))verifyFilterMismatches.fetch_add(1,std::memory_order_relaxed);}
    [[gnu::cold]] void verifyCount(const Pending& p)noexcept{
        verifyChecks.fetch_add(1,std::memory_order_relaxed);if(p.count!=p.raw->count)verifyCountMismatches.fetch_add(1,std::memory_order_relaxed);}
    FastThread::LayoutSlot fastLayout(Tls& t,VkDescriptorSetLayout handle){
        if(!handle)return {};auto& f=*t.fast;
        const auto epoch=layoutEpoch.load(std::memory_order_acquire);
        if(f.layoutInstance!=instance||f.layoutEpoch!=epoch){f.flushLayouts();f.layoutInstance=instance;f.layoutEpoch=epoch;fastLayoutFlushes.fetch_add(1,std::memory_order_relaxed);}
        if(f.layoutCount){const auto mask=f.layouts.size()-1;
            for(auto i=layoutSlot(handle)&mask;;i=(i+1)&mask){const auto& e=f.layouts[i];if(e.handle==handle){++t.layoutHits;return e;}if(!e.handle)break;}}
        std::shared_ptr<const Layout> value;{std::lock_guard lock(mutex);const auto it=layouts.find(handle);if(it!=layouts.end())value=it->second;}
        fastLayoutMisses.fetch_add(1,std::memory_order_relaxed);return f.insertLayout(handle,std::move(value));
    }
    // Same answer as command() (one instance + commandEpoch); 4 entries, replaced ones are retired, not dropped.
    Command* fastCommand(FastThread& f,VkCommandBuffer handle){
        const auto epoch=commandEpoch.load(std::memory_order_acquire);
        if(f.commandInstance!=instance||f.commandEpoch!=epoch){
            for(auto& e:f.commands)if(e.used){if(e.value)f.retiredCommands.push_back(std::move(e.value));e.value.reset();e.used=false;}
            f.commandInstance=instance;f.commandEpoch=epoch;}
        for(const auto& e:f.commands)if(e.used&&e.handle==handle)return e.value.get();
        std::shared_ptr<Command> value;{std::lock_guard lock(mutex);const auto it=commands.find(handle);if(it!=commands.end())value=it->second;}
        fastCommandMisses.fetch_add(1,std::memory_order_relaxed);
        auto& e=f.commands[f.nextCommand++&3u];if(e.value)f.retiredCommands.push_back(std::move(e.value));
        e.handle=handle;e.value=std::move(value);e.used=true;return e.value.get();
    }
    // Fast mode counts in the thread's Tls (plain adds) and tick() publishes them every 256 scopes.
    void count(Tls& t,std::uint64_t Tls::* local,std::atomic<std::uint64_t>& shared,std::uint64_t n=1)noexcept{
        if(t.fastOn&&t.countInstance==instance)t.*local+=n;else shared.fetch_add(n,std::memory_order_relaxed);
    }
    // Every 256 DrawScopes of a thread: publish its counts; about once per second re-read debug.rdrvr.dud.fast.
    void tick(Tls& t)noexcept{
        if(t.countInstance==instance&&(t.calls|t.hits|t.binds|t.offsets|t.scopes|t.layoutHits)){
            calls.fetch_add(t.calls,std::memory_order_relaxed);hits.fetch_add(t.hits,std::memory_order_relaxed);
            binds.fetch_add(t.binds,std::memory_order_relaxed);offsets.fetch_add(t.offsets,std::memory_order_relaxed);
            fastScopes.fetch_add(t.scopes,std::memory_order_relaxed);fastLayoutHits.fetch_add(t.layoutHits,std::memory_order_relaxed);
        }// else: counts of another owner instance (never in the game: one global owner) are dropped, never written to it.
        t.calls=t.hits=t.binds=t.offsets=t.scopes=t.layoutHits=0;t.countInstance=instance;
        const int saved=errno;timespec ts{};clock_gettime(CLOCK_MONOTONIC,&ts);
        const auto now=std::uint64_t(ts.tv_sec)*1000000000ull+std::uint64_t(ts.tv_nsec);
        if(now>=t.fastNextNs){t.fastNextNs=now+1000000000ull;bool on=false;unsigned lean=0;bool verify=false;
#ifdef __ANDROID__
            // Default ON (unset); "0" (any value but "1") turns it off. Saves about 0.8 ms of
            // render-thread time per frame on Quest 3.
            char v[PROP_VALUE_MAX]{};on=__system_property_get("debug.rdrvr.dud.fast",v)<=0||(v[0]=='1'&&!v[1]);
            // cpu02: debug.rdrvr.dud.lean unset = all bits (1 layout slot cache, 4 owned-set filter), else its value; dud.verify=1 checks them.
            char l[PROP_VALUE_MAX]{};lean=__system_property_get("debug.rdrvr.dud.lean",l)<=0?leanAll:unsigned(std::strtoul(l,nullptr,0))&leanAll;
            char w[PROP_VALUE_MAX]{};verify=__system_property_get("debug.rdrvr.dud.verify",w)>0&&w[0]=='1'&&!w[1];
#endif
            if(on&&!t.fast){auto* f=new(std::nothrow) FastThread;
                if(f){try{f->retiredCommands.reserve(32);fastOwner.fast=f;fastOwner.slot=&t.fast;t.fast=f;}catch(...){delete f;}}}
            t.fastOn=on&&t.fast;fastOn.store(t.fastOn,std::memory_order_relaxed);
            t.lean=std::uint8_t(t.fastOn?lean:0);t.verify=t.lean&&verify;leanOn.store(t.lean,std::memory_order_relaxed);verifyOn.store(t.verify,std::memory_order_relaxed);
        }
        errno=saved;
    }
    bool bufferCurrent92(Command& c,VkBuffer b,std::uint64_t incarnation){
        return !incarnation||bufferLife92(c,b).generation==incarnation;
    }
    bool anyOwned(std::uint32_t count,const VkDescriptorSet* handles){
        if(!handles||!count)return false;std::lock_guard lock(mutex);
        for(unsigned i=0;i<count;++i)if(ownedSets.contains(handles[i]))return true;
        return false;
    }
    CaptureCommand captureCommand(const Command& c)const noexcept{return {c.device,c.commandPool,c.command,instance,c.captureAllocation,c.captureRecording};}
    void invalidateCapture(Command& c)noexcept{
        ++c.captureSerial;captureEpoch.fetch_add(1,std::memory_order_release);
        if(observer){const int saved=errno;observer.invalidate(observer.context,captureCommand(c));errno=saved;}
    }
    // Lifecycle callers hold mutex; a command's recording itself is externally
    // synchronized by Vulkan. Other commands never inspect these local vectors.
    void forget(Command& c)noexcept{++forgets;invalidateCapture(c);if(c.captureRecording!=UINT64_MAX)++c.captureRecording;else c.active=false;ownedSets.retire(c.owner);c.ownedCount=0;c.ownedIndex.clear();c.cache.clear();c.needsDescriptorBind=true;++c.generation;}
    void destroy(Command& c)noexcept{forget(c);for(auto p:c.pools)api.destroyPool(c.device,p,nullptr);poolsDestroyed+=c.pools.size();c.pools.clear();c.poolIndex=0;c.usedInPool=0;c.recording=false;}
    bool reset(Command& c)noexcept{
        c.recording=false;memoHits+=c.memoHits;c.memoHits=0;
        // Only immutable sets survive. A native command reset is independent of
        // these private pools. No descriptors are rewritten after publication.
        // A destroyed layout or uniform buffer does not force a forget;
        // its entries are unreachable through Key.generation/Key.bufferGeneration
        // (never rewritten, never bound again) and age out at the owned bound.
        // retirement only marks coverage loss (disableBuffers).
        const bool bounded=c.ownedCount<=maxOwned&&c.pools.size()<=maxPools;
        const bool keep=bufferCoverage92&&retirement92!=UINT64_MAX&&c.retainable92&&
            c.retirement92==retirement92&&!c.cache.empty()&&bounded;
        if(keep){
            invalidateCapture(c);
            if(c.captureRecording==UINT64_MAX||c.generation==UINT64_MAX){c.active=false;return false;}
            ++c.captureRecording;++c.generation;c.needsDescriptorBind=true;++retainedRecordings92;retainedSets+=c.cache.size();
            return true;
        }
        if(!bounded)++forgetsBound;
        forget(c);c.retirement92=retirement92;c.retainable92=bufferCoverage92&&retirement92!=UINT64_MAX;
        for(auto p:c.pools)if(api.resetPool(c.device,p,0)!=VK_SUCCESS){destroy(c);++faults;return false;}
        // Every pool is kept across resets (all are idle at this boundary)
        // and refilled in order; pools are destroyed only with their command.
        c.poolIndex=0;c.usedInPool=0;++resets;return true;
    }
    VkDescriptorSet allocate(Command& c,VkDescriptorSetLayout l) {
        // A frame fits one pool; retained pools are refilled in order after a
        // reset and a new one is created only when every retained pool is full.
        constexpr unsigned perPool=1024;
        if(!c.pools.empty()&&c.usedInPool==perPool&&c.poolIndex+1<c.pools.size()){++c.poolIndex;c.usedInPool=0;}
        if(c.pools.empty()||c.usedInPool==perPool){
            if(c.pools.size()==maxPools)return {};
            // Reserve metadata before publishing a Vulkan object.
            c.pools.reserve(c.pools.size()+1);
            VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,perPool*slots};
            VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};ci.maxSets=perPool;ci.poolSizeCount=1;ci.pPoolSizes=&size;
            VkDescriptorPool p{};if(api.createPool(c.device,&ci,nullptr,&p)!=VK_SUCCESS||!p)return {};
            c.pools.push_back(p);c.poolIndex=c.pools.size()-1;c.usedInPool=0;++poolsCreated;
        }
        VkDescriptorSetAllocateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ci.descriptorPool=c.pools[c.poolIndex];ci.descriptorSetCount=1;ci.pSetLayouts=&l;
        VkDescriptorSet set{};if(api.allocate(c.device,&ci,&set)!=VK_SUCCESS||!set)return {};
        ++c.usedInPool;++c.ownedCount;
        if(c.ownedCount>ownedPeak.load(std::memory_order_relaxed))ownedPeak.store(c.ownedCount,std::memory_order_relaxed);
        c.ownedIndex.insert(set);{std::lock_guard lock(mutex);ownedSets.insert(set,c.owner);}return set;
    }
};
struct DynamicUniformDescriptors::DrawScope::State {
    // command is what every reader uses; commandRef owns it in the normal mode, fast mode borrows it
    // from the thread's FastThread (kept alive until the next outermost scope).
    std::shared_ptr<Command> commandRef;Command* command{};unsigned count{};
    // Only [0,count) contains live Pending objects. The draw scope owns their
    // layout references until its destructor, including all failure paths.
    alignas(Pending) std::array<std::byte,sizeof(Pending)*sets> pendingStorage;
    Pending& pending(unsigned index)noexcept{return *std::launder(reinterpret_cast<Pending*>(pendingStorage.data()+sizeof(Pending)*index));}
    ~State(){for(unsigned i=0;i<count;++i)pending(i).~Pending();}
    VkBuffer buffer{};std::uint64_t bufferGeneration92{};std::uint32_t size{};void* context{};const void* constants{};
    std::uintptr_t frame{};std::uint64_t commandEpoch{};const Pending* boundMatrix{};
};
DynamicUniformDescriptors::DynamicUniformDescriptors():impl_(std::make_unique<Impl>()){}
DynamicUniformDescriptors::~DynamicUniformDescriptors()=default;
DynamicUniformDescriptors dynamicUniformDescriptors;
bool DynamicUniformDescriptors::observeCaptures(CaptureObserver observer)noexcept{
    if(ready()||impl_->observer||!observer)return false;impl_->observer=observer;return true;
}
bool DynamicUniformDescriptors::ready()const noexcept{return impl_->ready.load(std::memory_order_acquire);}
DynamicUniformDescriptors::Stats DynamicUniformDescriptors::snapshot()const noexcept{return {ready(),impl_->calls.load(std::memory_order_relaxed),impl_->hits.load(std::memory_order_relaxed),impl_->faults.load(std::memory_order_relaxed)};}
bool DynamicUniformDescriptors::configure(VkDevice d,Api api,Limits l)noexcept{
    if(!d||!api||!l.dynamicUniform||l.maxSets<sets||!l.alignment||(l.alignment&(l.alignment-1))||!l.maxRange||ready())return false;
    impl_->api=api;impl_->limits=l;impl_->device=d;impl_->ready.store(true,std::memory_order_release);return true;
}
bool DynamicUniformDescriptors::enablePersistentBuffers(VkDevice d)noexcept{
    if(!ready()||d!=impl_->device)return false;std::lock_guard lock(impl_->mutex);
    if(impl_->bufferCoverage92)return false;
    impl_->disableBuffers92();impl_->bufferCoverage92=true;return true;
}
void DynamicUniformDescriptors::disablePersistentBuffers(VkDevice d)noexcept{
    if(d!=impl_->device)return;std::lock_guard lock(impl_->mutex);impl_->disableBuffers92();
}
void DynamicUniformDescriptors::bufferCreated(VkDevice d,VkBuffer b,VkDeviceSize size,VkBufferUsageFlags usage)noexcept{
    const int saved=errno;impl_->bufferRoute.fetch_add(1,std::memory_order_relaxed);
    if(ready()&&d==impl_->device&&b&&size&&(usage&VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)){
        std::lock_guard lock(impl_->mutex);
        if(impl_->bufferCoverage92){
            if(impl_->buffers92.size()>=32768||impl_->nextBuffer92==UINT64_MAX||impl_->buffers92.contains(b))impl_->disableBuffers92();
            else try{impl_->buffers92.emplace(b,Impl::BufferLife92{++impl_->nextBuffer92,size});impl_->bufferEpoch92.fetch_add(1,std::memory_order_release);++impl_->bufferTracked;}
                 catch(...){impl_->disableBuffers92();}
        }
    }
    errno=saved;
}
void DynamicUniformDescriptors::bufferDestroyed(VkDevice d,VkBuffer b)noexcept{
    const int saved=errno;
    if(ready()&&d==impl_->device&&b){std::lock_guard lock(impl_->mutex);
        // Lazy eviction. Entries keyed by this buffer's generation become
        // unreachable; a reused handle gets a new generation on its next create.
        if(impl_->buffers92.erase(b)){++impl_->bufferRetired;impl_->bufferEpoch92.fetch_add(1,std::memory_order_release);}}
    errno=saved;
}
bool DynamicUniformDescriptors::nativeLayoutEligible(const void* vertex,const void* pixel)const noexcept{
    if(!ready()||!vertex||!pixel)return false;
    // Match the native merge of both shader vectors by set index and binding.
    // Refuse any unmodeled non-bindless descriptor group before converting its
    // first layout, so the ordinary native pipeline remains a complete fallback.
    std::array<std::array<std::uint8_t,slots>,sets> seen{};
    std::array<int,sets> types;types.fill(-1);unsigned total{};
    for(const void* vector:{vertex,pixel}){
        const auto begin=read<std::uintptr_t>(vector),end=read<std::uintptr_t>(vector,8);
        if(end<begin||(end-begin)%0x38||(end-begin)/0x38>sets||(!begin&&end))return false;
        for(auto group=begin;group<end;group+=0x38){
            const auto* data=reinterpret_cast<const void*>(group);const auto set=read<std::uint32_t>(data);
            if(set>=sets)return false;
            const auto first=read<std::uintptr_t>(data,8),last=read<std::uintptr_t>(data,16);
            const auto flags=read<std::uintptr_t>(data,0x20),flagsEnd=read<std::uintptr_t>(data,0x28);
            if(last<first||(last-first)%sizeof(VkDescriptorSetLayoutBinding)||(!first&&last)||
               flagsEnd<flags||(flagsEnd-flags)%4||(!flags&&flagsEnd))return false;
            const auto count=(last-first)/sizeof(VkDescriptorSetLayoutBinding);
            const auto flagCount=(flagsEnd-flags)/4;
            if(!count||count>256||(flagCount&&flagCount!=count))return false;
            for(std::size_t i=0;i<count;++i){
                const auto b=read<VkDescriptorSetLayoutBinding>(reinterpret_cast<void*>(first),i*sizeof(VkDescriptorSetLayoutBinding));
                if(types[set]>=0&&types[set]!=int(b.descriptorType))return false;types[set]=int(b.descriptorType);
                if(b.descriptorType<=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)continue;
                if(b.descriptorType!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||b.binding>=slots||b.descriptorCount!=1||b.pImmutableSamplers||
                   !b.stageFlags||(b.stageFlags&~(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT)))return false;
                const auto f=flagCount?read<VkDescriptorBindingFlags>(reinterpret_cast<void*>(flags),i*4):0;
                if(f&~(VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT|VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT|VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT))return false;
                const auto shape=std::uint8_t(1+bool(f&VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT));
                if(seen[set][b.binding]&&seen[set][b.binding]!=shape)return false;
                if(!seen[set][b.binding]){seen[set][b.binding]=shape;if(++total>impl_->limits.dynamicUniform)return false;}
            }
        }
    }
    return total!=0;
}
DynamicUniformDescriptors::LayoutScope::LayoutScope(DynamicUniformDescriptors& o,bool q)noexcept:owner_(&o),previous_(tls_.layoutScope),qualified_(q&&o.ready()){tls_.layoutScope=this;}
DynamicUniformDescriptors::LayoutScope::~LayoutScope(){tls_.layoutScope=previous_;}
DynamicUniformDescriptors::DrawScope::DrawScope(DynamicUniformDescriptors& o,void* context,bool q)noexcept:previous_(tls_.drawScope),owner_(&o),qualified_(q&&o.ready()){
    auto& t=tls_;
    // Every 256 scopes of this thread publish fast-mode counts (and about once per second re-read the property).
    if(!t.fastCountdown--){t.fastCountdown=255;o.impl_->tick(t);}
    // Outside every DrawScope no State/Pending of this thread exists, so what fast mode retired can go.
    if(!previous_&&t.fast&&t.fast->retired())t.fast->release();
    if(t.drawActivity!=UINT64_MAX)++t.drawActivity;
    t.drawScope=this;if(!qualified_)return;
    context_=context;if(context)dirty_=read<std::uint32_t>(context,0x32dc);
    if(previous_&&previous_->owner_==&o){good_=false;previous_->good_=false;++o.impl_->faults;return;}
    try{
        if(!context)throw 0;
        const auto frame=read<std::uintptr_t>(context,0x30a0);if(!frame)throw 0;
        const auto command=read<VkCommandBuffer>(reinterpret_cast<void*>(frame),0x38);
        const auto wrapper=read<std::uintptr_t>(reinterpret_cast<void*>(frame),0x98);if(!wrapper)throw 0;
        static_assert(sizeof(State)<=2048&&alignof(State)<=alignof(std::max_align_t));
        auto* s=new(storage_.data()) State;state_=s;s->context=context;s->frame=frame;s->constants=reinterpret_cast<void*>(frame+0x60);
        s->buffer=read<VkBuffer>(reinterpret_cast<void*>(wrapper),0x10);s->size=read<std::uint32_t>(reinterpret_cast<void*>(wrapper),0x20);
        // Fast mode borrows the Command from the thread's cache (no shared_ptr copy + release per draw).
        if(t.fastOn&&t.fast){s->command=o.impl_->fastCommand(*t.fast,command);if(t.countInstance==o.impl_->instance)++t.scopes;}
        else{s->commandRef=o.impl_->command(command);s->command=s->commandRef.get();}
        if(!s->command||!s->command->active||!s->command->recording||!s->buffer||!s->size)throw 0;
        s->commandEpoch=s->command->generation;
        const auto life=o.impl_->bufferLife92(*s->command,s->buffer);
        if(life.generation&&life.size>=s->size)s->bufferGeneration92=life.generation;
        else s->command->retainable92=false;
        auto& last=t.last;
        if(last.instance!=o.impl_->instance||last.generation!=s->commandEpoch||last.context!=context||last.command!=s->command)s->command->needsDescriptorBind=true;
        last={o.impl_->instance,s->commandEpoch,context,s->command};
        if(s->command->needsDescriptorBind){const auto flags=dirty_|2u;std::memcpy(static_cast<unsigned char*>(context)+0x32dc,&flags,sizeof flags);}
    }catch(...){good_=false;++o.impl_->faults;}
}
DynamicUniformDescriptors::DrawScope::~DrawScope(){
    auto& t=tls_;
    if(t.drawActivity!=UINT64_MAX)++t.drawActivity;
    if(qualified_&&!good_&&context_){const auto flags=read<std::uint32_t>(context_,0x32dc)|dirty_|2u;std::memcpy(static_cast<unsigned char*>(context_)+0x32dc,&flags,sizeof flags);}
    if(state_)state_->~State();t.drawScope=previous_;
}
bool DynamicUniformDescriptors::DrawScope::matrixBinding(UniformBinding& out)const noexcept {
    out={};const auto* s=state_;
    if(tls_.drawScope!=this||!owner_||!qualified_||!good_||!s||!s->boundMatrix||
       !owner_->ready()||!s->command->active||!s->command->recording||
       s->command->generation!=s->commandEpoch||s->command->needsDescriptorBind)return false;
    const auto& p=*s->boundMatrix;if(!p.set||!p.matrixRange)return false;
    out={s->command->command,p.set,s->buffer,p.matrixOffset,p.matrixRange,true};return true;
}
VkResult DynamicUniformDescriptors::createLayout(PFN_vkCreateDescriptorSetLayout original,VkDevice d,const VkDescriptorSetLayoutCreateInfo* ci,const VkAllocationCallbacks* a,VkDescriptorSetLayout* out)noexcept{
    const int incoming=errno;auto* scope=tls_.layoutScope;
    auto native=[&](){++impl_->nativeLayouts;errno=incoming;return original(d,ci,a,out);};
    if(!ready()||d!=impl_->device||!scope||scope->owner_!=this||!scope->qualified_||!ci||!out||
       ci->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO||!ci->bindingCount||ci->bindingCount>slots||!ci->pBindings||
       (ci->flags&~VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT)||
       scope->converted_+ci->bindingCount>impl_->limits.dynamicUniform)return native();
    std::array<VkDescriptorSetLayoutBinding,slots> bindings{};std::array<VkDescriptorBindingFlags,slots> flags{};
    const VkDescriptorSetLayoutBindingFlagsCreateInfo* sourceFlags=nullptr;
    if(ci->pNext){sourceFlags=static_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(ci->pNext);
        if(sourceFlags->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO||sourceFlags->pNext||
           (sourceFlags->bindingCount&&sourceFlags->bindingCount!=ci->bindingCount)||
           (sourceFlags->bindingCount&&!sourceFlags->pBindingFlags))return native();}
    try{
        auto layout=std::make_shared<Layout>();layout->count=ci->bindingCount;std::uint16_t seen{};
        for(unsigned i=0;i<ci->bindingCount;++i){auto b=ci->pBindings[i];const auto f=sourceFlags&&sourceFlags->bindingCount?sourceFlags->pBindingFlags[i]:0;
            if(b.descriptorType!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||b.descriptorCount!=1||b.binding>=slots||b.pImmutableSamplers||
               !b.stageFlags||(b.stageFlags&~(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT))||
               (seen&(1u<<b.binding))||(f&~(VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT|VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT|VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT)))return native();
            seen|=std::uint16_t(1u<<b.binding);b.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;bindings[i]=b;
            flags[i]=f&VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;layout->bindings[i]={b.binding,bool(flags[i]),b.stageFlags};
        }
        std::sort(layout->bindings.begin(),layout->bindings.begin()+layout->count,[](const auto& x,const auto& y){return x.slot<y.slot;});
        for(unsigned i=0;i<layout->count;++i){const auto& b=layout->bindings[i];layout->slotMask|=std::uint16_t(1u<<b.slot);if(b.partial)layout->partialMask|=std::uint16_t(1u<<b.slot);}
        VkDescriptorSetLayoutBindingFlagsCreateInfo f{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};f.bindingCount=ci->bindingCount;f.pBindingFlags=flags.data();
        auto copy=*ci;copy.flags=0;copy.pNext=sourceFlags?&f:nullptr;copy.pBindings=bindings.data();
        errno=incoming;const auto r=original(d,&copy,a,out);const int saved=errno;if(r!=VK_SUCCESS||!*out){errno=saved;return r;}
        layout->handle=*out;
        try{std::lock_guard lock(impl_->mutex);layout->generation=++impl_->generation;
            if(!impl_->layouts.emplace(*out,std::move(layout)).second)throw 0;
            impl_->layoutEpoch.fetch_add(1,std::memory_order_release);}
        catch(...){impl_->api.destroyLayout(d,*out,a);*out={};return native();}
        scope->converted_+=ci->bindingCount;++impl_->converted;errno=saved;return r;
    }catch(...){return native();}
}
void DynamicUniformDescriptors::destroyLayout(PFN_vkDestroyDescriptorSetLayout original,VkDevice d,VkDescriptorSetLayout l,const VkAllocationCallbacks* a)noexcept{
    // Sets allocated from this layout stay valid Vulkan objects (never updated
    // afterwards) and are unreachable through Key.generation; no forget is forced.
    const int saved=errno;if(d==impl_->device){std::lock_guard lock(impl_->mutex);impl_->layouts.erase(l);impl_->layoutEpoch.fetch_add(1,std::memory_order_release);}errno=saved;original(d,l,a);
}
std::uintptr_t DynamicUniformDescriptors::nativeSet(NativeOriginal original,void* context,void* pool,const void* layoutPtr,const void* constants,void* vs,void* ps,int type)noexcept{
    auto& t=tls_;
    if(t.drawScope&&t.drawScope->owner_==this&&t.drawScope->state_)++t.drawScope->state_->command->captureSerial;
    const int incoming=errno;std::shared_ptr<const Layout> layout;const Layout* raw{};FastThread::LayoutSlot view{};
    if(ready()&&layoutPtr){try{const auto handle=read<VkDescriptorSetLayout>(layoutPtr);
            // Fast mode resolves through the thread's full table (raw pointer, no refcount).
            if(t.fastOn&&t.fast){view=impl_->fastLayout(t,handle);raw=view.layout;}else{layout=impl_->layout(handle);raw=layout.get();}}catch(...){
        auto* s=t.drawScope;if(s&&s->owner_==this)s->good_=false;++impl_->faults;errno=incoming;return 0;}}
    if(!raw){errno=incoming;return original(context,pool,layoutPtr,constants,vs,ps,type);}
    impl_->count(t,&Tls::calls,impl_->calls);auto* scope=t.drawScope;
    auto failure=[&](){if(scope&&scope->owner_==this)scope->good_=false;++impl_->faults;errno=incoming;return std::uintptr_t(0);};
    if(!scope||scope->owner_!=this||!scope->qualified_||!scope->good_||!scope->state_||type!=6)return failure();
    auto& s=*scope->state_;if(context!=s.context||constants!=s.constants||s.count==sets||
        !s.command->recording||s.command->generation!=s.commandEpoch||read<std::uintptr_t>(context,0x30a0)!=s.frame)return failure();
    try{
        if(!impl_->bufferCurrent92(*s.command,s.buffer,s.bufferGeneration92))return failure();
        const auto first=read<std::uintptr_t>(constants),last=read<std::uintptr_t>(constants,8);
        if(!first||last<first||(last-first)%8||(last-first)/8>256)return failure();
        // cpu02 (dud.lean bit 1; t.lean is 0 outside fast mode): generation and bindings come from the table slot (bindings[i] =
        // i-th set bit of slotMask) instead of the Layout heap object; lean=false is the old path. Same Key/Pending by construction.
        const bool p1=(t.lean&Impl::leanLayout)&&view.layout==raw;
        auto build=[&](bool lean,Key& key,Pending& pending)__attribute__((always_inline))->bool{
            key.generation=lean?view.generation:raw->generation;key.bufferGeneration=s.bufferGeneration92;key.buffer=s.buffer;
            unsigned i=0;for(std::uint32_t m=view.slotMask;lean?m!=0:i<raw->count;++i){std::uint32_t slot;bool partial;
                if(lean){slot=std::uint32_t(__builtin_ctz(m));m&=m-1;partial=(view.partialMask>>slot)&1u;}else{slot=raw->bindings[i].slot;partial=raw->bindings[i].partial;}
                if(slot>=(last-first)/8)return false;
                const auto cb=read<std::uintptr_t>(reinterpret_cast<void*>(first),slot*8);
                if(!cb){if(!partial)return false;continue;}
                const auto cl=read<std::uintptr_t>(reinterpret_cast<void*>(cb),8);if(!cl)return false;
                const auto range=read<std::int32_t>(reinterpret_cast<void*>(cl),0x18);const auto offset=read<std::uint64_t>(reinterpret_cast<void*>(cb),0x28);
                if(range<=0||std::uint32_t(range)>impl_->limits.maxRange||offset>std::numeric_limits<std::uint32_t>::max()||
                   offset%impl_->limits.alignment||offset>s.size||std::uint64_t(range)>s.size-offset)return false;
                key.ranges[i]=std::uint32_t(range);pending.ranges[i]=std::uint32_t(range);pending.offsets[i]=std::uint32_t(offset);
                if(slot==1){pending.matrixRange=std::uint32_t(range);pending.matrixOffset=std::uint32_t(offset);}
            }
            pending.count=std::uint8_t(i);return true;};
        Key key;Pending pending;const bool built=build(p1,key,pending);
        // cpu02: debug.rdrvr.dud.verify=1 also builds the Key the old way (Layout heap object) and counts any difference.
        if(p1&&t.verify){Key k;Pending p;const bool ok=build(false,k,p);impl_->verifyChecks.fetch_add(1,std::memory_order_relaxed);
            if(ok!=built||(ok&&!(k==key&&p.offsets==pending.offsets&&p.ranges==pending.ranges&&p.matrixRange==pending.matrixRange&&p.matrixOffset==pending.matrixOffset&&p.count==pending.count)))
                impl_->verifyKeyMismatches.fetch_add(1,std::memory_order_relaxed);}
        if(!built)return failure();
        auto& c=*s.command;auto& memo=c.memo[key.generation&63];
        // A zero memo never matches: layout generations start at 1 and the stamp is
        // the recording generation plus one.
        if(memo.stamp==c.generation+1&&memo.layoutGeneration==key.generation&&memo.buffer==key.buffer&&
           memo.bufferGeneration==key.bufferGeneration&&memo.ranges==key.ranges){
            pending.set=memo.set;++c.memoHits;impl_->count(t,&Tls::hits,impl_->hits);
        }else{
            const auto hash=KeyHash{}(key);
            if(const auto hit=c.cache.find(key,hash)){pending.set=hit;impl_->count(t,&Tls::hits,impl_->hits);}
            else{
                pending.set=impl_->allocate(c,raw->handle);if(!pending.set)return failure();
                std::array<VkDescriptorBufferInfo,slots> infos{};std::array<VkWriteDescriptorSet,slots> writes{};unsigned n{};
                for(unsigned i=0;i<raw->count;++i)if(key.ranges[i]){infos[n]={s.buffer,0,key.ranges[i]};auto& w=writes[n];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=pending.set;w.dstBinding=raw->bindings[i].slot;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;w.pBufferInfo=&infos[n];++n;}
                if(n)impl_->api.update(c.device,n,writes.data(),0,nullptr);impl_->writes+=n;
                // Metadata failure can discard reuse but must retain this valid set
                // in its owning pool until the command is idle.
                try{c.cache.insert(key,hash,pending.set);}catch(...){}
            }
            memo={c.generation+1,key.generation,key.bufferGeneration,key.buffer,key.ranges,pending.set};
        }
        if(t.drawScope!=scope||!scope->good_||scope->state_!=&s||!s.command->active||!s.command->recording||
           s.command->generation!=s.commandEpoch||!impl_->bufferCurrent92(*s.command,s.buffer,s.bufferGeneration92))return failure();
        pending.layout=std::move(layout);pending.raw=raw;const auto set=pending.set;new(s.pendingStorage.data()+sizeof(Pending)*s.count)Pending(std::move(pending));++s.count;errno=incoming;return reinterpret_cast<std::uintptr_t>(set);
    }catch(...){return failure();}
}
void DynamicUniformDescriptors::bind(PFN_vkCmdBindDescriptorSets original,VkCommandBuffer command,VkPipelineBindPoint point,VkPipelineLayout layout,std::uint32_t first,std::uint32_t count,const VkDescriptorSet* descriptors,std::uint32_t n,const std::uint32_t* offsets)noexcept{
    auto& t=tls_;const int incoming=errno;auto* scope=t.drawScope;
    if(!ready()||!scope||scope->owner_!=this||!scope->qualified_){
        commandStateInvalidated(command);
        if(ready())try{if(impl_->anyOwned(count,descriptors)){if(scope&&scope->owner_==this)scope->good_=false;++impl_->faults;errno=incoming;return;}}
            catch(...){++impl_->faults;errno=incoming;return;}
        errno=incoming;original(command,point,layout,first,count,descriptors,n,offsets);return;}
    auto fail=[&](){scope->good_=false;if(scope->state_)impl_->invalidateCapture(*scope->state_->command);++impl_->faults;errno=incoming;};
    if(!scope->good_||!scope->state_){fail();return;}auto& s=*scope->state_;
    if(command!=s.command->command||!s.command->recording||s.command->generation!=s.commandEpoch||
       point!=VK_PIPELINE_BIND_POINT_GRAPHICS||n||!descriptors||!count||count>sets||first>sets-count){fail();return;}
    if(!impl_->bufferCurrent92(*s.command,s.buffer,s.bufferGeneration92)){fail();return;}
    std::array<const Pending*,sets> matched{};
    // cpu02: only [0,total) is ever passed on, so no zero fill.
    std::array<std::uint32_t,slots*sets> dynamic;unsigned total{},consumed{};
    ++s.command->captureSerial;
    s.boundMatrix=nullptr;const Pending* matrix=nullptr;
    // cpu02: dud.lean bit 1 takes the binding count from Pending, bit 4 asks the owned-set filter before the probe (same answers).
    const unsigned lean=t.lean;const bool filtered=lean&Impl::leanOwned,verify=t.verify;
    for(unsigned j=0;j<count;++j){const Pending* found=nullptr;
        if(consumed<s.count&&s.pending(consumed).set==descriptors[j])found=&s.pending(consumed++);
        else if(s.command->ownedIndex.contains(descriptors[j],filtered)){fail();return;}
        else if(filtered&&verify)impl_->verifyFilter(s.command->ownedIndex,descriptors[j]);
        matched[j]=found;
        if(found){const unsigned bindings=lean&Impl::leanLayout?found->count:found->raw->count;if(verify&&(lean&Impl::leanLayout))impl_->verifyCount(*found);
            for(unsigned i=0;i<bindings;++i)dynamic[total++]=found->offsets[i];
            if(first==0&&j==0)matrix=found;}
    }
    if(consumed!=s.count||total>impl_->limits.dynamicUniform){fail();return;}
    const auto observer=impl_->observer;
    bool captureEnabled=bool(observer);
    if(captureEnabled&&observer.enabled){
        // This predicate runs before CaptureBatch construction and any capture-only
        // mapped-ring reads. Its callbacks must not leave a stale owned DS bind.
        const auto activity=t.drawActivity,epoch=impl_->captureEpoch.load(std::memory_order_acquire);
        const auto serial=s.command->captureSerial,generation=s.command->generation;
        captureEnabled=observer.enabled(observer.context);
        if(!ready()||t.drawScope!=scope||!scope->good_||scope->state_!=&s||
           activity!=t.drawActivity||epoch!=impl_->captureEpoch.load(std::memory_order_acquire)||
           !s.command->active||!s.command->recording||s.command->generation!=generation||
           s.command->generation!=s.commandEpoch||s.command->captureSerial!=serial){fail();return;}
    }
    if(!captureEnabled){
        if(!impl_->bufferCurrent92(*s.command,s.buffer,s.bufferGeneration92)){fail();return;}
        errno=incoming;original(command,point,layout,first,count,descriptors,total,total?dynamic.data():nullptr);const int saved=errno;
        s.command->needsDescriptorBind=false;s.boundMatrix=matrix;
        impl_->count(t,&Tls::binds,impl_->binds);impl_->count(t,&Tls::offsets,impl_->offsets,total);errno=saved;return;
    }
    // cpu02: the capture bind (CaptureBatch is ~10 KB) runs in its own frame, so the plain bind above keeps a small one.
    [&]()__attribute__((noinline)){
        CaptureBatch batch;bool prepared=false;
        if(observer){
            batch.owner_=this;batch.scope_=scope;batch.state_=&s;batch.thread_=std::this_thread::get_id();
            batch.epoch_=impl_->captureEpoch.load(std::memory_order_acquire);batch.serial_=s.command->captureSerial;
            batch.commandEpoch_=s.commandEpoch;batch.command_=impl_->captureCommand(*s.command);batch.drawActivity_=t.drawActivity;
            batch.frame_=s.frame;batch.pipelineLayout_=layout;batch.first_=first;batch.count_=count;
            batch.wrapper_=read<std::uintptr_t>(reinterpret_cast<void*>(s.frame),0x98);
            batch.mapped_=read<std::uintptr_t>(reinterpret_cast<void*>(s.frame),0xa0);
            batch.cursor_=read<std::uint64_t>(reinterpret_cast<void*>(s.frame),0xa8);
            for(unsigned j=0;j<count;++j){
                auto& set=batch.sets_[j];set.descriptor=descriptors[j];const auto* p=matched[j];if(!p)continue;
                set.converted=true;set.layout=p->raw->handle;set.generation=p->raw->generation;
                set.firstRange=batch.rangeCount_;set.rangeCount=p->raw->count;
                for(unsigned i=0;i<p->raw->count;++i){auto& r=batch.ranges_[batch.rangeCount_++];
                    r.set=first+j;r.binding=p->raw->bindings[i].slot;r.dynamicIndex=i;r.descriptor=p->set;
                    r.descriptorLayout=set.layout;r.layoutGeneration=set.generation;r.stages=p->raw->bindings[i].stages;
                    r.flags=p->raw->bindings[i].partial?VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT:0;
                    r.buffer=s.buffer;r.effectiveOffset=p->offsets[i];r.range=p->ranges[i];r.present=r.range!=0;
                }
            }
            batch.active_=true;
            if(batch.current())prepared=observer.prepare(observer.context,batch)&&batch.current();
            batch.active_=false;
        }
        if(!impl_->bufferCurrent92(*s.command,s.buffer,s.bufferGeneration92)){
            if(observer)observer.finish(observer.context,batch,false);fail();return;
        }
        errno=incoming;original(command,point,layout,first,count,descriptors,total,total?dynamic.data():nullptr);const int saved=errno;
        if(observer){
            batch.active_=true;const bool commit=prepared&&batch.current();batch.active_=false;
            observer.finish(observer.context,batch,commit);
            if(!commit)impl_->invalidateCapture(*s.command);
        }
        s.command->needsDescriptorBind=false;s.boundMatrix=matrix;
        impl_->count(t,&Tls::binds,impl_->binds);impl_->count(t,&Tls::offsets,impl_->offsets,total);errno=saved;
    }();
}
void DynamicUniformDescriptors::commandStateInvalidated(VkCommandBuffer command)noexcept{
    if(!ready())return;try{auto c=impl_->command(command);if(c){c->needsDescriptorBind=true;impl_->invalidateCapture(*c);}}catch(...){}
}
void DynamicUniformDescriptors::commandAllocated(VkDevice d,VkCommandPool p,VkCommandBuffer c)noexcept{
    if(!ready()||d!=impl_->device||!c)return;try{auto command=std::make_shared<Command>();command->device=d;command->commandPool=p;command->command=c;command->active=true;std::lock_guard lock(impl_->mutex);
        if(impl_->nextCaptureAllocation==UINT64_MAX)return;command->captureAllocation=++impl_->nextCaptureAllocation;
        const auto owner=impl_->ownedSets.acquire();command->owner=owner;
        if(!impl_->commands.emplace(c,std::move(command)).second){impl_->ownedSets.release(owner);++impl_->faults;return;}impl_->commandEpoch.fetch_add(1,std::memory_order_release);}catch(...){++impl_->faults;}
}
void DynamicUniformDescriptors::commandBegun(VkCommandBuffer c,VkResult r)noexcept{
    if(!ready()||r!=VK_SUCCESS)return;std::lock_guard lock(impl_->mutex);const auto it=impl_->commands.find(c);if(it!=impl_->commands.end()){it->second->recording=impl_->reset(*it->second);impl_->commandEpoch.fetch_add(1,std::memory_order_release);}
}
void DynamicUniformDescriptors::commandEnded(VkCommandBuffer c,VkResult r)noexcept{
    if(!ready()||r!=VK_SUCCESS)return;std::lock_guard lock(impl_->mutex);const auto it=impl_->commands.find(c);if(it!=impl_->commands.end()){impl_->invalidateCapture(*it->second);it->second->recording=false;++it->second->generation;impl_->commandEpoch.fetch_add(1,std::memory_order_release);}
}
void DynamicUniformDescriptors::commandReset(VkCommandBuffer c,VkResult r)noexcept{
    if(!ready()||r!=VK_SUCCESS)return;std::lock_guard lock(impl_->mutex);const auto it=impl_->commands.find(c);if(it!=impl_->commands.end()){impl_->reset(*it->second);impl_->commandEpoch.fetch_add(1,std::memory_order_release);}
}
void DynamicUniformDescriptors::commandFreed(VkDevice d,VkCommandPool p,VkCommandBuffer c)noexcept{
    if(d!=impl_->device)return;std::lock_guard lock(impl_->mutex);const auto it=impl_->commands.find(c);if(it==impl_->commands.end()||it->second->commandPool!=p)return;it->second->active=false;impl_->destroy(*it->second);impl_->ownedSets.release(it->second->owner);impl_->commands.erase(it);impl_->commandEpoch.fetch_add(1,std::memory_order_release);
}
void DynamicUniformDescriptors::poolReset(VkDevice d,VkCommandPool p,VkResult r)noexcept{
    if(d!=impl_->device||r!=VK_SUCCESS)return;std::lock_guard lock(impl_->mutex);for(auto& e:impl_->commands)if(e.second->commandPool==p)impl_->reset(*e.second);impl_->commandEpoch.fetch_add(1,std::memory_order_release);
}
void DynamicUniformDescriptors::poolDestroyed(VkDevice d,VkCommandPool p)noexcept{
    if(d!=impl_->device)return;std::lock_guard lock(impl_->mutex);for(auto it=impl_->commands.begin();it!=impl_->commands.end();)if(it->second->commandPool==p){it->second->active=false;impl_->destroy(*it->second);impl_->ownedSets.release(it->second->owner);it=impl_->commands.erase(it);}else ++it;impl_->commandEpoch.fetch_add(1,std::memory_order_release);
}
void DynamicUniformDescriptors::shutdownAfterIdle(VkDevice d)noexcept{
    if(d!=impl_->device)return;impl_->ready.store(false,std::memory_order_release);std::lock_guard lock(impl_->mutex);for(auto& e:impl_->commands){e.second->active=false;impl_->destroy(*e.second);}impl_->commands.clear();impl_->ownedSets.clear();impl_->layouts.clear();impl_->disableBuffers92();impl_->device={};impl_->layoutEpoch.fetch_add(1,std::memory_order_release);impl_->commandEpoch.fetch_add(1,std::memory_order_release);
}
void DynamicUniformDescriptors::abandonDevice(VkDevice d)noexcept{
    if(d!=impl_->device)return;impl_->ready.store(false,std::memory_order_release);std::lock_guard lock(impl_->mutex);
    for(auto& e:impl_->commands){e.second->active=false;e.second->recording=false;impl_->forget(*e.second);e.second->pools.clear();e.second->poolIndex=0;e.second->usedInPool=0;}
    impl_->commands.clear();impl_->ownedSets.clear();impl_->layouts.clear();impl_->disableBuffers92();impl_->device={};impl_->layoutEpoch.fetch_add(1,std::memory_order_release);impl_->commandEpoch.fetch_add(1,std::memory_order_release);
}

bool DynamicUniformDescriptors::CaptureBatch::current()const noexcept{
    return thread_==std::this_thread::get_id()&&owner_&&active_&&owner_->captureCurrent(*this);
}
bool DynamicUniformDescriptors::captureCurrent(const CaptureBatch& b)const noexcept{
    // TLS equality is checked before inspecting the lexical scope/state.
    const auto& t=tls_;auto* scope=t.drawScope;
    if(!ready()||t.drawActivity==UINT64_MAX||t.drawActivity!=b.drawActivity_||scope!=b.scope_||!scope||scope->owner_!=this||!scope->good_||!scope->qualified_||
       scope->state_!=b.state_||impl_->captureEpoch.load(std::memory_order_acquire)!=b.epoch_)return false;
    const auto& s=*scope->state_;const auto& c=*s.command;
    if(!c.active||!c.recording||c.generation!=b.commandEpoch_||c.captureSerial!=b.serial_||
       impl_->captureCommand(c)!=b.command_||s.frame!=b.frame_||!b.wrapper_||!b.mapped_||
       b.cursor_>s.size||b.mapped_>UINTPTR_MAX-s.size||
       read<std::uintptr_t>(s.context,0x30a0)!=b.frame_)return false;
    const auto* f=reinterpret_cast<const void*>(b.frame_);
    if(read<VkCommandBuffer>(f,0x38)!=c.command||read<std::uintptr_t>(f,0x98)!=b.wrapper_||
       read<std::uintptr_t>(f,0xa0)!=b.mapped_||read<std::uint64_t>(f,0xa8)!=b.cursor_)return false;
    const auto* w=reinterpret_cast<const void*>(b.wrapper_);
    if(read<VkBuffer>(w,0x10)!=s.buffer||read<std::uint32_t>(w,0x20)!=s.size)return false;
    for(unsigned i=0;i<b.rangeCount_;++i){const auto& r=b.ranges_[i];
        if(r.present&&(r.effectiveOffset>b.cursor_||r.range>b.cursor_-r.effectiveOffset))return false;
    }
    return true;
}
bool DynamicUniformDescriptors::CaptureBatch::copy(unsigned index,void* destination,std::size_t size)const noexcept{
    const int saved=errno;
    if(!current()||index>=rangeCount_||!destination)return false;
    const auto& r=ranges_[index];const auto dst=reinterpret_cast<std::uintptr_t>(destination);
    if(!r.present||size!=r.range||dst>UINTPTR_MAX-size)return false;
    const auto* scope=static_cast<const DrawScope*>(scope_);const auto capacity=scope->state_->size;
    if(dst<mapped_+capacity&&dst+size>mapped_)return false;
    std::memcpy(destination,reinterpret_cast<const void*>(mapped_+r.effectiveOffset),size);
    const bool result=current();errno=saved;return result;
}

std::string DynamicUniformDescriptors::report()const{
    std::size_t trackedBuffers{};bool coverage{};{std::lock_guard lock(impl_->mutex);trackedBuffers=impl_->buffers92.size();coverage=impl_->bufferCoverage92;}
    std::ostringstream o;o<<"{\"ready\":"<<ready()<<",\"convertedLayouts\":"<<impl_->converted<<",\"nativeLayouts\":"<<impl_->nativeLayouts<<",\"setCalls\":"<<impl_->calls<<",\"setHits\":"<<impl_->hits<<",\"descriptorWrites\":"<<impl_->writes<<",\"binds\":"<<impl_->binds<<",\"dynamicOffsets\":"<<impl_->offsets<<",\"faults\":"<<impl_->faults<<",\"resets\":"<<impl_->resets<<",\"retainedRecordings92\":"<<impl_->retainedRecordings92<<",\"poolsCreated\":"<<impl_->poolsCreated
     <<",\"memoHits\":"<<impl_->memoHits<<",\"retainedSets\":"<<impl_->retainedSets<<",\"forgets\":"<<impl_->forgets<<",\"forgetsBound\":"<<impl_->forgetsBound<<",\"poolsDestroyed\":"<<impl_->poolsDestroyed<<",\"ownedPeak\":"<<impl_->ownedPeak
     <<",\"bufferCoverage\":"<<coverage<<",\"bufferRoute\":"<<impl_->bufferRoute<<",\"bufferTracked\":"<<impl_->bufferTracked<<",\"bufferRetired\":"<<impl_->bufferRetired<<",\"trackedBuffers\":"<<trackedBuffers
     // debug.rdrvr.dud.fast (last value a drawing thread read) and its work counters; the per-thread
     // counts (setCalls/setHits/binds/dynamicOffsets too) are published every 256 scopes of that thread.
     <<",\"fast\":"<<impl_->fastOn.load(std::memory_order_relaxed)<<",\"fastScopes\":"<<impl_->fastScopes<<",\"fastLayoutHits\":"<<impl_->fastLayoutHits
     <<",\"fastLayoutMisses\":"<<impl_->fastLayoutMisses<<",\"fastLayoutFlushes\":"<<impl_->fastLayoutFlushes<<",\"fastCommandMisses\":"<<impl_->fastCommandMisses
     // cpu02: debug.rdrvr.dud.lean bits in effect (last drawing thread) and the dud.verify counts (every *Mismatches must stay 0).
     <<",\"lean\":"<<impl_->leanOn.load(std::memory_order_relaxed)<<",\"verify\":"<<impl_->verifyOn.load(std::memory_order_relaxed)<<",\"verifyChecks\":"<<impl_->verifyChecks
     <<",\"verifyKeyMismatches\":"<<impl_->verifyKeyMismatches<<",\"verifyCountMismatches\":"<<impl_->verifyCountMismatches<<",\"verifyFilterMismatches\":"<<impl_->verifyFilterMismatches<<'}';return o.str();
}
}
