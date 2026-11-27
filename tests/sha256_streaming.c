#include <recomp_mod.h>
#include <recomp_mod/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"sha stream line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void) {
    static const uint8_t abc[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    rmod_sha256_state state;uint8_t out[32],expected[32],data[1025];
    rmod_sha256_init(&state);rmod_sha256_update(&state,NULL,0);
    rmod_sha256_update(&state,(const uint8_t*)"a",1);rmod_sha256_update(&state,(const uint8_t*)"bc",2);
    rmod_sha256_final(&state,out);CHECK(!memcmp(out,abc,32));
    for(size_t i=0;i<sizeof(data);++i)data[i]=(uint8_t)(i*131u+7u);
    for(size_t length=0;length<=sizeof(data);++length) {
        CHECK(rmod_sha256_bytes(data,length,expected));
        for(size_t chunk=1;chunk<=65;++chunk) {
            rmod_sha256_init(&state);rmod_sha256_update(&state,NULL,0);
            for(size_t at=0;at<length;) {
                size_t n=length-at;if(n>chunk)n=chunk;
                rmod_sha256_update(&state,data+at,n);at+=n;rmod_sha256_update(&state,NULL,0);
            }
            rmod_sha256_final(&state,out);CHECK(!memcmp(out,expected,32));
        }
    }
    puts("SHA256 streaming PASS: known vector; 1026 lengths x65 chunk sizes with empty updates, partial/full/tail boundaries");
    return 0;
}
