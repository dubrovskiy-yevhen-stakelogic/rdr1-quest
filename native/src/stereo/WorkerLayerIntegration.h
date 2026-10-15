#pragma once
#include "WorkerLayerRequest.h"
#include "WorkerLayerBuildId.h"
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <cstddef>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sys/system_properties.h>

namespace rdr2_game_worker {
inline constexpr const char* LibraryName="libVkLayer_RDR2_raw_worker28.so";
using OwnsEntry=uint32_t (*)(PFN_vkVoidFunction);
using ConfigureOnce=uint32_t (*)(uint32_t,uint32_t);
using DiagnosticNonce=void (*)(uint64_t);
struct Module final { void* retained{}; void* base{}; OwnsEntry owns{}; DiagnosticNonce diagnostic{}; bool ready{}; };
inline Module module;
inline std::once_flag loadOnce;

inline bool expectedBuildId(const unsigned char* data, size_t size) noexcept {
    while (size>=sizeof(Elf64_Nhdr)) {
        Elf64_Nhdr note{}; std::memcpy(&note,data,sizeof(note));
        data+=sizeof(note); size-=sizeof(note);
        const size_t names=(static_cast<size_t>(note.n_namesz)+3u)&~size_t(3);
        const size_t desc=(static_cast<size_t>(note.n_descsz)+3u)&~size_t(3);
        if (names>size || desc>size-names) return false;
        if (note.n_type==NT_GNU_BUILD_ID && note.n_namesz==4 &&
            std::memcmp(data,"GNU\0",4)==0 && note.n_descsz==sizeof(ExpectedLayerBuildId) &&
            std::memcmp(data+names,ExpectedLayerBuildId,sizeof(ExpectedLayerBuildId))==0) return true;
        data+=names+desc; size-=names+desc;
    }
    return false;
}
struct Identity final { void* base{}; bool matched{}; };
inline int matchModule(dl_phdr_info* info,size_t,void* context) noexcept {
    auto& identity=*static_cast<Identity*>(context);
    if (static_cast<uintptr_t>(info->dlpi_addr)!=reinterpret_cast<uintptr_t>(identity.base)) return 0;
    for (ElfW(Half) i=0;i<info->dlpi_phnum;++i) {
        const auto& note=info->dlpi_phdr[i];
        if (note.p_type!=PT_NOTE || !note.p_memsz) continue;
        // Read only notes contained in a readable load segment of this module.
        bool readable=false;
        for (ElfW(Half) j=0;j<info->dlpi_phnum;++j) {
            const auto& load=info->dlpi_phdr[j];
            if (load.p_type==PT_LOAD && (load.p_flags&PF_R) && note.p_vaddr>=load.p_vaddr &&
                note.p_vaddr-load.p_vaddr<=load.p_memsz &&
                note.p_memsz<=load.p_memsz-(note.p_vaddr-load.p_vaddr)) {readable=true;break;}
        }
        if (readable && expectedBuildId(reinterpret_cast<const unsigned char*>(info->dlpi_addr+note.p_vaddr),note.p_memsz)) {
            identity.matched=true;break;
        }
    }
    return 1;
}
inline bool available() noexcept {
    try {
        std::call_once(loadOnce,[]{
            // Read once before the first Vulkan instance. A process restart is
            // required to change this control; it never unloads a live layer.
            char selection88[PROP_VALUE_MAX]{};
            const int selectionLength88=__system_property_get("debug.rdrvr.worker_layer",selection88);
            if(selectionLength88==1&&selection88[0]=='0') {
                __android_log_print(ANDROID_LOG_INFO,"RdrWorker88","startupLayer=0 source=explicit-property moduleLoaded=0");
                return;
            }
            __android_log_print(ANDROID_LOG_INFO,"RdrWorker88","startupLayer=1 source=%s",selectionLength88==1&&selection88[0]=='1'?"explicit-property":"default");
            // Keep the module loaded for the full Vulkan/PFN lifetime.
            module.retained=dlopen(LibraryName,RTLD_NOW|RTLD_LOCAL);
            const char* reason="LIBRARY_UNAVAILABLE";
            if (module.retained) {
                auto owns=reinterpret_cast<OwnsEntry>(dlsym(module.retained,"rdrWorker28OwnsEntry"));
                auto configure=reinterpret_cast<ConfigureOnce>(dlsym(module.retained,"rdrWorker28ConfigureOnce"));
                auto diagnostic=reinterpret_cast<DiagnosticNonce>(dlsym(module.retained,"rdrWorker74DiagnosticNonce"));
                Dl_info ownInfo{},configInfo{},diagnosticInfo{};
                reason="EXPORT_IDENTITY_REJECTED";
                if (owns && configure && diagnostic && dladdr(reinterpret_cast<void*>(diagnostic),&diagnosticInfo) && dladdr(reinterpret_cast<void*>(owns),&ownInfo) &&
                    dladdr(reinterpret_cast<void*>(configure),&configInfo) && ownInfo.dli_fbase &&
                    ownInfo.dli_fbase==configInfo.dli_fbase && ownInfo.dli_fbase==diagnosticInfo.dli_fbase && ownInfo.dli_fname) {
                    const auto* name=std::strrchr(ownInfo.dli_fname,'/');name=name?name+1:ownInfo.dli_fname;
                    Identity identity{ownInfo.dli_fbase,false};
                    if (std::strcmp(name,LibraryName)==0) dl_iterate_phdr(matchModule,&identity);
                    if (identity.matched) {
                        reason="CONFIGURATION_REJECTED";
                        // mode 2 = permanent pass-through. The layer stays in the chain
                        // (ownsEntry/diagnostic nonce keep working) but never creates a worker and
                        // forwards every call directly: no per-call admission accounting, atomics
                        // or TLS work. Older layers reject 2, so a layer that would still pay the
                        // protocol can never be activated by mistake.
                        if (configure(1,2)==1) {module.base=identity.base;module.owns=owns;module.diagnostic=diagnostic;module.ready=true;reason="READY";}
                    }
                }
            }
            __android_log_print(module.ready?ANDROID_LOG_INFO:ANDROID_LOG_ERROR,"RdrWorker28","activation %s",reason);
        });
        return module.ready;
    } catch (...) {return false;}
}
inline bool ownsEntry(uintptr_t address) noexcept {
    if (!address || !available()) return false;
    Dl_info info{};
    return dladdr(reinterpret_cast<void*>(address),&info) && info.dli_fbase==module.base &&
        module.owns(reinterpret_cast<PFN_vkVoidFunction>(address))==1;
}
inline void diagnosticWindow74(uint64_t nonce) noexcept {
    const int incoming=errno;
    if (available() && module.diagnostic) module.diagnostic(nonce);
    errno=incoming;
}
}
