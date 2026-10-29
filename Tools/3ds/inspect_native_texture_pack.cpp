// Host-only contract inspector, never a rendering executable.
#include "../../Source/platforms/3ds/native/texture_pack.hpp"
#include <cstdio>
#include <vector>
int main(int argc,char** argv){
    if(argc!=2)return 2;
    FILE* file=std::fopen(argv[1],"rb");if(!file)return 3;
    std::fseek(file,0,SEEK_END);const auto size=std::ftell(file);std::rewind(file);
    if(size<32||size>64*1024*1024){std::fclose(file);return 4;}
    std::vector<std::byte> bytes(size);const bool read=std::fread(bytes.data(),1,bytes.size(),file)==bytes.size();std::fclose(file);if(!read)return 5;
    dusk::pica::TexturePack pack;if(dusk::pica::TexturePack::open(bytes,pack)!=dusk::pica::PackError::none)return 6;
    unsigned formats[4]{};std::size_t payload=0;
    // Source compiler emits contiguous original-index +1 IDs. A generic pack
    // may have sparse IDs; this private source-bank inspector requires these.
    for(unsigned id=1;id<=pack.size();++id){dusk::pica::TextureView view;
        if(pack.image(id,view)!=dusk::pica::PackError::none)return 7;
        ++formats[unsigned(view.image.format)];payload+=view.payload.size();
    }
    std::printf("{\"images\":%u,\"native_payload_bytes\":%zu,\"rgba8\":%u,\"rgb565\":%u,\"etc1\":%u,\"etc1a4\":%u}\n",pack.size(),payload,formats[0],formats[1],formats[2],formats[3]);
}
