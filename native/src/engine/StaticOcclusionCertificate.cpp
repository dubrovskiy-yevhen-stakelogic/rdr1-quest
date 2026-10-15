#include "StaticOcclusionCertificate.h"
#include "RelativePositionProof.h"
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <string_view>

namespace rdrvr::engine {
namespace {
using C=StaticOcclusionCertificate;
struct Errno {int saved{errno};~Errno(){errno=saved;}};
constexpr std::array<std::uint32_t,64> constants{
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
void compress(std::array<std::uint32_t,8>& state,const std::uint8_t* bytes){
    std::array<std::uint32_t,64> w{};
    for(unsigned i=0;i<16;++i)w[i]=(std::uint32_t(bytes[4*i])<<24)|(std::uint32_t(bytes[4*i+1])<<16)|(std::uint32_t(bytes[4*i+2])<<8)|bytes[4*i+3];
    for(unsigned i=16;i<64;++i){const auto x=w[i-15],y=w[i-2];w[i]=w[i-16]+(std::rotr(x,7)^std::rotr(x,18)^(x>>3))+w[i-7]+(std::rotr(y,17)^std::rotr(y,19)^(y>>10));}
    auto a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
    for(unsigned i=0;i<64;++i){const auto t1=h+(std::rotr(e,6)^std::rotr(e,11)^std::rotr(e,25))+((e&f)^(~e&g))+constants[i]+w[i];const auto t2=(std::rotr(a,2)^std::rotr(a,13)^std::rotr(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
}
bool digestEquals(const C::Digest& digest,const char* hex){
    constexpr char digits[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i)if(digits[digest[i]>>4]!=hex[2*i]||digits[digest[i]&15]!=hex[2*i+1])return false;return true;
}
struct Pin {const char* digest;unsigned bytes;C::PositionModel model;};
using P=C::PositionModel;
// Only fingerprints and audited mathematical contracts, never retail SPIR-V.
// Full-byte SHA256 also pins precision decorations, input location/type and CB
// layout. Relative: WorldView * xyz1(WorldSingle * xyz1(input0)-ViewInverse[3]).
// Direct: WorldViewProj * xyz1(input0). All matrices: set0 binding1, 272 bytes.
constexpr Pin pins[]{
    {"008b30f35beadf00fcb2104203b8fb12d97e148918002f061865b00ddc6e7364",1928,P::RelativeAffineXYZ1},
    {"01478a1be5aba49824aa17931671ad88286357bf4e7bf4c4e6bedba903d1dee4",3068,P::RelativeAffineXYZ1},
    {"01bb6d8abbe988efd86452dac1dcbdf4c5ce45b65af016e34e96dc68b6d604dc",3068,P::RelativeAffineXYZ1},
    {"01fb737f259a03b2faf202d31965d8ff963cb838bc4f4e58c8322eed9c65f39e",5492,P::RelativeAffineXYZ1},
    {"04ceac1cc655fc07967fe08399c65299486fbe8cbb28b16e48806a13cc084572",3624,P::RelativeAffineXYZ1},
    {"0524cc0b2544a37a8da905ba2832baeccaf0d4493129e6437770b4fe358b9535",1968,P::RelativeAffineXYZ1},
    {"05d86af7a91b24337a7789558af9f3089abb82db70b06d63d55bb57dba47be30",3600,P::RelativeAffineXYZ1},
    {"064659831969a3e8b25066753a24a4706991de6ed82d38b6b1b0bf10223af674",3924,P::RelativeAffineXYZ1},
    {"072611a5bf932aacff1de72931be7974202f67965f6fc5466859d58d2a14fbdc",2644,P::RelativeAffineXYZ1},
    {"07cb3ac97907ab9feca7cc354c963715de8224214fd23a246fa642714e110b21",4396,P::RelativeAffineXYZ1},
    {"0808a96612544eda20e51642c50daa60c0cdff43b90dac3c84f0307f391f7f0a",3436,P::RelativeAffineXYZ1},
    {"08e6864231210d0d035d98a8d3f0c9bebfb3c287794be5119c8e61cbca3dae37",2080,P::RelativeAffineXYZ1},
    {"08fcdc85f9f1506d2f0dc6d646554c8e5874e13d9162492c8d22fdddfa50c6a6",3968,P::RelativeAffineXYZ1},
    {"09b914d4d83f6394976adac8d4a1d465bf1f88e848491ab61402d4ed27c9ea84",5416,P::RelativeAffineXYZ1},
    {"0a6c2855c3a00bcea9a308f9e66af264b99e0b7cc96d8f33ac2b4d11a67bdd81",1708,P::RelativeAffineXYZ1},
    {"0af46180d11a02f97f31237c8673b9e80192707715d94d0e8f4b4ad6a9897ceb",2876,P::RelativeAffineXYZ1},
    {"0d6ea327614bfb085a198e3050f678b82e75443908419cd9ba1a2de99038cbda",1632,P::RelativeAffineXYZ1},
    {"0e63d37ba72a0347d54788c916f2a7d386ab5e82ade7ace07bd1a575d072c8b5",3688,P::RelativeAffineXYZ1},
    {"0fe07b94f981c55c4f64fbe70f34e1ba2fbb08842b496aa258b487464097e48e",1444,P::RelativeAffineXYZ1},
    {"104c43b00a2fce14d75e869676f271b2f918e377480528a70a75b594ad6a3ea9",2700,P::RelativeAffineXYZ1},
    {"1122b33980be386d3094bbef04bf535f95fb122620ca1578ec84859fbce70279",3568,P::RelativeAffineXYZ1},
    {"116bb00aa894d644492eb934ebc318a265aad50436115b981d36ef0593214272",1608,P::RelativeAffineXYZ1},
    {"11ed67b766bcaadd0a3803dad64dc25e8e4feb455a398a5cda4a6859b1450797",3348,P::RelativeAffineXYZ1},
    {"13c88e3f3330eac35e400a85ec2aaa86edb4c70cc0eebe0c19f9461adb869b61",3912,P::RelativeAffineXYZ1},
    {"13d8cb2bbc862742290e56e9a697eef855c67139bafb5e235f75f263c6747652",3688,P::RelativeAffineXYZ1},
    {"14ee5cbd6287694f69f3d7b7c5d823635095772084063d26b8945ca785080860",6348,P::RelativeAffineXYZ1},
    {"17a3fcd04d47eb2358917c5e95dc91916112fbedfed42135be567a8b3960352e",7788,P::RelativeAffineXYZ1},
    {"18dd6102f8876a3168fdacd9565e7ee648003cacd32863a7fd6e54045b8a7467",4536,P::RelativeAffineXYZ1},
    {"1993b01a0dce0869a9002ec516244705c23bd33f90adfb6484b22d28b716e6bd",10448,P::RelativeAffineXYZ1},
    {"1abbf10d0f455a3d75fb467293fad0f5a429f03ca11c17241a9c18df23ff4b50",3596,P::RelativeAffineXYZ1},
    {"1c714da3efc3454c52fd0fe011507612977816b9f38c23b2059051f331b7ea36",2092,P::RelativeAffineXYZ1},
    {"1ccc0dccaa21f326224c970df0b4ceb31d5006c0c4aad99ff4fc165af3a328e5",3984,P::RelativeAffineXYZ1},
    {"1d1e81fa2a3169b2350b1a37dd8b1f76d2db2aa6f3bf29b6f6141a7f1805cbd3",4504,P::RelativeAffineXYZ1},
    {"1da471df2ea353fcf8d498bb7a2e029154aa01f5e9fa71be3e92d33c1d78b942",4332,P::RelativeAffineXYZ1},
    {"1eb2c5e30552322f0f53f29efb9d5a57363b1dd27c4ceee8208a628d2e9e534e",3380,P::RelativeAffineXYZ1},
    {"1f3ced4d283b4d3a2f61412d8fb36e23a258a3a8cc17f6f3af72e172737dc381",1668,P::RelativeAffineXYZ1},
    {"1faff604db736bff5336c5ae1b7e15f1bf08aad5732a4936c66d4115e36b1f92",3680,P::RelativeAffineXYZ1},
    {"21155c36fcbe0d8fb111b6ae95e026edd9d8e73dfe97ceadf844390a50851137",3308,P::RelativeAffineXYZ1},
    {"2194b081e375bc24990b2a15fe66c83bd17efaff6ae49a537153aeb987c5da36",5132,P::RelativeAffineXYZ1},
    {"21d7e8cfb6ab3aa4b9a24debfd7fd20f3b84b41cae3a38f82b35def8472f25cc",2740,P::RelativeAffineXYZ1},
    {"21fdd9897da3b400fa656cf418c3235e63e2a240a4a3791a4c69b95e5a8a7160",4544,P::RelativeAffineXYZ1},
    {"233ba42894c5f162cd2b0de996c25b2e7ae774ab98ab553be5b1f37d757e2291",3652,P::RelativeAffineXYZ1},
    {"23be6c7bd192248de5ae79a955e5ed047130e624494aefb9adeb82f457846644",3372,P::RelativeAffineXYZ1},
    {"2405c6338e841caeea3d9cfc2cfd482795a027445a661b98f75a63275ced39c4",3640,P::RelativeAffineXYZ1},
    {"24677308c92214e626f3575f0f566c1a7ac8531cdcf6e867dabe29a6fe425d87",3960,P::RelativeAffineXYZ1},
    {"2502cb499ac271f979255101bfbf4ff36bbee256037381c3f6c468e4dde20d14",4396,P::RelativeAffineXYZ1},
    {"2a608e60031c8267dbf8fbc230895245546ab6a20070041f1bc60f8b44fff58a",5028,P::RelativeAffineXYZ1},
    {"2ba9560b1c3c290ea435baed82de8e848e4ff2b36a07068a8a0ceed78326bda9",3720,P::RelativeAffineXYZ1},
    {"2be99b05845ec8b00d60e359cc89549d2218b071a17bf9af04093b020dd9dde0",3348,P::RelativeAffineXYZ1},
    {"2cf3c6d240689d2a06b973fb11f1fb1a65206efa704505891526d6e98bb2d116",3444,P::RelativeAffineXYZ1},
    {"2cff52629cc80443aad80eb5399c21c05d548bd9ee61cf105d833c3b63ce667a",5364,P::RelativeAffineXYZ1},
    {"2d1e3c6c8edacfd05fd578822ea362bfdb7ad6d88037e631209b3a405a2a1c6e",5056,P::RelativeAffineXYZ1},
    {"2d3303e6a5ade1f2f60c7afa5cda8e38d6a3624f3c7b1e722363f9f2b24a137c",3436,P::RelativeAffineXYZ1},
    {"2deaf43089a9a73d14365eee9fa709ab688688c696bd3213b06c6ff51c208afd",2100,P::RelativeAffineXYZ1},
    {"31b93a8422bf2bfafb5bbb4f5dab15d8d96ed46fcccd1b6c5da7bf0bbb12320f",3436,P::RelativeAffineXYZ1},
    {"31e5317a269a02034b185387b123edcd64a6b6c032e9f5b791be67f23660e176",3324,P::RelativeAffineXYZ1},
    {"336b3446a17a993857824de8fb94c91f75c5cca58e6f1c6e432a962eb4d2a4c4",2596,P::RelativeAffineXYZ1},
    {"337902cdcef707a42d1292ad987cf74800f5124c343c9c3b5997286cc3a65f40",3428,P::RelativeAffineXYZ1},
    {"340ac8ea9723c9027348fdd8d67c72694bb092d423ff95f52f40e53b2d5fe27f",5336,P::RelativeAffineXYZ1},
    {"3438489383534a4615ee08a5447e3fcf929e243f9da9e8b0594863a6c3eb576a",5252,P::RelativeAffineXYZ1},
    {"360b1ae48d306a36c595109fb3a31764bce64caea698d25835f863a4aece8084",3960,P::RelativeAffineXYZ1},
    {"3624de3bdbd915c0d268ac1f39936568e61447eee23d1ae685596edb96500ec9",4524,P::RelativeAffineXYZ1},
    {"39f14dc07a6091c09f108ac1ab7a67e688dcd1326c4759e1cc8dd947866ca1fe",4824,P::RelativeAffineXYZ1},
    {"3acf9145fc7ce661efe9437d4d8e9a02eff163b041c791e1992922b89211f55c",3740,P::RelativeAffineXYZ1},
    {"3bdebbc08a11528541e23c424b65e4389205dff997f72351ee9efbe0e6a2f54f",3448,P::RelativeAffineXYZ1},
    {"3c1bc3b6677c2f8556b35805cd2ae697969ed9cd62954d45626ea28d1df45e4a",4792,P::RelativeAffineXYZ1},
    {"3cb2aaad3eba310a92186d60c1d6ad1f8741f3c8bd6c7bcec2fabe47a1fd657e",7072,P::RelativeAffineXYZ1},
    {"3e5f0e1de8fb2da8676b3db7d97e1a9ad42472e3ff74f828de6b401acf431e45",4048,P::RelativeAffineXYZ1},
    {"3f7fde0edf72c6b6512be3b71b46e663254211d245ee0bd4838a13bddd12721d",3820,P::RelativeAffineXYZ1},
    {"4202f93e4611dd6b060e3888e144b8db93099e020f7f5f2962eb5b8eaf37d578",3512,P::RelativeAffineXYZ1},
    {"43cbed8bedd3e6715f57702e5b3e00c6d1411b08020fb13edfccd9f955c09377",1872,P::RelativeAffineXYZ1},
    {"45eeca1deab2bc7fe9d29c0a3e7fb908be1508717565f6275e7837c5656e6ac5",1632,P::RelativeAffineXYZ1},
    {"463aa4f28ed3c9cab93d9fb2a1034291da3769382794475d765a5c83b2307953",4368,P::RelativeAffineXYZ1},
    {"4885a59f57f044a3d855a2f1b3b546e05de4df89ca91149f194c0ba4bd849d4a",4340,P::RelativeAffineXYZ1},
    {"53c92b18db4c10d2cae040461b6c88995472021699834b07fd2b6b2d4a39114c",5860,P::RelativeAffineXYZ1},
    {"55664950183555164387ddee480e0e7edaef41ef4afa2bde7b96cfa77057d772",3616,P::RelativeAffineXYZ1},
    {"5665467857232ca2ead2a7f850b7d630a57bd83e928b19e156268e3b8f26bac3",2536,P::RelativeAffineXYZ1},
    {"57f193f96f19ca122cde99c9c54529ad3d44823dfae279d4b83a2697f093e29e",3476,P::RelativeAffineXYZ1},
    {"5893791f24720d6ed1c2ff409740c71c35b247058b24bbba9ebbe2523d921535",1888,P::RelativeAffineXYZ1},
    {"597d7109ef5de513ee3aaf6c14f5e1d58564c4e2fb253e0f9eb978b62c8d6dac",2596,P::RelativeAffineXYZ1},
    {"59a1a85d050546934ca14d081a2e410b9a5295f69f5fd13306487f5fb378096a",1476,P::RelativeAffineXYZ1},
    {"59f21d242bf081a208094e7594a5611f21f1d62bb5626f2f29e702bcf4ef90fd",4744,P::RelativeAffineXYZ1},
    {"5ab096b50e1b1e6a698282c52a8987fd8dfa55fb262e0d07eab2f2801d56c705",4800,P::RelativeAffineXYZ1},
    {"5c29a13b3e13668892b698745ae46c99ded58e42fc25dc805b39307dd5610974",4216,P::RelativeAffineXYZ1},
    {"6145bed4df93acbf6c312456d18550aa186d50fbac5046ffe4b52339bd3bfcd3",5056,P::RelativeAffineXYZ1},
    {"61b63fce0a0a0f8acc91cb84e169dbfad0bd8a970c467ec0422346f13d438646",3408,P::RelativeAffineXYZ1},
    {"624e11bb797bbeb34afa33b539d1fa5017dd7e8c2342e5eb4625ffe7f0000ec3",8076,P::RelativeAffineXYZ1},
    {"626ce2d22ece1a5b68357197bc2278e95f3140b0b33f7eec3fd495518d531a10",3968,P::RelativeAffineXYZ1},
    {"630e0be8e64557d671ca78cc8d26e27a01387cac069bc6ca875c887296d62290",2548,P::RelativeAffineXYZ1},
    {"636d051f49cc657ef9f013b64843780d65933fd5ef5f524f20dc415211883126",3080,P::RelativeAffineXYZ1},
    {"63d28386e866e13f0b896b7b85bfa19db7be55ca44929aaa5a7b96b629a572a8",3976,P::RelativeAffineXYZ1},
    {"6434fb310bc6b463c00c7d49b156786717f59e79de1b596eaab46f91cbe2c80e",6204,P::RelativeAffineXYZ1},
    {"645d8e83405cea2d165564580136b29ac03fe78771aea353a7a490cd361c41e6",3448,P::RelativeAffineXYZ1},
    {"65bc7aecd90627f6dee1ae5c4925c34ec66a20bc0bdc44c6013889c13e310e28",3924,P::RelativeAffineXYZ1},
    {"66ffb38e350a76b3bf6e0438a306129811a801f8fb0ae7e5f265e392d227da0a",2116,P::RelativeAffineXYZ1},
    {"67676841b34572e662e8026fd0cbeb268ee31b34ac78a1c794402a79516c7d00",4880,P::RelativeAffineXYZ1},
    {"67ab1033bac5794cf36910111d32a81586801fb7d71b65f6c0024366298154bc",2536,P::RelativeAffineXYZ1},
    {"67cee5be85d93b48ab2a40fe2c2e08382962e9f042969be783281ca932d762c1",3968,P::RelativeAffineXYZ1},
    {"6b7cb38beb1ac7f25c5033b14e8b7fd22bfa306b6b82a3a966f724f51a541c2c",3080,P::RelativeAffineXYZ1},
    {"6e788825f19c0434240e0798e8a97cb7be9f9b87fc631d926d9c5e3b740bdc9d",4900,P::RelativeAffineXYZ1},
    {"6ff7be877b1839d0036536a94ba809e8a9a7141531f828811f5baa083c53b725",1936,P::RelativeAffineXYZ1},
    {"7352a326f3fd55c30e0922235586d827d06cc32298e67cf62d2ed18dee095f5f",3772,P::RelativeAffineXYZ1},
    {"73cdf00af0cca1a0e4824db97dd0bc31ea6f852c829afb879f5e0177fb51c3e3",3912,P::RelativeAffineXYZ1},
    {"74a9db0346c0b5801bc71b25118e3046461c77bd187e97cfedf087c418664015",1468,P::RelativeAffineXYZ1},
    {"756f2c9d778302eff5d827d622b50c8de82fe19632691b7247fdf33485658176",2608,P::RelativeAffineXYZ1},
    {"7663f39bb80d93bb59d85ef952a201443a74026054e022489b2812b31f61da74",3756,P::RelativeAffineXYZ1},
    {"7eed84b87d841498d7713901f41b7fef3727a3cc33706f76576e33762278e875",3980,P::RelativeAffineXYZ1},
    {"7f26341e9fdda2a75e46a34c619218b93d2ae8b3a554cc4b59a8b73b3bcb47cd",6504,P::RelativeAffineXYZ1},
    {"7f588649760f27eef51d42969f763bc0868074bec5220da1e7b60b751d1800a4",2896,P::RelativeAffineXYZ1},
    {"8025f0971f0b471296939f36166419dbaf84c8e7cc74ba413eea413fcc8559a2",3400,P::RelativeAffineXYZ1},
    {"824e9369291f5d7721b2849627790671b67dcb397dca41361fbc6cc9aee01b9f",1700,P::RelativeAffineXYZ1},
    {"82fc51fd5cca3cea45c90e44c7f1f59a7fb520e38cd77f3b4de99565cdad5d21",3560,P::RelativeAffineXYZ1},
    {"846beb97208e67ae6387c0569c5848c167cb301ea78f0613bb6b32495dc3c4c0",6580,P::RelativeAffineXYZ1},
    {"854241fd3fdc71efc3affb4e83f23016340c7c4eba51d39656f67a026887d9c1",3768,P::RelativeAffineXYZ1},
    {"871eb4a9b94f1aa4129bd815ca14582f5905f58066535594a96053b65661d905",3960,P::RelativeAffineXYZ1},
    {"87fd11565a17b4a74c4347466988008f9241fd340ae1f4b9650149170ab936c2",1444,P::RelativeAffineXYZ1},
    {"8da1a1d5a32ab43c9e504286d3c1ec0c12c866da25e4c6f172a72b2cef3f237f",3348,P::RelativeAffineXYZ1},
    {"8de45273996817b170d16346eac5a9ac6a30fd7be297c3e353133fc63e720a40",5296,P::RelativeAffineXYZ1},
    {"8f38f059b60a998832c2f41f596e2bd4fa8f6a4b7c21e70ca53aaf1873a5db85",2536,P::RelativeAffineXYZ1},
    {"8f9bcdd3e72c6124364f4bbdb17fcfb67205827f6c2bc5a25d566141aa41109f",4072,P::RelativeAffineXYZ1},
    {"8fdbd3c0fb372e7947f7f017db5f2d7b754443536f5159e96d0410273e44ebf3",3380,P::RelativeAffineXYZ1},
    {"908f942de339b9507aba129859243eb43b7adf69e255e4edb9abbecb2d840635",2552,P::RelativeAffineXYZ1},
    {"919a556762ac1011800007d9f7512468db7185b80af58e4ab939ae3e1a5e41f5",3408,P::RelativeAffineXYZ1},
    {"9207380f4be550189c3e3b3a0e1705fba163f371305159a960b762600620c904",3768,P::RelativeAffineXYZ1},
    {"92cffa1c55c680ca94e5e70d76ffd33952b99b96499c7a57775b74ddf7d10c76",4088,P::RelativeAffineXYZ1},
    {"94ea6efff27c22b123fc157ccd4b78b556df1885689a1b24a0994764ced58782",12148,P::RelativeAffineXYZ1},
    {"96b60024f02323626e3dfb6e80c76b4811e466d8640e74dad68eab9ab954e087",4368,P::RelativeAffineXYZ1},
    {"9b2a08f9fc0b2c7a8fb4ec8e8d55114a9fe064cf43aa6b82f52906c2d783f4fd",3304,P::RelativeAffineXYZ1},
    {"9c9c9e47414b84166e162fd28777221b378959544df097a0e837f9a89356fa10",7780,P::RelativeAffineXYZ1},
    {"9d24e3620a050f622cfb6e3d2ea417ad1f9cc903f02809bc0593eb60ff04090a",3424,P::RelativeAffineXYZ1},
    {"9d2ffac4853518c842ea52de6236498d3e50fb31da4915523c3d4f2cb1eaa778",3492,P::RelativeAffineXYZ1},
    {"9d3c209b19b0a61a85337ab3fc4cf9c24f7489b688bb039ad1791a2d41e56895",3256,P::RelativeAffineXYZ1},
    {"9f2e969d9ba0ecb4e244158ac640a729257a595c1f3b50dd8d309646665c6467",4060,P::RelativeAffineXYZ1},
    {"9f388683bd62ad7d709b9e5320a3484b72a2174a29767070cb46eab465ccc90a",4852,P::RelativeAffineXYZ1},
    {"9fa7c5718575619f15f5b8a0d8e329c1fdeecc930c7728fa9014c77b6c5a6f57",5284,P::RelativeAffineXYZ1},
    {"a117277bd508aa8c52221d57ce6a16a4161b6215b7e6749930bc1107b276ff95",2832,P::RelativeAffineXYZ1},
    {"a1466b90807392aa1da903fbdd9a01f1c30168cb9a20627ad319e865f202585e",4880,P::RelativeAffineXYZ1},
    {"a29c81a806d22840e006f746df04e748b0b61f3e6c3a3527abb068f3ea6c9417",4196,P::RelativeAffineXYZ1},
    {"a34004e9decb2b94f21aeea0d97e80165cbaa7cd9dcf5dc41fbc9848bcc54dc6",4740,P::RelativeAffineXYZ1},
    {"a3ff4704976254593e20f9b5a4900097e99832ea5e320a88436121a85248dc9b",1624,P::RelativeAffineXYZ1},
    {"a42a92eeb86bc22612ae8dc865046880debb8acca6d34b860c70cabc23162881",5764,P::RelativeAffineXYZ1},
    {"a4c33f16ca705553169e71734987ad9f5007cf7fe70da3e4f7eed44e4222fb2d",4060,P::RelativeAffineXYZ1},
    {"a50d27894005a2bef72f09312785a75285db7d25158c60d6b4b1cf7c31474fc5",1476,P::RelativeAffineXYZ1},
    {"a5b9b06b8997d39e732fedc8d2e0e0fa99cacb5e066c266c6fb6f73effc8db40",11520,P::RelativeAffineXYZ1},
    {"a61d92d771998df19442cbeabfcaa750118fce9f912e3bf5e3a094049705ad2d",3592,P::RelativeAffineXYZ1},
    {"a670f77ba999e3edb5f763f7b6799de9a6fd6cde7335cc8e90645d74ab792acc",1656,P::RelativeAffineXYZ1},
    {"a82c508ff2bc72e860fa43117567d4f528f2ad7faf6d65dd94d7c6a4410bc930",3436,P::RelativeAffineXYZ1},
    {"a9699bd1e51d019fe5a3768b8d5884b5ad457d4cf5b4ac849ffe700e42926d4c",3780,P::RelativeAffineXYZ1},
    {"a9cc3a51070fcee78d3c7811d319ede842d3272c858559d33010f0bb1ac510b5",5332,P::RelativeAffineXYZ1},
    {"af96536d2e38870de78acd945aaf2491e1ea75af4a2571b654d72e54fa382716",3804,P::RelativeAffineXYZ1},
    {"afaf608f7b8da40d1626b879dd7b3225e91afc230c08dc54d1ae9d4a1435f3a1",3632,P::RelativeAffineXYZ1},
    {"b186cd06fcc4464149fa9ef3b8c2a323136a1c4383670a0ee607929fede78974",1780,P::RelativeAffineXYZ1},
    {"b214e4122e1758a81e94640b8f38670dd8a3f2bd57bc0096e5a7590aea312e2d",2548,P::RelativeAffineXYZ1},
    {"b361aa287983728afd7a7a2ad737e7ac043f94b6d4f6a87d1663a7c9fa151569",2608,P::RelativeAffineXYZ1},
    {"b3b7cfb753d55dadb0a3d768e5fa68716190a4209811970c560fd986430fa250",3132,P::RelativeAffineXYZ1},
    {"b57003ff3dc4e6d76351ea58e53f409a3818afc85db1c56c5da975b173ce53eb",5596,P::RelativeAffineXYZ1},
    {"b67381295a6bef284de3b06d4db6824e3682754d85c491b4d48a13ea0a3a2190",2604,P::RelativeAffineXYZ1},
    {"bca83d569ecff89fc1c66456e2d7ea034cd70a95e8fda98fdb0a7d60edd091f5",4368,P::RelativeAffineXYZ1},
    {"bd3fa5216646810b3a640a1292bc9304696491bf86f179f9d3adb6172c03d272",3428,P::RelativeAffineXYZ1},
    {"bd46783b6061520faf91cdf2af41cadd461f78afd43b27933eb58844136b6005",3528,P::RelativeAffineXYZ1},
    {"bdf952452f79bfc0fd0eec056ef1bd74bc66bce9ad3f9ae9eeedccd4667a0159",2184,P::RelativeAffineXYZ1},
    {"bf2cfc6027cbf0c323be7b37e400642d45d7cd052e7bbaee799e67a3c8474b69",3576,P::RelativeAffineXYZ1},
    {"bf47e6bf7d253da5b6bacac3f78bb51f0a2fd64ea29d8769fa5e54a1f9f2da11",4348,P::RelativeAffineXYZ1},
    {"bfadc681b54c2f08d2eb81f1d85145509ccafaea37028d886e1cdd47cf740485",1436,P::RelativeAffineXYZ1},
    {"bfe5ed4ec22716c0ce311ec96671d381297fdf2d8b0efc60051f58c95129b3de",3308,P::RelativeAffineXYZ1},
    {"c05668c90dc5dd4488f9805f09a95b7f69d007cfbedbcaaa42854bcffc151d19",7620,P::RelativeAffineXYZ1},
    {"c114585d9d18986bc40bea6fd86960d0c05679c1dd3119d393d943658f8a49b9",3976,P::RelativeAffineXYZ1},
    {"c129bc21bc5b07b211969a120112ecb6722fbac63653a05a5e22b4a07947d962",3396,P::RelativeAffineXYZ1},
    {"c91fa94f77326b9976886064eaf5106bbd8496e55512df6c0006b80a54bfe3af",2952,P::RelativeAffineXYZ1},
    {"cad6a0deb4d883d9c3e960dc73b2fbe4eab7e423d29d78de130a5275789fa7d1",2956,P::RelativeAffineXYZ1},
    {"ccb21347f6affa256a24bd5f6dd8eb87a2ac5448994903cad94f20388012e3cf",6888,P::RelativeAffineXYZ1},
    {"cce1f5f4f880f2d687e4af812f67e0e8ad5c18c5de13156808849e62b5ac91b4",3444,P::RelativeAffineXYZ1},
    {"cf1d11c2619840f7de81b05fd87b2cfb38bfde1d4a68223faa65fd6c1c61258f",3456,P::RelativeAffineXYZ1},
    {"cfa1dfccbee60d72226d69a5a9fa24c08b9f27eea9bc0e86f2c675089cb0db66",3424,P::RelativeAffineXYZ1},
    {"d031d30e6b9eb9bfe90381898d6c68c1ca4bc96bc449c263e2511b191248c887",2856,P::RelativeAffineXYZ1},
    {"d046c58fd87a0ea62e2b134d0ee7a5671c319b55ba3a2ed6bf2b1a39127a7848",4172,P::RelativeAffineXYZ1},
    {"d0da5217e6a5271c9e5ed874c201f6e8f009650c44577637cdf76ac4029d83c5",6164,P::RelativeAffineXYZ1},
    {"d18d6dcaec827e7bd9b6a08859ea14c16088f55a528148225fad2f082436478b",3968,P::RelativeAffineXYZ1},
    {"d256ca85118b682f0ebe180eb055037e7699b58c556268f522b1b88b0eb10880",3380,P::RelativeAffineXYZ1},
    {"d259603f33726493d8b6a13362f57d5c18a5bf33ceb9dfa74b211b3d0aa64cac",1888,P::RelativeAffineXYZ1},
    {"d4e614395ed013123fd75b7bbc6d1575fa94d664ab3acfab89e0f000182a855b",1976,P::RelativeAffineXYZ1},
    {"d60d6e0370159d3ebabef0211e7b50b2290c24ee5b1c009693e96ee204a94a63",4272,P::RelativeAffineXYZ1},
    {"d71e35a04deeedc35f9536b7d4291b91ef914332ed83b6545e5efb41cf24d1b7",3340,P::RelativeAffineXYZ1},
    {"d77a1a25a2ce7b38d4cf6dd2c0424d39fb353778d0aa49988b53a9f2827520ec",3912,P::RelativeAffineXYZ1},
    {"d78ba1d6ce783941e4a33ead5e841310ce23221787a468eb199f2d3b08bb33ec",4852,P::RelativeAffineXYZ1},
    {"d87da58967a35c3a010c11aab33cc098d38a1baad2111280c8e187cf15870e38",3444,P::RelativeAffineXYZ1},
    {"d987eaa83ea986c57ae93135c485e9a1b116547a2d01452f006c32284afe5309",2608,P::RelativeAffineXYZ1},
    {"da124dc05e6e5b45b73299297c49477ad3bc07da486e6e9d7e04b7cdc872f9f0",7420,P::RelativeAffineXYZ1},
    {"db0c7b06919b63e4c887d37fb570c82dc32a4e037369d4a6eaf05352f475e198",3340,P::RelativeAffineXYZ1},
    {"db17cdb74007810875225b8415cd3fdd8d40ca5f74a72ac25b9753d82d37cdd1",2156,P::RelativeAffineXYZ1},
    {"db68ad58d79f2fc0b11844d2ac85479ef42a21a84f03d6bc84df244b631ff99b",4308,P::RelativeAffineXYZ1},
    {"ddae3feb9496cbb9aa9bddab4bd5a559838c33413d46b7ed8e03c2d694a87348",1868,P::RelativeAffineXYZ1},
    {"de01aa084d6705b3578c52c1610ad475fe59235c0701cea7ccb96218b9054c2f",3340,P::RelativeAffineXYZ1},
    {"de3e719bcd84f386c7ced633573d5372f36aca16533b3fcd2d3954f9aeaa8ac0",3460,P::RelativeAffineXYZ1},
    {"def99c0cec2ab72350c50242c76d6dd1ab51193bec4c6af4fe51b9bd17a2489d",3408,P::RelativeAffineXYZ1},
    {"e0935680f0f37c53ba01d7488cc4ed11b1f9b6cf08e34a416a0b7067e49df64d",5860,P::RelativeAffineXYZ1},
    {"e2ea308b1038302106bf59e023abb01a94e6b3e67f472d4f4e5f0a3d96dfbbf2",2548,P::RelativeAffineXYZ1},
    {"e357bf389c1c3e2bb8d61725a9c734e40e8aedf4755efed53096b6567c84d751",4604,P::RelativeAffineXYZ1},
    {"e4cddb96415d0dd204bd9e79b2b01e5cdc44bc602c98a3f7f8ad622a2e62a30e",4612,P::RelativeAffineXYZ1},
    {"e6579a31aaad796e0b441e7c59c43a54920dd792fd2ee871e1cb443344ba3524",3512,P::RelativeAffineXYZ1},
    {"e7cd8327bc9398d4ac2227d4b28f9ba3a9ae2a05bd61ba055de6d17fb03bab64",4048,P::RelativeAffineXYZ1},
    {"e8b1fbc82aef6a6181ae3da683dd0c1830f6f0f7331f4da410bcaa7c8335d401",3528,P::RelativeAffineXYZ1},
    {"eac4de8238bb82db9201cab3a3d7fe5c533850b76a4a8f817bfcbbf1ab580338",4416,P::RelativeAffineXYZ1},
    {"ebde99bde8ff7eb8e3a84d8ddf3a7c2d3cc3a8a8b7455682742d5dd25ef14558",4828,P::RelativeAffineXYZ1},
    {"ec782758c6bd04cc8b4a189bcb4fad30a1063c9041d7048a6d0d34ff62785bdf",1864,P::RelativeAffineXYZ1},
    {"efb2dc8f12b7728f5b6c6b3621ffe0f0d54c551ee2e06880ee88fa59698cee1c",4172,P::RelativeAffineXYZ1},
    {"f05f20835a56ca1be2659028236fe1862a4afcf47e22675df135982acee70d80",3976,P::RelativeAffineXYZ1},
    {"f0c5dd6f8009066771dc8ba830af13d36ec39be7ba2af63fb0a15a3ecd936a12",2800,P::RelativeAffineXYZ1},
    {"f0c777832c84839dae8c6b9d12404460571aa0e53d631087b746423c5c0cc0c2",4512,P::RelativeAffineXYZ1},
    {"f10f8e75020f04761ef68f86c15a57ce528dfe5d388dd65e3ffa11c6ded3313f",4072,P::RelativeAffineXYZ1},
    {"f2a9021222be144aabb15be33260e3a2f0e633b65c78be7dd70a788b3294bd57",3664,P::RelativeAffineXYZ1},
    {"f3d47a4faaaa373dcd949d94ead790e95efd57e93a41bbaa7962677a7af68165",3976,P::RelativeAffineXYZ1},
    {"f6a5de4989227d1c58d2612a55fe440c2a915dca39706a808d2e5179f579efb3",3552,P::RelativeAffineXYZ1},
    {"fbd2086e7bf8a300481f820966ff9f21cd98b30caf013fe4ad5ad540a6e92218",4164,P::RelativeAffineXYZ1},
    {"fc11dd2c0c218e34e19a7b948e976c56edfe822a12f6f5b8dab380dec4c36615",4180,P::RelativeAffineXYZ1},
    {"fc3b7485e6caddb583dedff74d5390c636e99cbae9c634bf90043db07d606df5",5020,P::RelativeAffineXYZ1},
    {"ff90c06a03e98b0a2e8030352c4c7791dcc0f4184dd3079a7f9e5b9c80e26223",2644,P::RelativeAffineXYZ1},
    {"ffa98955a572df48cdcdf3d5e853eff1a425086e55c04c0ec5f6b1d853da72b1",2092,P::RelativeAffineXYZ1},
    {"02177733a6bf65ac89f42938f4dbf7cefa432c872082b1db76f9e13f2e62bbe0",4576,P::DirectMvpXYZ1},
    {"0e1ed66f8865135d40bd4484c87c9294f9cf9986d385bc38fc062611a0358b9b",1520,P::DirectMvpXYZ1},
    {"1b94d473d98a3aa020d286762ca2f0357ef2b33c4c17fcbb1d4ea4f845149eb0",1784,P::DirectMvpXYZ1},
    {"2f986a3ff1f9f91ec9e05a1b5b52b8960422ff3edb7e11824b0980b93f5c8813",4664,P::DirectMvpXYZ1},
    {"6dfd9477b32fd500eb4b36a6cc3ab3e80d53f4e92d5bd56c04422ea81d469c0b",2264,P::DirectMvpXYZ1},
    {"7240ab48fcdab2dc7ebd74fe1f4ca72911daabeb2fa6b562c61a019e0e84d10c",1496,P::DirectMvpXYZ1},
    {"7727debc4af77458ccb0ba0ca23bc72a6de65bd6779fd384048223028226756b",1840,P::DirectMvpXYZ1},
    {"84d9a1fb0346991e64efe4fef48bbc195cfdb4992c18e38ae287ccb93c8f45ff",1652,P::DirectMvpXYZ1},
    {"9a00217a9180a8425122f9c274bf5bbbe30a3318cf5d44985e91293ee9333ac9",1856,P::DirectMvpXYZ1},
    {"a168eb90957a6bf38a84122c25a6dbcccb780de762dbef98c7c94d2bb2ea0edf",1764,P::DirectMvpXYZ1},
    {"aebef2e2f853cc4b1cc30da2f98bc834f1568dc115a5d47390fce7670373756e",1636,P::DirectMvpXYZ1},
    {"aede7d75bfaa141f655d658fa0661fab999b56cbbd39b4142556e659ad7e9b5b",1684,P::DirectMvpXYZ1},
    {"b7f0c7b4e3735f65b43cc4fe6e1a686bd56c1089c6039a8e9df99d2a808d4725",1324,P::DirectMvpXYZ1},
    {"bc460fbeeef7320414dbba7bdf2fbc093b4419d7812a709b68d214cbd36a6b8d",1176,P::DirectMvpXYZ1},
    {"c23d1b68ac0bb8dfdd1d3a6b682fc4b3485b6d5ee3b2e1cb50ec0154f99ed249",1712,P::DirectMvpXYZ1},
    {"c2dde0954f1dbaa5ad6cf9d3d69d90c3b4ce6b3cd09c18084ab1786453f96831",1572,P::DirectMvpXYZ1},
    {"d1fa1df4f622f8a7ec41dab2de31472b2521f06eba31db9abbb7fb146b5dde93",1588,P::DirectMvpXYZ1},
    {"e6a187b8c11c6e87fa65a72561c5aeae6e804f16fc191031495b526ce1e36819",1612,P::DirectMvpXYZ1}};
bool text(std::span<const std::uint32_t> words,std::string_view expected){
    const auto* p=reinterpret_cast<const char*>(words.data());const auto bytes=words.size_bytes();
    return expected.size()<bytes&&!std::memcmp(p,expected.data(),expected.size())&&!p[expected.size()];
}
bool finiteViewport(const VkViewport& v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.width)&&std::isfinite(v.height)&&v.width>0&&v.height!=0&&v.minDepth==0&&v.maxDepth==1;}
}
C::Digest C::fingerprint(std::span<const std::uint8_t> bytes) noexcept {
    std::array<std::uint32_t,8> state{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const auto size=bytes.size();std::size_t offset{};while(size-offset>=64){compress(state,bytes.data()+offset);offset+=64;}
    std::array<std::uint8_t,128> tail{};const auto remain=size-offset;if(remain)std::memcpy(tail.data(),bytes.data()+offset,remain);tail[remain]=0x80;
    const auto length=remain<56?64u:128u;const std::uint64_t bits=std::uint64_t(size)*8;
    for(unsigned i=0;i<8;++i)tail[length-1-i]=std::uint8_t(bits>>(8*i));compress(state,tail.data());if(length==128)compress(state,tail.data()+64);
    Digest out{};for(unsigned i=0;i<8;++i)for(unsigned j=0;j<4;++j)out[4*i+j]=std::uint8_t(state[i]>>(24-j*8));return out;
}
C::Shader C::shader(const VkShaderModuleCreateInfo* info) noexcept {
    Errno preserve;Shader out{};
    if(!info||info->sType!=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO||info->pNext||info->flags||!info->pCode||info->codeSize<20||info->codeSize>512*1024||info->codeSize%4)return out;
    const std::span<const std::uint32_t> words(info->pCode,info->codeSize/4);
    if(words[0]!=0x07230203||words[1]!=0x00010300||!words[3]||words[3]>1000000||words[4])return out;
    out.digest=fingerprint({reinterpret_cast<const std::uint8_t*>(info->pCode),info->codeSize});
    unsigned entries{},functions{},labels{},returns{};bool effects=false,coverage=true,depth=false,precision=false,vertexClip=false;
    for(std::size_t i=5;i<words.size();){const auto n=words[i]>>16,op=words[i]&65535;if(!n||n>words.size()-i)return {};
        const auto a=words.subspan(i+1,n-1);
        if(op==15){if(a.size()<3||++entries!=1)return {};out.stage=a[0]==0?1:a[0]==4?2:0;const auto* p=reinterpret_cast<const char*>(a.data()+2);const auto size=(a.size()-2)*4;
            unsigned length{};while(length<size&&length<out.entry.size()-1&&p[length]){if(p[length]<32||p[length]>126)return {};out.entry[length]=p[length];++length;}if(length==size||p[length])return {};}
        if(op==10&&!text(a,"SPV_EXT_descriptor_indexing"))effects=true;
        if(op==11&&(a.size()<2||!text(a.subspan(1),"GLSL.std.450")))effects=true;
        if(op==16){if(a.size()<2)return {};if(a[1]!=7&&a[1]!=9)effects=true;}
        if(op==71||op==72){const unsigned d=op==71?1:2;if(a.size()<=d)return {};const auto decoration=a[d];
            precision|=decoration==0;if(decoration==36||decoration==37||decoration==29)effects=true;
            if(decoration==11){if(a.size()<=d+1)return {};const auto builtin=a[d+1];depth|=builtin==22||builtin==20||builtin==5014;
                vertexClip|=builtin==3||builtin==4;effects|=builtin==9||builtin==10;}}
        if(op==59){if(a.size()<3)return {};const auto storage=a[2];if(storage!=0&&storage!=1&&storage!=2&&storage!=3&&storage!=6&&storage!=7)effects=true;}
        if(op==32){if(a.size()<3)return {};const auto storage=a[1];if(storage!=0&&storage!=1&&storage!=2&&storage!=3&&storage!=6&&storage!=7)effects=true;}
        if(op==25){if(a.size()<8)return {};if(a[6]==2)effects=true;}
        if(op==54)++functions;if(op==248)++labels;if(op==253)++returns;
        if(op==57||op==99||op==218||op==219||(op>=220&&op<=242)||op==318||op==319||op==4434||op==4435)effects=true;
        if(op==245||op==246||op==247||op==249||op==250||op==251||op==252||op==254||op==255||op==4416||op==5380)coverage=false;
        if(op>320&&op!=4416&&op!=5380)effects=true;
        i+=n;
    }
    if(entries!=1||!out.stage||functions!=1)return out;
    out.sideEffectFree=!effects;out.fragmentSafe=out.stage==2&&!effects&&!depth;
    out.fragmentOpaque=out.fragmentSafe&&coverage&&labels==1&&returns==1;
    out.vertexCoverage=out.stage==1&&!effects&&!vertexClip;
    if(out.stage==1&&out.sideEffectFree&&!precision)for(const auto& pin:pins)if(pin.bytes==info->codeSize&&digestEquals(out.digest,pin.digest)){out.position=pin.model;break;}
    if(out.stage==1&&out.sideEffectFree&&out.vertexCoverage&&out.position==PositionModel::Unknown){
        try { if(detail::RelativePositionProof(words).run()){out.position=PositionModel::RelativeAffineXYZ1;out.structuralPosition=true;} } catch(...) { /* cold allocation failure stays unknown */ }
    }
    return out;
}
C::Pipeline C::pipeline(const VkGraphicsPipelineCreateInfo& ci,const Shader& vertex,const Shader& fragment) noexcept {
    Errno preserve;Pipeline out{};
    auto refuse=[&](PipelineReason reason){out.reason=reason;return out;};
    if(ci.sType!=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO||(ci.flags&~(VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT|VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT|VK_PIPELINE_CREATE_DERIVATIVE_BIT))||ci.stageCount!=2||!ci.pStages||ci.pTessellationState||!ci.layout||!ci.renderPass||ci.subpass)return out;
    out.reason=PipelineReason::ShaderStages;
    unsigned stageMask{};for(unsigned i=0;i<2;++i){const auto& s=ci.pStages[i];const unsigned bit=s.stage==VK_SHADER_STAGE_VERTEX_BIT?1:s.stage==VK_SHADER_STAGE_FRAGMENT_BIT?2:0;
        if(!bit||(stageMask&bit)||s.sType!=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO||s.pNext||s.flags||s.pSpecializationInfo||!s.pName||!s.module)return out;
        const auto& proof=bit==1?vertex:fragment;if(proof.stage!=bit||std::strcmp(s.pName,proof.entry.data()))return out;stageMask|=bit;}
    if(stageMask!=3)return out;
    if(vertex.position==PositionModel::Unknown)return refuse(PipelineReason::UnknownPosition);
    if(!vertex.sideEffectFree)return refuse(PipelineReason::VertexEffects);
    out.reason=PipelineReason::Extension;
    if(ci.pNext){const auto* rate=static_cast<const VkPipelineFragmentShadingRateStateCreateInfoKHR*>(ci.pNext);
        if(rate->sType!=VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR||rate->pNext||rate->fragmentSize.width!=1||rate->fragmentSize.height!=1||
           rate->combinerOps[0]!=VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR||rate->combinerOps[1]!=VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR)return out;}
    out.reason=PipelineReason::VertexInput;
    const auto* input=ci.pVertexInputState;const auto* assembly=ci.pInputAssemblyState;
    if(!input||input->sType!=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO||input->pNext||input->flags||!input->vertexAttributeDescriptionCount||input->vertexAttributeDescriptionCount>32||!input->pVertexAttributeDescriptions||
       !input->vertexBindingDescriptionCount||input->vertexBindingDescriptionCount>32||!input->pVertexBindingDescriptions||!assembly||assembly->sType!=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO||assembly->pNext||assembly->flags||assembly->topology!=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST||assembly->primitiveRestartEnable)return out;
    out.reason=PipelineReason::PositionAttribute;
    const VkVertexInputAttributeDescription* position{};for(unsigned i=0;i<input->vertexAttributeDescriptionCount;++i){const auto& a=input->pVertexAttributeDescriptions[i];if(a.location==vertex.positionLocation){if(position)return out;position=&a;}}
    if(!position||position->binding!=0||(position->format!=VK_FORMAT_R32G32B32_SFLOAT&&position->format!=VK_FORMAT_R32G32B32A32_SFLOAT))return out;
    out.reason=PipelineReason::VertexBinding;
    const VkVertexInputBindingDescription* binding{};for(unsigned i=0;i<input->vertexBindingDescriptionCount;++i){const auto& b=input->pVertexBindingDescriptions[i];if(b.binding==position->binding){if(binding)return out;binding=&b;}}
    const unsigned bytes=position->format==VK_FORMAT_R32G32B32_SFLOAT?12:16;
    if(!binding||binding->inputRate!=VK_VERTEX_INPUT_RATE_VERTEX||!binding->stride||binding->stride>4096||position->offset>binding->stride||binding->stride-position->offset<bytes||position->offset%4||binding->stride%4)return out;
    out.position=vertex.position;out.positionKnown=true;out.binding=position->binding;out.offset=position->offset;out.stride=binding->stride;out.format=position->format;
    out.reason=PipelineReason::FixedState;
    const auto* raster=ci.pRasterizationState;const auto* depth=ci.pDepthStencilState;const auto* ms=ci.pMultisampleState;const auto* blend=ci.pColorBlendState;const auto* view=ci.pViewportState;
    if(!raster||raster->sType!=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO||raster->flags||raster->depthClampEnable||raster->rasterizerDiscardEnable||raster->depthBiasEnable||raster->polygonMode!=VK_POLYGON_MODE_FILL||raster->lineWidth!=1||
       (raster->cullMode&~VK_CULL_MODE_FRONT_AND_BACK)||raster->cullMode==VK_CULL_MODE_FRONT_AND_BACK||(raster->frontFace!=VK_FRONT_FACE_CLOCKWISE&&raster->frontFace!=VK_FRONT_FACE_COUNTER_CLOCKWISE)||
       !depth||depth->sType!=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO||depth->pNext||depth->flags||!depth->depthTestEnable||depth->stencilTestEnable||depth->depthBoundsTestEnable||
       (depth->depthCompareOp!=VK_COMPARE_OP_LESS&&depth->depthCompareOp!=VK_COMPARE_OP_LESS_OR_EQUAL&&depth->depthCompareOp!=VK_COMPARE_OP_EQUAL)||
       !ms||ms->sType!=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO||ms->pNext||ms->flags||ms->rasterizationSamples!=VK_SAMPLE_COUNT_1_BIT||ms->alphaToCoverageEnable||ms->alphaToOneEnable||ms->sampleShadingEnable||(ms->pSampleMask&&!(ms->pSampleMask[0]&1))||
       !blend||blend->sType!=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO||blend->pNext||blend->flags||blend->logicOpEnable||blend->attachmentCount>8||(blend->attachmentCount&&!blend->pAttachments)||
       !view||view->sType!=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO||view->pNext||view->flags||view->viewportCount!=1||view->scissorCount!=1)return out;
    out.cull=raster->cullMode;out.frontFace=raster->frontFace;
    out.reason=PipelineReason::DepthClip;
    if(raster->pNext){const auto* clip=static_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT*>(raster->pNext);
        if(clip->sType!=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT||clip->pNext||clip->flags||clip->depthClipEnable!=VK_TRUE)return out;}
    out.reason=PipelineReason::DynamicState;
    if(ci.pDynamicState){const auto& d=*ci.pDynamicState;if(d.sType!=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO||d.pNext||d.flags||d.dynamicStateCount>16||(d.dynamicStateCount&&!d.pDynamicStates))return out;
        unsigned seen{};for(unsigned i=0;i<d.dynamicStateCount;++i){const auto state=d.pDynamicStates[i];if(unsigned(state)>unsigned(VK_DYNAMIC_STATE_STENCIL_REFERENCE))return out;
            const auto bit=1u<<unsigned(state);if(seen&bit)return out;seen|=bit;
            if(state==VK_DYNAMIC_STATE_VIEWPORT)out.requiredDynamic|=Viewport;
            else if(state==VK_DYNAMIC_STATE_SCISSOR)out.requiredDynamic|=Scissor;
            // Remaining core states affect features already statically disabled:
            // fill triangles, no depth bias/bounds/stencil, no source blending.
        }}
    out.reason=PipelineReason::Viewport;
    if(!(out.requiredDynamic&Viewport)){if(!view->pViewports||!finiteViewport(view->pViewports[0]))return out;out.viewport=view->pViewports[0];}
    out.reason=PipelineReason::Scissor;
    if(!(out.requiredDynamic&Scissor)){if(!view->pScissors||!view->pScissors[0].extent.width||!view->pScissors[0].extent.height)return out;out.scissor=view->pScissors[0];}
    if(!fragment.fragmentSafe)return refuse(PipelineReason::FragmentEffects);out.candidateSafe=true;out.reason=PipelineReason::None;
    bool blended=false;for(unsigned i=0;i<blend->attachmentCount;++i)blended|=blend->pAttachments[i].blendEnable!=0;
    out.opaqueDepth=depth->depthWriteEnable&&depth->depthCompareOp!=VK_COMPARE_OP_EQUAL&&!blended&&fragment.fragmentOpaque&&vertex.vertexCoverage;
    return out;
}
}
