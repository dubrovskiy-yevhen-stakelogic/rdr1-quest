#pragma once
#include "EngineTextureMap.h"
#include "RetailSceneBoundary.h"
namespace rdrvr::engine {
struct RetailResourceApi {
    PassSourceAbi memory{};bool (*ownsThread)(){};bool (*currentGfx)(void**){};
    std::uintptr_t factory{},device{},collector{},postfx{},lights{},timer{};
    std::uintptr_t factoryVtable{},deviceVtable{},gfxVtable{},framebufferVtable{};
    explicit operator bool()const{return memory.read&&ownsThread&&currentGfx&&factory&&device&&collector&&postfx&&lights&&timer&&factoryVtable&&deviceVtable&&gfxVtable&&framebufferVtable;}
};
struct RetailResourceSet {
    void *factory{},*device{},*collector{},*postfx{},*gfx{},*context{},*framebuffer{},*depth{},*sampled{};
    std::uint32_t timerOrdinal{};EngineTargetSpec spec{};
    std::array<StockSceneTarget,8> stock{};std::array<TargetImage,8> images{};unsigned count{};
    std::span<const StockSceneTarget> sources()const{return {stock.data(),count};}
};
// Read-only discovery at the main color return. Actual bound framebuffer must
// agree with postFX color and the collector's depth; no guessed target is used.
class RetailResources {
    const char* status_{"NOT_INSPECTED"};const char* targetStatus_{"NOT_READ"};
    int sourceIndex_{-1};std::array<ImageRecord,8> observed_{};
    bool boundRead_{},boundColorMatches_{},boundDepthMatches_{};
    RetailResourceApi api_;PassResources& images_;VkDevice device_{};
public:
    RetailResources(RetailResourceApi api,PassResources& images,VkDevice device):api_(api),images_(images),device_(device){}
    bool inspect(const RetailSceneFrame&,RetailResourceSet&,bool includeWater=false) noexcept;
    const char* status()const noexcept{return status_;}
    std::string report()const;
    static bool same(const RetailResourceSet&,const RetailResourceSet&) noexcept;
    static const char* difference(const RetailResourceSet&,const RetailResourceSet&) noexcept;
};
}
