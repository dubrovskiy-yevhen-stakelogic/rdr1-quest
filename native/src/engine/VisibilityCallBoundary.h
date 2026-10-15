#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>

namespace rdrvr::engine {
class VisibilityCallBoundary {
public:
    using Original=void(*)(void*,void*,const void*);
    using Begin=std::uint64_t(*)(void*,void*,const void*);
    using Finish=void(*)(std::uint64_t,bool) noexcept;
private:
    Original original_{};Begin begin_{};Finish finish_{};
    inline static thread_local unsigned depth_{};
    inline static thread_local const VisibilityCallBoundary* active_{};
    inline static thread_local void* renderer_{};
    inline static thread_local void* viewport_{};
    inline static thread_local const void* camera_{};
    inline static thread_local std::uint64_t serial_{};
    std::atomic<std::uint64_t> errors_{},nested_{};
public:
    void configure(Original original,Begin begin,Finish finish){original_=original;begin_=begin;finish_=finish;}
    void invoke(void* renderer,void* viewport,const void* camera){
        const int incoming=errno;std::uint64_t serial{};bool success{};
        if(depth_++)++nested_;
        else try{serial=begin_(renderer,viewport,camera);}catch(...){++errors_;}
        struct End {std::uint64_t& serial;bool& success;Finish finish;
            const VisibilityCallBoundary* previous;void* renderer;void* viewport;const void* camera;std::uint64_t token;
            ~End(){const int e=errno;active_=previous;renderer_=renderer;viewport_=viewport;camera_=camera;serial_=token;if(serial)finish(serial,success);--depth_;errno=e;}
        } end{serial,success,finish_,active_,renderer_,viewport_,camera_,serial_};
        active_=this;renderer_=renderer;viewport_=viewport;camera_=camera;serial_=serial;
        errno=incoming;original_(renderer,viewport,camera);success=true;
    }
    std::uint64_t serial(void* renderer)const{return depth_==1&&active_==this&&renderer_==renderer?serial_:0;}
    std::uint64_t serial(void* renderer,void* viewport,const void* camera)const{return viewport_==viewport&&camera_==camera?serial(renderer):0;}
    std::uint64_t errors()const{return errors_;}
    std::uint64_t nested()const{return nested_;}
};
} // namespace rdrvr::engine
