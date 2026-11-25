#include "recomp_mod.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s (%s)\n",__LINE__,#x,rmod_error(context));return 1;}}while(0)
static rmod_context* active=nullptr;
static int activations=0;
static void plugin(){char value[16],asset[2048];if(!rmod_option_get(active,"level",value,sizeof(value))||std::strcmp(value,"5")||!rmod_asset_get(active,"assets/note.txt",asset,sizeof(asset))||rmod_asset_get(active,"../escape.txt",asset,sizeof(asset)))std::abort();++activations;}
static void other_plugin(){}
struct Engine {
    std::array<uint8_t,8> ram{{1,2,3,4,5,6,7,8}};
    std::array<uint8_t,8> cartridge{{8,7,6,5,4,3,2,1}};
    int fail_write=0,writes=0,hooks=0,commits=0,rollbacks=0;
    bool fail_restore=false;
    bool fail_hook=false;
};
static int begin(void* user){static_cast<Engine*>(user)->writes=0;return 1;}
static uint8_t* memory(Engine* e,const char* space){return !std::strcmp(space,"ram")?e->ram.data():!std::strcmp(space,"cartridge")?e->cartridge.data():nullptr;}
static int read(void* u,const char* s,uint64_t a,uint8_t* out,size_t n){auto* p=memory(static_cast<Engine*>(u),s);if(!p||a>8||n>8-a)return 0;std::memcpy(out,p+a,n);return 1;}
static int write(void* u,const char* s,uint64_t a,const uint8_t* value,size_t n){auto* e=static_cast<Engine*>(u);auto* p=memory(e,s);if(!p||a>8||n>8-a)return 0;++e->writes;if(e->fail_restore&&e->writes>2)return 0;std::memcpy(p+a,value,n);return e->writes!=e->fail_write;}
static int hook(void* u,const char*){auto* e=static_cast<Engine*>(u);++e->hooks;return !e->fail_hook;}
static void finish(void* u,int committed){auto* e=static_cast<Engine*>(u);if(committed)++e->commits;else{++e->rollbacks;e->hooks=0;}}
int main(int argc,char** argv){
    if(argc!=3)return 2;
    const std::filesystem::path root=argv[1];
    rmod_context* context=rmod_create();active=context;
    CHECK(context);
    const auto initialize=[&](rmod_context* c,const char* name){return rmod_initialize(c,(root/name).string().c_str(),"fixture",argv[2]);};
    const auto install=[&](const char* name){return rmod_install(context,(root/(std::string(name)+".rmod")).string().c_str());};
    const auto original=(root/"original.bin").string();
    CHECK(initialize(context,"catalog"));
    CHECK(rmod_register_plugin(context,"base.plugin","video.renderer",plugin));
    CHECK(rmod_register_plugin(context,"conflict.plugin","video.renderer",other_plugin));
    CHECK(install("base"));CHECK(install("dependent"));CHECK(install("conflict"));
    CHECK(!install("base"));CHECK(!install("traversal"));CHECK(!install("invalid"));CHECK(!install("corrupt"));
    CHECK(rmod_feature_count(context)==3);rmod_feature_info info{};CHECK(rmod_feature_at(context,0,&info));CHECK(!rmod_feature_at(context,3,&info));
    CHECK(install("resource"));CHECK(rmod_enable(context,"resource","main",1));
    CHECK(!rmod_commit(context,original.c_str()));
    CHECK(rmod_resource_set(context,"resource","main","donor",original.c_str()));
    CHECK(rmod_enable(context,"dependent","main",1));CHECK(!rmod_commit(context,original.c_str()));
    CHECK(rmod_diagnostic_count(context)>0);rmod_diagnostic_info diagnostic{};CHECK(rmod_diagnostic_at(context,0,&diagnostic));
    CHECK(rmod_enable(context,"base","main",1));CHECK(!rmod_option_set(context,"base","main","level","4"));
    CHECK(rmod_option_set(context,"base","main","level","5"));CHECK(rmod_commit(context,original.c_str()));
    std::array<uint8_t,32> accepted{},after{};rmod_identity(context,accepted.data());
    CHECK(rmod_activate(context));CHECK(activations==1);
    CHECK(rmod_enable(context,"conflict","main",1));CHECK(!rmod_commit(context,original.c_str()));
    rmod_identity(context,after.data());CHECK(after==accepted);
    CHECK(rmod_enable(context,"conflict","main",0));
    CHECK(!rmod_commit(context,(root/"wrong.bin").string().c_str()));
    rmod_identity(context,after.data());CHECK(after==accepted);
    auto* independent=rmod_create();CHECK(independent);CHECK(initialize(independent,"other"));CHECK(rmod_feature_count(independent)==0);
    CHECK(rmod_feature_count(context)==4);rmod_destroy(independent);
    auto* reload=rmod_create();CHECK(reload);CHECK(initialize(reload,"catalog"));
    CHECK(rmod_register_plugin(reload,"base.plugin","video.renderer",plugin));CHECK(rmod_commit(reload,original.c_str()));
    rmod_identity(reload,after.data());CHECK(after==accepted);rmod_destroy(reload);
    Engine engine;const auto initial_ram=engine.ram,initial_cartridge=engine.cartridge;
    rmod_engine bus{&engine,"fixture",argv[2],begin,read,write,hook,finish};
    const uint8_t before[]={1,2},replacement[]={9,10},cart_before[]={8,7};
    rmod_patch patches[]={{"ram",0,before,replacement,2},{"cartridge",0,cart_before,replacement,2}};
    const rmod_hook hooks[]={{"native.callback"}};
    std::array<uint8_t,32> fingerprint{};
    auto wrong_engine=bus;wrong_engine.game_id="different";
    CHECK(!rmod_apply(context,patches,2,hooks,1,&wrong_engine,fingerprint.data()));CHECK(engine.writes==0);
    auto overlap=patches[0];rmod_patch overlapping[]={patches[0],overlap};
    CHECK(!rmod_apply(context,overlapping,2,nullptr,0,&bus,fingerprint.data()));CHECK(engine.writes==0);
    engine.ram[0]=0;CHECK(!rmod_apply(context,patches,2,hooks,1,&bus,fingerprint.data()));CHECK(engine.cartridge==initial_cartridge);engine.ram=initial_ram;
    engine.fail_write=2;CHECK(!rmod_apply(context,patches,2,hooks,1,&bus,fingerprint.data()));CHECK(engine.ram==initial_ram&&engine.cartridge==initial_cartridge);
    engine.fail_write=0;engine.fail_hook=true;CHECK(!rmod_apply(context,patches,2,hooks,1,&bus,fingerprint.data()));CHECK(engine.ram==initial_ram&&engine.cartridge==initial_cartridge&&engine.hooks==0);
    engine.fail_hook=false;engine.fail_write=2;engine.fail_restore=true;
    CHECK(rmod_apply(context,patches,2,hooks,1,&bus,fingerprint.data())==-1);
    engine.fail_write=0;engine.fail_restore=false;engine.ram=initial_ram;engine.cartridge=initial_cartridge;
    engine.fail_hook=false;CHECK(rmod_apply(context,patches,2,hooks,1,&bus,fingerprint.data())==1);CHECK(engine.ram[0]==9&&engine.cartridge[0]==9&&engine.commits==1);
    auto first_fingerprint=fingerprint;engine.ram=initial_ram;engine.cartridge=initial_cartridge;
    CHECK(rmod_apply(context,patches,2,hooks,1,&bus,fingerprint.data())==1);CHECK(fingerprint==first_fingerprint);
    // Catalog contents are bound to the identity, independent of host paths.
    std::ofstream(root/"catalog/packages/base/1/assets/note.txt")<<"different";
    CHECK(rmod_commit(context,original.c_str()));rmod_identity(context,after.data());CHECK(after!=accepted);
    rmod_destroy(context);std::puts("package manifests, imports, identity, context isolation and guarded transactions passed");return 0;
}
