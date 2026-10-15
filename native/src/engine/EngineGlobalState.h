#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>
#include <string>

namespace rdrvr::engine {
struct EngineGlobalApi {
    using Reader=bool(*)(void*,std::uintptr_t,void*,std::size_t);
    Reader read{};void* user{};
    void (*set)(int,const float*,int,int){};
    bool (*ownsThread)(){};
    std::uintptr_t globals{},buffers{},typeSizes{},commandBuffer{};
    explicit operator bool()const{return read&&set&&ownsThread&&globals&&buffers&&typeSizes&&commandBuffer;}
};
// Pinned grcEffect numeric globals. Values are copied from ConstantBuffer's CPU
// backing store and restored through SetGlobalFloatCommon, never into a Vulkan
// allocation. Texture globals are restored by the separate argument journal;
// samplerstate descriptors have no numeric payload and retain identity only.
// Place this outside both eye draws, but inside the game's render ownership.
class EngineGlobalState {
    struct Entry {
        int id{},type{},count{},offset{};
        std::uintptr_t buffer{},data{},layout{};std::uint32_t bytes{};
        std::vector<float> value;
    };
    EngineGlobalApi api_;const std::thread::id owner_{std::this_thread::get_id()};
    struct Signature {std::uintptr_t buffer{};std::int32_t offset{};unsigned char type{},count{};bool operator==(const Signature&)const=default;};
    std::array<Signature,240> signatures_{};
    std::array<unsigned char,16> typeSizes_{};
    struct BufferView {std::uintptr_t address{},data{},layout{},mapped{};std::uint32_t size{};};
    // Phase-local: emptied before capture and again before restore. No engine
    // callback runs while inspecting a phase on the owning render thread.
    std::array<BufferView,240> buffers_{};unsigned bufferCount_{};
    std::vector<Entry> entries_;std::int32_t count_{};bool active_{},failed_{};
    const char* status_{"NOT_CAPTURED"};int inspectedId_{};unsigned inspectedType_{},inspectedCount_{},inspectedUnit_{};
    int inspectedOffset_{};std::uint32_t inspectedSize_{};unsigned samplerCount_{};
    bool owner()const;
    bool signatures(int,std::array<Signature,240>&)const;
    bool inspect(Entry&,const Signature&,bool capture,bool readValue=true);
    bool readValues();
    bool header(std::int32_t&);
public:
    explicit EngineGlobalState(EngineGlobalApi api):api_(api){}
    ~EngineGlobalState(){restore();}
    EngineGlobalState(const EngineGlobalState&)=delete;
    EngineGlobalState& operator=(const EngineGlobalState&)=delete;
    bool capture() noexcept;
    bool restore() noexcept;
    void discardUnchanged() noexcept {active_=false;entries_.clear();}
    std::string report()const;
    bool good()const{return !failed_;}
    bool active()const{return active_;}
};
}
