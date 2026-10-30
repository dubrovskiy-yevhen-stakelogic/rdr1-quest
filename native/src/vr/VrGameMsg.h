#pragma once
// GAME MESSAGES: the game refuses some actions with a help message that cannot be seen in VR
// (APPLE: apple_feed_horse = 'Apples may only be used on horseback.'). Every message that reaches hudScript::SetHelpMessage is also shown as a small
// notice in the normal ink on the stats quad (the CLOCK NOTICE mechanism, runtime/HeatWarning.h, XrHarness), independent of the FPS readout and of the HUD row.
//
// Engine facts (librdr.so 1.58 arm64):
//  - hudScript::SetHelpMessage(text, seconds, a, b, c, d, e, f) 0x29fbacc: b = text is literal (hudScript::SetHelpFormat passes the resolved text), else it is a key;
//    RDRUIGame::AllocateMessage then calls UIStringTable::GetString(key) in the SAME call on the SAME thread. So the lookup here (UIStringTable::Get(const char*) 0x1aa1628,
//    read only, on the caller's thread, right before the native) adds no new thread to the table. Entry +8 = UTF-16 text (the way GetStringUTF8 0x1aa0c28 reads it).
//  - The message goes to queue 1 = the Flash object "HelpBox" (RDRUIGame::DisplayMessage, names at 0x2d693b8); shown only while hudManager+0xc3d8's +0x68 / +0x69 are clear
//    (RDRUIGame::Update). In VR it lands in the Flash area of the HUD image (top-left 1374x720 of 1680x1760), at the head quad's upper-left, 50% scale: very small, far from
//    the eyes. hudScript::IsShowingHelp 0x29fbd3c (HelpBox IsVisible) is polled on the main thread while a notice runs: report boxPolls / boxUp say whether the game's box was up too.
//  - Strings: strings_android.rpf; markup <grey> </grey> <0xfcaf17>, buttons {@PAUSE.PAUSE} {@FOOT.INTERACT:GPD}, unresolved {0} (see clean()).
//
// Threads: note on the game's thread (script / UI), inside the hook, under a short spin lock; frame / visible / lines on the XR thread only, frame takes the lock only when
// something is pending. The hook is the satchel evidence one: debug.rdrvr.vr.satchel.evidence 0 at boot = no hook, no notices.
// Levers: menu HUD > GAME MESSAGES (vr.cfg gameMessages, default OFF); debug.rdrvr.vr.gamemsg (read at 1 Hz): unset / -1 = the menu, 0 off, 1 on, 2 shows a test line.
// Report: vr.menu.satchel.lastHelpKey / lastHelpText / gameMessages.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::gamemsg {
inline constexpr std::uintptr_t stringGetOffset=0x1aa1628u,stringTableOffset=0x2f88b70u,showingHelpOffset=0x29fbd3cu,entryTextField=0x8u; // UIStringTable::Get(const char*), atSingleton<UIStringTable>::sm_Instance, hudScript::IsShowingHelp, txtStringData +8
inline constexpr std::array<unsigned char,24> stringGetFingerprint{
#include "gamecode/VrGameMsg_1aa1628_24_8h.inc"
};
inline constexpr std::array<unsigned char,24> showingHelpFingerprint{
#include "gamecode/VrGameMsg_29fbd3c_24_8h.inc"
};
inline constexpr const char* skippedKey="SC_CHALLENGE_AVAILABLE_MSG"; // SetHelpMessage drops this one itself
inline constexpr unsigned columns=52,maxLines=6,textChars=columns*maxLines+1,panelChars=(columns+1)*maxLines+1,keyChars=40,pendingSize=4,sourceLimit=1024;
inline constexpr std::uint64_t holdNs=30'000'000'000,maxContinuousNs=20'000'000'000,quietNs=30'000'000'000;
inline constexpr const char* testText="GAME MESSAGE TEST - THE GAME HELP TEXT SHOWS HERE";

