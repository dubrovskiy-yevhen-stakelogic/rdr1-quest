#pragma once
#include <algorithm>
#include <array>
#include <string_view>
#include <cstdint>
namespace rdrvr::diagnostics {
// Small original bitmap alphabet. No font engine, assets or GPU draw calls.
class PerfPanel {
    static std::array<unsigned char,7> glyph(char c){
        switch(c){
        case '0':return {14,17,19,21,25,17,14};case '1':return {4,12,4,4,4,4,14};
        case '2':return {14,17,1,2,4,8,31};case '3':return {30,1,1,14,1,1,30};
        case '4':return {2,6,10,18,31,2,2};case '5':return {31,16,16,30,1,1,30};
        case '6':return {14,16,16,30,17,17,14};case '7':return {31,1,2,4,8,8,8};
        case '8':return {14,17,17,14,17,17,14};case '9':return {14,17,17,15,1,1,14};
        case 'A':return {14,17,17,31,17,17,17};case 'B':return {30,17,17,30,17,17,30};
        case 'C':return {14,17,16,16,16,17,14};case 'D':return {30,17,17,17,17,17,30};
        case 'E':return {31,16,16,30,16,16,31};case 'F':return {31,16,16,30,16,16,16};
        case 'G':return {14,17,16,23,17,17,15};case 'H':return {17,17,17,31,17,17,17};
        case 'I':return {14,4,4,4,4,4,14};case 'J':return {7,2,2,2,2,18,12};
        case 'K':return {17,18,20,24,20,18,17};case 'L':return {16,16,16,16,16,16,31};
        case 'M':return {17,27,21,21,17,17,17};case 'N':return {17,25,21,19,17,17,17};
        case 'O':return {14,17,17,17,17,17,14};case 'P':return {30,17,17,30,16,16,16};
        case 'Q':return {14,17,17,17,21,18,13};case 'R':return {30,17,17,30,20,18,17};
        case 'S':return {15,16,16,14,1,1,30};case 'T':return {31,4,4,4,4,4,4};
        case 'U':return {17,17,17,17,17,17,14};case 'V':return {17,17,17,17,17,10,4};
        case 'W':return {17,17,17,21,21,27,17};case 'X':return {17,17,10,4,10,17,17};
        case 'Y':return {17,17,10,4,4,4,4};case 'Z':return {31,1,2,4,8,16,31};
        case '.':return {0,0,0,0,0,12,12};case ':':return {0,12,12,0,12,12,0};
        case '/':return {1,2,2,4,8,8,16};case '-':return {0,0,0,31,0,0,0};
        case '%':return {25,25,2,4,8,19,19};case '?':return {14,17,1,2,4,0,4};
        case '>':return {16,8,4,2,4,8,16};case '<':return {1,2,4,8,4,2,1};
        // lower case (drawn only after the keepCase marker, e.g. A case-sensitive link)
        case 'a':return {0,0,14,1,15,17,15};case 'b':return {16,16,22,25,17,17,30};case 'c':return {0,0,14,16,16,17,14};
        case 'd':return {1,1,13,19,17,17,15};case 'e':return {0,0,14,17,31,16,14};case 'f':return {6,9,8,28,8,8,8};
        case 'g':return {0,15,17,17,15,1,14};case 'h':return {16,16,22,25,17,17,17};case 'i':return {4,0,12,4,4,4,14};
        case 'j':return {2,0,6,2,2,18,12};case 'k':return {16,16,18,20,24,20,18};case 'l':return {12,4,4,4,4,4,14};
        case 'm':return {0,0,26,21,21,17,17};case 'n':return {0,0,22,25,17,17,17};case 'o':return {0,0,14,17,17,17,14};
        case 'p':return {0,0,30,17,30,16,16};case 'q':return {0,0,13,19,15,1,1};case 'r':return {0,0,22,25,16,16,16};
        case 's':return {0,0,14,16,14,1,30};case 't':return {8,8,28,8,8,9,6};case 'u':return {0,0,17,17,17,19,13};
        case 'v':return {0,0,17,17,17,10,4};case 'w':return {0,0,17,17,21,21,10};case 'x':return {0,0,17,10,4,10,17};
        case 'y':return {0,0,17,17,15,1,14};case 'z':return {0,0,31,2,4,8,31};
        default:return {};
        }
    }
public:
    // heat01: 414 -> 430 px: the GRAPHICS page has 26 lines (+ HEAT WARNING row) at the 16 px pitch (12+25*16+14 = 426).
    // boost01: 430 -> 446 px: 27 lines (+ GPU BOOST row), 12+26*16+14 = 442.
    static constexpr unsigned width=640,height=446;
    // heat01: red = the rest of its line in red (the marker draws nothing, takes no column); a newline returns to the normal ink.
    static constexpr char red='\x01';
    static constexpr char keepCase='\x02'; // The rest of the line keeps its lower case
    static constexpr char gold='\x03',cyan='\x04'; // Header / sub-header colour for the rest of the line
    static constexpr std::uint32_t goldInk=0xff40c8ffu,cyanInk=0xffffc860u;
    static constexpr std::uint32_t ink=0xffe0f4ffu,redInk=0xff3038ffu; // RGBA8 sRGB, little endian (R in the low byte)
    std::array<std::uint32_t,width*height> pixels{};
    void draw(std::string_view text){
        pixels.fill(0xff14100cu);unsigned x=12,y=12;std::uint32_t colour=ink;bool keep=false;
        // 25 px per line fits 16 lines; longer texts (GRAPHICS menu rows) get a
        // tighter pitch (>= 16 px, glyphs are 14 px tall) instead of losing the last lines.
        unsigned lines=1;for(char c:text)lines+=c=='\n';
        const unsigned pitch=lines>1&&12+(lines-1)*25+14>height?std::max(16u,(height-26)/(lines-1)):25u;
        for(char c:text){if(c=='\n'){x=12;y+=pitch;colour=ink;keep=false;continue;}if(c==red){colour=redInk;continue;}if(c==keepCase){keep=true;continue;}if(c==gold){colour=goldInk;continue;}if(c==cyan){colour=cyanInk;continue;}
            if(!keep&&c>='a'&&c<='z')c=char(c-'a'+'A');
            auto rows=glyph(c);if(y+14>height)break;
            if(x+10<width)for(unsigned row=0;row<7;++row)for(unsigned col=0;col<5;++col)
                if(rows[row]&(1u<<(4-col)))for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx)
                    pixels[(y+row*2+dy)*width+x+col*2+dx]=colour;
            x+=12;
        }
    }
};
}
