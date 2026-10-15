#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <thread>

namespace rdrvr::engine {
struct EngineTextureApi {
    void (*global)(int,const void*){};
    void (*instance)(const void*,void*,int,const void*,bool,bool){};
    bool (*ownsThread)(){};
    explicit operator bool() const {return global && instance && ownsThread;}
};
// Journal for a synchronous, render-thread texture argument hook. It preserves
// the game's most recent requested value and restores it through the real APIs,
// including the bindless constant-buffer cache. It never edits shader objects.
// The hook owner must keep this scope and all borrowed instance data alive until
// finish, and must reject the stereo pair if good() or finish() is false.
class EngineTextureScope {
public:
    using Map=bool(*)(void*,const void*,const void**);
    static constexpr std::size_t capacity=64;
private:
    struct Entry {const void* effect{};void* data{};int id{};const void* requested{};bool global{};};
    EngineTextureApi api_;Map map_{};void* user_{};
    const std::thread::id owner_{std::this_thread::get_id()};
    std::array<Entry,capacity> entries_{};std::size_t count_{};
    std::atomic<bool> active_{},failed_{};
    std::atomic<const char*> status_{"NOT_BEGUN"};
    void fail(const char* reason){if(!failed_.exchange(true))status_=reason;}
    bool owner() const;
    const void* route(const void*,void*,int,const void*,bool);
public:
    EngineTextureScope(EngineTextureApi api,Map map,void* user):api_(api),map_(map),user_(user){}
    ~EngineTextureScope();
    EngineTextureScope(const EngineTextureScope&)=delete;
    EngineTextureScope& operator=(const EngineTextureScope&)=delete;
    bool begin() noexcept;
    void global(int,const void*);
    void instance(const void*,void*,int,const void*,bool,bool);
    bool finish() noexcept;
    bool good() const {return !failed_.load();}
    bool active() const {return active_.load();}
    const char* status()const{return status_.load();}
};
}