// ---- text ----
struct Cleaned {unsigned latin{},other{};};
// Latin-1 letters 0xC0..0xFF, 0 = drop (the multiply / divide signs).
inline char foldLatin1(unsigned c) noexcept {
    static constexpr char table[65]="AAAAAAACEEEEIIIIDNOOOOO\0OUUUUYPSAAAAAAACEEEEIIIIDNOOOOO\0OUUUUYPY";
    return c>=0xc0u&&c<=0xffu?table[c-0xc0u]:char(0);
}
// Any glyph of the panel (VrMenuPages panelGlyph) or nothing: tags dropped, {@GROUP.NAME:MODE} = NAME, {0} dropped, accents folded, letters upper-cased, spaces collapsed.
// other = letters the panel has no form for (Cyrillic, CJK): when they dominate the caller falls back to the key, whose words are English.
template<class C> inline Cleaned clean(const C* in,char* out,std::size_t size) noexcept {
    Cleaned r{};std::size_t n=0;bool space=true;
    const auto put=[&](char c){
        if(c==' '){if(space)return;space=true;}else space=false;
        if(n+1<size)out[n++]=c;
    };
    const auto letter=[&](unsigned c){
        if(c>='a'&&c<='z')c-='a'-'A';
        if(c>='A'&&c<='Z'){++r.latin;put(char(c));}
        else if(c>='0'&&c<='9')put(char(c));
    };
    const C* p=in;
    for(unsigned guard=0;p&&*p&&guard<sourceLimit;++p,++guard){
        unsigned c=unsigned(std::make_unsigned_t<C>(*p));
        if(c=='\\'){
            // The table writes a line break as two backslashes + r (578 strings, 'to switch targets.\\rPull ..'): the r is no letter
            const C* e=p;
            while(e[1]=='\\')++e;
            if(e[1]=='r'||e[1]=='n'){++e;put(' ');}
            p=e;
            continue;
        }
        if(c=='<'){
            const C* e=p+1;unsigned k=0;
            while(*e&&*e!='>'&&k<24){++e;++k;}
            if(*e=='>')p=e;
            continue;
        }
        if(c=='{'){
            const C* e=p+1;unsigned k=0;
            while(*e&&*e!='}'&&k<64){++e;++k;}
            if(*e!='}')continue;
            if(p[1]=='@'){
                const C* name=p+2;
                for(const C* q=p+2;q<e&&*q!=':';++q)if(*q=='.')name=q+1;
                for(const C* q=name;q<e&&*q!=':';++q){const unsigned v=unsigned(std::make_unsigned_t<C>(*q));if(v=='_')put(' ');else letter(v);}
                if(e[1]=='{'&&e[2]=='@')put(' ');
            }
            p=e;
            continue;
        }
        if constexpr(sizeof(C)==1){
            if(c>=0x80u){
                if((c&0xe0u)==0xc0u&&(unsigned(std::make_unsigned_t<C>(p[1]))&0xc0u)==0x80u){c=(c&0x1fu)<<6|(unsigned(std::make_unsigned_t<C>(p[1]))&0x3fu);++p;}
                else{if(c>=0xe0u)++r.other;continue;}
            }
        }
        if(c<=' '||c==0xa0u||c=='_'||c=='&')put(' ');
        else if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'))letter(c);
        else if(c=='.'||c==':'||c=='/'||c=='-'||c=='%'||c=='?')put(char(c));
        else if(c=='!')put('.');
        else if(c==';')put(':');
        else if(c==0x2013u||c==0x2014u)put('-');
        else if(c>=0xc0u&&c<=0xffu){if(const char f=foldLatin1(c)){++r.latin;put(f);}}
        else if(c>=0x100u&&!(c>=0x2000u&&c<0x2c00u))++r.other;
    }
    while(n&&out[n-1]==' ')--n;
    if(size)out[n]=0;
    return r;
}
// Word wrap to the panel's width, '\n' between the lines; what does not fit ends in "..". Returns the line count, longest = the widest line in glyphs.
inline unsigned wrap(const char* text,char* out,std::size_t size,unsigned& longest) noexcept {
    longest=0;std::size_t n=0;unsigned lines=0,column=0;
    if(!size)return 0;
    out[0]=0;
    const auto emit=[&](char c){if(n+1<size)out[n++]=c;};
    const auto newLine=[&]{longest=std::max(longest,column);if(column){emit('\n');++lines;column=0;}};
    bool cut=false;
    for(const char* p=text;*p&&!cut;){
        while(*p==' ')++p;
        unsigned word=0;
        while(p[word]&&p[word]!=' ')++word;
        if(!word)break;
        if(column&&column+1+word>columns)newLine();
        if(lines>=maxLines){cut=true;break;}
        if(column){emit(' ');++column;}
        for(unsigned i=0;i<word;++i){
            if(column==columns){newLine();if(lines>=maxLines){cut=true;break;}}
            emit(p[i]);++column;
        }
        p+=word;
    }
    if(column)newLine();
    if(n&&out[n-1]=='\n')--n;
    out[n]=0;
    if(cut&&n){
        // Last line ends in ".." inside the width
        std::size_t start=n;
        while(start&&out[start-1]!='\n')--start;
        if(n-start+2>columns)n=start+columns-2;
        out[n++]='.';out[n++]='.';out[n]=0;
        longest=std::max(longest,unsigned(n-start));
    }
    return lines;
}
// How long a message stays: the game's own duration (or 6 s when it has none), never shorter than a slow read, 15 s at most.
inline std::uint64_t readNs(std::size_t chars) noexcept {return 2'500'000'000ull+std::uint64_t(chars)*55'000'000ull;}
inline std::uint64_t durationNs(float seconds,std::size_t chars) noexcept {
    const float s=seconds>0.f?std::clamp(seconds,3.f,12.f):6.f;
    return std::min<std::uint64_t>(std::max(std::uint64_t(double(s)*1e9),readNs(chars)),15'000'000'000ull);
}

// ---- game thread -> XR thread ----
struct Pending {std::uint64_t ns{},seq{};float seconds{};char text[textChars]{};};
inline Pending pendingRing[pendingSize]{};
inline std::uint64_t pendingHead{},pendingTail{},nextSeq{},lastSeenNs{};
inline char lastPushed[textChars]{},lastKey[keyChars]{};
inline std::atomic_flag ringLock=ATOMIC_FLAG_INIT;
struct RingGuard {RingGuard() noexcept {while(ringLock.test_and_set(std::memory_order_acquire)){}} ~RingGuard() {ringLock.clear(std::memory_order_release);}};
inline std::atomic<std::uint64_t> pendingCount{},lastSeq{},doneSeq{},cappedSeq{},repeatSeq{},repeatUntilNs{};
inline std::atomic<std::uint64_t> seen{},decoded{},literal{},missing{},foreign{},skipped{},queued{},shown{},repeats{},ignored{},dropped{},boxPolls{},boxUp{};
inline std::atomic<bool> enabledNow{true},wantBox{false};
inline std::atomic<int> propertyValue{-1};
inline void copyKey(const char* key) noexcept {
    std::size_t n=0;
    if(key)for(;n+1<keyChars&&key[n];++n)lastKey[n]=key[n]>=32&&key[n]<127&&key[n]!='"'&&key[n]!='\\'?key[n]:'?';
    lastKey[n]=0;
}
// Game thread, inside the hook: text = already clean (clean()), key = what the game passed (report only).
inline void note(const char* key,const char* text,float seconds,std::uint64_t now) noexcept {
    if(!text||!*text)return;
    const std::size_t chars=std::strlen(text);
    RingGuard guard;
    if(key)copyKey(key);
    const auto seq=lastSeq.load(std::memory_order_relaxed);
    if(seq&&!std::strcmp(lastPushed,text)){
        // A script that repeats its message every frame: the same notice stays, up to maxContinuousNs, then it keeps quiet while the repeats go on.
        const auto before=lastSeenNs;lastSeenNs=now;
        if(cappedSeq.load(std::memory_order_relaxed)==seq){
            if(now>=before&&now-before<quietNs){ignored.fetch_add(1,std::memory_order_relaxed);return;}
        }else if(doneSeq.load(std::memory_order_relaxed)<seq){
            repeatUntilNs.store(now+durationNs(seconds,chars),std::memory_order_relaxed);repeatSeq.store(seq,std::memory_order_relaxed);
            repeats.fetch_add(1,std::memory_order_relaxed);return;
        }
    }
    if(pendingTail-pendingHead>=pendingSize){++pendingHead;dropped.fetch_add(1,std::memory_order_relaxed);}
    auto& p=pendingRing[pendingTail++%pendingSize];
    p.ns=now;p.seq=++nextSeq;p.seconds=seconds;
    std::snprintf(p.text,sizeof p.text,"%s",text);
    std::snprintf(lastPushed,sizeof lastPushed,"%s",text);
    lastSeenNs=now;
    lastSeq.store(p.seq,std::memory_order_relaxed);
    pendingCount.store(pendingTail-pendingHead,std::memory_order_release);
    queued.fetch_add(1,std::memory_order_relaxed);
}
inline void noteBox(bool up) noexcept {boxPolls.fetch_add(1,std::memory_order_relaxed);if(up)boxUp.fetch_add(1,std::memory_order_relaxed);}

// ---- XR thread only ----
struct Current {
    char lines[panelChars]{};
    unsigned lineCount{},longest{};
    std::uint64_t seq{},queuedNs{},firstNs{},untilNs{},durNs{},readTimeNs{};
    bool active{};
};
inline Current current{};
inline std::uint64_t nextProperty{},drawnSeq{};
inline bool testFired{};
inline void finish(Current& c,bool capped) noexcept {
    if(capped)cappedSeq.store(c.seq,std::memory_order_relaxed);
    doneSeq.store(c.seq,std::memory_order_relaxed);c.active=false;c.firstNs=0;c.lineCount=0;c.lines[0]=0;
    wantBox.store(false,std::memory_order_relaxed);
}
inline bool popPending(Current& c) noexcept {
    if(!pendingCount.load(std::memory_order_acquire))return false;
    Pending p{};
    {
        RingGuard guard;
        if(pendingHead==pendingTail)return false;
        p=pendingRing[pendingHead++%pendingSize];
        pendingCount.store(pendingTail-pendingHead,std::memory_order_release);
    }
    const auto chars=std::strlen(p.text);
    c.lineCount=wrap(p.text,c.lines,sizeof c.lines,c.longest);
    c.seq=p.seq;c.queuedNs=p.ns;c.firstNs=0;c.durNs=durationNs(p.seconds,chars);c.readTimeNs=std::min(c.durNs,readNs(chars));c.active=c.lineCount>0;
    if(!c.active)doneSeq.store(c.seq,std::memory_order_relaxed);
    return c.active;
}
inline void pollProperty(std::uint64_t now) noexcept {
    if(now<nextProperty)return;
    nextProperty=now+1'000'000'000ull;
#if defined(__ANDROID__)
    char v[PROP_VALUE_MAX]{};
    propertyValue.store(__system_property_get("debug.rdrvr.vr.gamemsg",v)>0&&v[0]>='0'&&v[0]<='2'&&!v[1]?int(v[0]-'0'):-1,std::memory_order_relaxed);
#endif
}
// Every XR frame (stats quad). setting = the menu row, canShow = nothing else owns the quad now (VR menu, welcome). The clock of a message starts when it is really on the quad.
// true = the quad must be redrawn now.
inline bool frame(std::uint64_t now,bool setting,bool canShow) noexcept {
    pollProperty(now);
    const int property=propertyValue.load(std::memory_order_relaxed);
    const bool on=property>=0?property!=0:setting;
    enabledNow.store(on,std::memory_order_relaxed);
    auto& c=current;
    if(!on){
        if(c.active)finish(c,false);
        while(popPending(c))finish(c,false);
    }else{
        if(property==2&&!testFired){testFired=true;note(nullptr,testText,8.f,now);}
        if(property!=2)testFired=false;
        if(c.active&&c.firstNs){
            if(repeatSeq.load(std::memory_order_relaxed)==c.seq)c.untilNs=std::max(c.untilNs,std::min(repeatUntilNs.load(std::memory_order_relaxed),c.firstNs+maxContinuousNs));
            const bool next=pendingCount.load(std::memory_order_relaxed)&&now>=c.firstNs+c.readTimeNs;
            if(now>=c.untilNs||next)finish(c,now>=c.firstNs+maxContinuousNs);
        }else if(c.active&&now>=c.queuedNs&&now-c.queuedNs>holdNs){
            dropped.fetch_add(1,std::memory_order_relaxed);finish(c,false);
        }
        while(!c.active&&popPending(c)){
            if(now>=c.queuedNs&&now-c.queuedNs>holdNs){dropped.fetch_add(1,std::memory_order_relaxed);finish(c,false);}
        }
        if(c.active&&!c.firstNs&&canShow){
            c.firstNs=now;c.untilNs=now+c.durNs;shown.fetch_add(1,std::memory_order_relaxed);
            wantBox.store(true,std::memory_order_relaxed);
        }
    }
    const std::uint64_t onQuad=c.active&&c.firstNs&&canShow?c.seq:0; // canShow: the menu / welcome panel gave the quad back, its pixels are still theirs
    if(onQuad==drawnSeq)return false;
    drawnSeq=onQuad;
    return true;
}
inline bool visible() noexcept {return current.active&&current.firstNs!=0;}
inline const char* lines() noexcept {return current.lines;}
inline unsigned lineCount() noexcept {return current.lineCount;}
inline unsigned longest() noexcept {return current.longest;}

// Report thread: appended to vr.menu.satchel.
inline void appendReport(std::string& out) {
    char key[keyChars]{},text[textChars]{};
    {RingGuard guard;std::memcpy(key,lastKey,sizeof key);std::memcpy(text,lastPushed,sizeof text);}
    char b[textChars+keyChars+560];
    const auto u=[](const std::atomic<std::uint64_t>& v){return (unsigned long long)v.load(std::memory_order_relaxed);};
    std::snprintf(b,sizeof b,",\"lastHelpKey\":\"%s\",\"lastHelpText\":\"%s\",\"gameMessages\":{\"on\":%s,\"property\":%d,\"seen\":%llu,\"decoded\":%llu,\"literal\":%llu,\"missing\":%llu,\"foreign\":%llu,\"skipped\":%llu,"
        "\"queued\":%llu,\"shown\":%llu,\"repeats\":%llu,\"ignored\":%llu,\"dropped\":%llu,\"boxPolls\":%llu,\"boxUp\":%llu}",
        key,text,enabledNow.load(std::memory_order_relaxed)?"true":"false",propertyValue.load(std::memory_order_relaxed),u(seen),u(decoded),u(literal),u(missing),u(foreign),u(skipped),
        u(queued),u(shown),u(repeats),u(ignored),u(dropped),u(boxPolls),u(boxUp));
    out+=b;
}
} // namespace rdrvr::vr::gamemsg
