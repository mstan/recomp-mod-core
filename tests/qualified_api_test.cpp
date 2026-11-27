#include "recomp_mod.h"
#include "recomp_mod/archive.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "qualified mod line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
namespace fs = std::filesystem;
static std::vector<uint8_t> bytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary); CHECK(f);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static std::string hex(const std::array<uint8_t,32>& value) {
    const char* digits="0123456789abcdef"; std::string result;
    for (auto c:value) { result+=digits[c>>4]; result+=digits[c&15]; } return result;
}
struct Bus { std::array<uint8_t,4> ram{{1,2,3,4}}, cart{{5,6,7,8}}; int writes=0, commits=0, rollbacks=0, fail=0; };
static int begin(void* u) { static_cast<Bus*>(u)->writes=0; return 1; }
static uint8_t* memory(Bus* b,const char* s) { return !std::strcmp(s,"ram")?b->ram.data():!std::strcmp(s,"cartridge")?b->cart.data():nullptr; }
static int read(void* u,const char* s,uint64_t a,uint8_t* out,size_t n) {
    auto* p=memory(static_cast<Bus*>(u),s); if(!p||a>4||n>4-a)return 0; std::memcpy(out,p+a,n);return 1;
}
static int write(void* u,const char* s,uint64_t a,const uint8_t* data,size_t n) {
    auto* b=static_cast<Bus*>(u);auto* p=memory(b,s);if(!p||a>4||n>4-a)return 0;
    std::memcpy(p+a,data,n);return ++b->writes!=b->fail;
}
static void finish(void* u,int commit) { auto* b=static_cast<Bus*>(u);if(commit)++b->commits;else ++b->rollbacks; }
static void imported_plan() {
    std::array<uint8_t,32> digest{},accepted{},after{};
    CHECK(rmod_sha256_bytes(nullptr,0,digest.data()));
    CHECK(hex(digest)=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(rmod_sha256_bytes("abc",3,digest.data()));
    CHECK(hex(digest)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(!rmod_sha256_bytes(nullptr,1,digest.data())&&!rmod_sha256_bytes("",0,nullptr));
    auto* c=rmod_create();CHECK(c);
    std::string image(64,'1'),plan(64,'2');
    CHECK(rmod_bind_plan(c,"fixture",image.c_str(),plan.c_str()));rmod_identity(c,accepted.data());
    CHECK(hex(accepted)==plan);
    CHECK(!rmod_bind_plan(c,"",image.c_str(),plan.c_str()));
    CHECK(!rmod_bind_plan(c,"fixture",image.c_str(),"short"));
    CHECK(!rmod_bind_plan(c,"fixture",image.c_str(),std::string(64,'A').c_str()));
    rmod_identity(c,after.data());CHECK(accepted==after);
    Bus b; const auto ram=b.ram,cart=b.cart;
    const uint8_t r0[]={1},c0[]={5},replacement[]={9};
    const rmod_patch patches[]={{"ram",0,r0,replacement,1},{"cartridge",0,c0,replacement,1}};
    rmod_engine engine{&b,"fixture",image.c_str(),begin,read,write,nullptr,finish};
    b.fail=2;CHECK(!rmod_apply(c,patches,2,nullptr,0,&engine,digest.data()));
    CHECK(b.ram==ram&&b.cart==cart&&b.rollbacks==1);
    b.fail=0;CHECK(rmod_apply(c,patches,2,nullptr,0,&engine,digest.data())==1);
    CHECK(b.ram[0]==9&&b.cart[0]==9&&b.commits==1);
    const auto first=digest;b.ram=ram;b.cart=cart;
    CHECK(rmod_bind_plan(c,"fixture",image.c_str(),std::string(64,'3').c_str()));
    CHECK(rmod_apply(c,patches,2,nullptr,0,&engine,digest.data())==1&&first!=digest);
    rmod_destroy(c);
}
static void archives(const fs::path& root) {
    auto* context=rmod_create();CHECK(context);
    CHECK(rmod_initialize(context,(root/"rejected-catalog").string().c_str(),"fixture",std::string(64,'1').c_str()));
    for(const char* type:{"stored","deflated"}) {
        auto data=bytes(root/(std::string(type)+".rmod"));
        std::vector<recomp::mods::ZipEntry> entries;std::string error;
        CHECK(recomp::mods::parse_zip(data,entries,&error)&&entries.size()==2);
        const auto target=root/(std::string("extract-")+type);
        CHECK(recomp::mods::extract_zip(data,entries,target,&error));
        CHECK(bytes(target/fs::u8path("assets/\xE9\x9B\xAA.txt"))==bytes(root/"payload.bin"));
    }
    for(const char* name:{"symlink","reparse","traversal"}) {
        auto data=bytes(root/(std::string(name)+".rmod"));
        std::vector<recomp::mods::ZipEntry> entries;std::string error;
        CHECK(!recomp::mods::parse_zip(data,entries,&error)&&!error.empty());
        CHECK(!rmod_install(context,(root/(std::string(name)+".rmod")).string().c_str()));
        CHECK(std::strstr(rmod_error(context),"unsafe ZIP path"));
    }
    rmod_destroy(context);
    auto corrupt=bytes(root/"corrupt.rmod");std::vector<recomp::mods::ZipEntry> entries;std::string error;
    CHECK(recomp::mods::parse_zip(corrupt,entries,&error));
    CHECK(!recomp::mods::extract_zip(corrupt,entries,root/"extract-corrupt",&error));
}
static void identities(const fs::path& root) {
    std::array<uint8_t,32> image{},a{},b{};const auto original=bytes(root/"original.bin");
    CHECK(rmod_sha256_bytes(original.data(),original.size(),image.data()));const auto digest=hex(image);
    for(const char* name:{"catalog-a","catalog-b"}) {
        auto* c=rmod_create();CHECK(c);const auto catalog=(root/name).string();
        CHECK(rmod_initialize(c,catalog.c_str(),"fixture",digest.c_str()));
        CHECK(rmod_install(c,(root/"deflated.rmod").string().c_str()));
        CHECK(rmod_commit(c,(root/"original.bin").string().c_str()));
        rmod_identity(c,name[8]=='a'?a.data():b.data());rmod_destroy(c);
    }
    CHECK(a==b);
    const auto base=root/"catalog-b/packages/shared/1/assets";
    fs::rename(base/fs::u8path("\xE9\x9B\xAA.txt"),base/fs::u8path("\xE9\x9B\xB2.txt"));
    auto* c=rmod_create();CHECK(c);
    CHECK(rmod_initialize(c,(root/"catalog-b").string().c_str(),"fixture",digest.c_str()));
    CHECK(rmod_commit(c,(root/"original.bin").string().c_str()));rmod_identity(c,b.data());CHECK(a!=b);
    rmod_destroy(c);
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;const fs::path root=fs::u8path(argv[1]);
    imported_plan();archives(root);identities(root);
#ifdef __cpp_char8_t
    std::puts("UTF-8 identity checked with C++20 char8_t support");
#else
    std::puts("UTF-8 identity checked with C++17 filesystem strings");
#endif
    std::puts("qualified mod APIs, imported guarded plans, archive formats/metadata, Unicode and path-independent identity PASS");
}
