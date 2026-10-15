#pragma once
// The game keeps its saves (RDR2SAVE0..3.SAV etc., d11::AndroidSaveDataManager_c) in its private
// storage, which adb cannot reach on a non-debuggable APK; this library runs inside the game
// process with the app's uid. At JNI_OnLoad (before the game opens any save):
//  1. walks the private data dir (depth <= 5, skips cache/code_cache/lib/oat/app_webview) and lists every file whose name ends
//     in .SAV (any case) into <rdrvr>/save-files.txt (relative path, size, mtime);
//  2. copies each of them to <rdrvr>/save-export/<relative path with '/' -> '__'> (a fresh copy of the current saves);
//  3. IMPORT: for every file <rdrvr>/save-import/<relative path with '/' -> '__'>, backs the private file up to
//     <rdrvr>/save-backup/<unix time>/ (when it exists), writes the import over it (tmp + rename) and renames the import to
//     <name>.done so it is applied once. A plain file name that matches no listed save is written next to the first listed
//     save, or skipped when there is none.
// Everything is logged to <rdrvr>/save-transfer.log. debug.rdrvr.save.transfer 0 disables the whole step.
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::diagnostics::savexfer {
inline constexpr const char* rdrvrDir="/sdcard/Android/data/com.rockstargames.rdr/files/rdrvr";
inline constexpr const char* dataDirs[]={"/data/user/0/com.rockstargames.rdr","/data/data/com.rockstargames.rdr"};
struct Found {std::string rel,abs;long long size{},mtime{};};
inline bool endsWithSav(const char* n) noexcept {
    const std::size_t l=std::strlen(n);if(l<5)return false;
    const char* e=n+l-4;return e[0]=='.'&&(e[1]=='S'||e[1]=='s')&&(e[2]=='A'||e[2]=='a')&&(e[3]=='V'||e[3]=='v');
}
inline bool skipDir(const char* n) noexcept {
    static constexpr const char* skip[]{".","..","cache","code_cache","lib","oat","app_webview"};
    for(const char* s:skip)if(!std::strcmp(n,s))return true;
    return false;
}
inline void walk(const std::string& root,const std::string& rel,int depth,std::vector<Found>& out,unsigned& dirs) {
    if(depth>5||dirs>4000)return;
    DIR* d=opendir((rel.empty()?root:root+"/"+rel).c_str());if(!d)return;++dirs;
    while(dirent* e=readdir(d)){
        if(skipDir(e->d_name))continue;
        const std::string r=rel.empty()?std::string(e->d_name):rel+"/"+e->d_name,a=root+"/"+r;
        struct stat st{};if(lstat(a.c_str(),&st)!=0)continue;
        if(S_ISDIR(st.st_mode))walk(root,r,depth+1,out,dirs);
        else if(S_ISREG(st.st_mode)&&endsWithSav(e->d_name))out.push_back({r,a,(long long)st.st_size,(long long)st.st_mtime});
    }
    closedir(d);
}
inline std::string flat(const std::string& rel) {std::string o;for(char c:rel){if(c=='/')o+="__";else o+=c;}return o;}
inline bool copyFile(const std::string& from,const std::string& to) {
    std::FILE* in=std::fopen(from.c_str(),"rb");if(!in)return false;
    const std::string tmp=to+".tmp";std::FILE* out=std::fopen(tmp.c_str(),"wb");if(!out){std::fclose(in);return false;}
    char buf[65536];bool ok=true;std::size_t n;
    while((n=std::fread(buf,1,sizeof buf,in))>0)if(std::fwrite(buf,1,n,out)!=n){ok=false;break;}
    ok=ok&&!std::ferror(in);std::fclose(in);
    ok=std::fflush(out)==0&&ok;ok=fsync(fileno(out))==0&&ok;ok=std::fclose(out)==0&&ok;
    if(!ok||std::rename(tmp.c_str(),to.c_str())!=0){std::remove(tmp.c_str());return false;}
    return true;
}
inline void run() noexcept {
    try{
#if defined(__ANDROID__)
        char v[PROP_VALUE_MAX]{};if(__system_property_get("debug.rdrvr.save.transfer",v)>0&&v[0]=='0'&&!v[1])return;
#endif
        const int savedErrno=errno;
        const std::string base=rdrvrDir;mkdir((base+"/save-export").c_str(),0770);
        std::FILE* log=std::fopen((base+"/save-transfer.log").c_str(),"a");
        const auto say=[&](const char* what,const std::string& a,const char* result){if(log){std::fprintf(log,"%s %s %s\n",what,a.c_str(),result);std::fflush(log);}};
        const long long now=(long long)std::time(nullptr);
        if(log)std::fprintf(log,"=== %lld save01 start\n",now);
        std::string root;std::vector<Found> found;unsigned dirs=0;
        for(const char* r:dataDirs){struct stat st{};if(stat(r,&st)==0&&S_ISDIR(st.st_mode)){root=r;break;}}
        if(root.empty()){say("root","-","NOT FOUND");if(log)std::fclose(log);errno=savedErrno;return;}
        walk(root,"",0,found,dirs);
        std::sort(found.begin(),found.end(),[](const Found& a,const Found& b){return a.rel<b.rel;});
        if(std::FILE* list=std::fopen((base+"/save-files.txt").c_str(),"w")){
            std::fprintf(list,"# save01 %lld root %s dirs %u files %zu\n",now,root.c_str(),dirs,found.size());
            for(const auto& f:found)std::fprintf(list,"%s\t%lld\t%lld\n",f.rel.c_str(),f.size,f.mtime);
            std::fclose(list);
        }
        for(const auto& f:found)say("export",f.rel,copyFile(f.abs,base+"/save-export/"+flat(f.rel))?"OK":"FAILED");
        const std::string imp=base+"/save-import";
        if(DIR* d=opendir(imp.c_str())){
            std::string backup;
            while(dirent* e=readdir(d)){
                const std::string name=e->d_name;if(!endsWithSav(name.c_str()))continue;
                std::string targetRel;for(const auto& f:found)if(flat(f.rel)==name){targetRel=f.rel;break;}
                if(targetRel.empty()){
                    if(found.empty()){say("import",name,"SKIPPED (no save dir known)");continue;}
                    // a NEW file goes to its own flattened path ("files__RDR2SAVE1.SAV" -> files/RDR2SAVE1.SAV); before, it
                    // landed next to the first save under its flat name (files/files__RDR2SAVE1.SAV, never read by the game).
                    for(std::size_t i=0;i<name.size();++i){if(i+1<name.size()&&name[i]=='_'&&name[i+1]=='_'){targetRel+='/';++i;}else targetRel+=name[i];}
                    say("import",name+" -> "+targetRel,"NEW FILE");
                }
                const std::string target=root+"/"+targetRel;
                struct stat st{};
                if(stat(target.c_str(),&st)==0){
                    if(backup.empty()){backup=base+"/save-backup";mkdir(backup.c_str(),0770);backup+="/"+std::to_string(now);mkdir(backup.c_str(),0770);}
                    say("backup",targetRel,copyFile(target,backup+"/"+flat(targetRel))?"OK":"FAILED");
                }
                // an EMPTY import file deletes that save (after the backup above), e.g. To drop an imported slot again
                struct stat is{};const bool erase=stat((imp+"/"+name).c_str(),&is)==0&&is.st_size==0;
                if(erase){const bool gone=std::remove(target.c_str())==0||errno==ENOENT;say("delete",targetRel,gone?"OK":"FAILED");
                    if(gone)std::rename((imp+"/"+name).c_str(),(imp+"/"+name+".done").c_str());continue;}
                const bool ok=copyFile(imp+"/"+name,target);
                say("import",targetRel,ok?"OK":"FAILED");
                if(ok)std::rename((imp+"/"+name).c_str(),(imp+"/"+name+".done").c_str());
            }
            closedir(d);
        }
        if(log){std::fprintf(log,"=== save01 end (%zu saves)\n",found.size());std::fclose(log);}
        errno=savedErrno;
    }catch(...){}
}
}
