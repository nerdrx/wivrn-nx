#include "driver/nx_fuse_corrections.h"
#include "os/os_time.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace wivrn;
static void put16(std::byte * p, uint16_t v) { p[0]=std::byte(v); p[1]=std::byte(v>>8); }
static void put32(std::byte * p, uint32_t v) { for (int i=0;i<4;++i) p[i]=std::byte(v>>(8*i)); }
static void put64(std::byte * p, uint64_t v) { for (int i=0;i<8;++i) p[i]=std::byte(v>>(8*i)); }
static void putf(std::byte * p, float v) { uint32_t b; std::memcpy(&b,&v,4); put32(p,b); }
static int sender(const char * path)
{
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0); sockaddr_un a{}; a.sun_family=AF_UNIX;
    std::strncpy(a.sun_path,path,sizeof(a.sun_path)-1); if (connect(fd,(sockaddr*)&a,sizeof(a))) return -1; return fd;
}
static std::array<std::byte, 40+8*16+1> packet(uint64_t seq,uint64_t gen,uint8_t count,float x=0.1f,uint64_t host=0)
{
    std::array<std::byte,40+8*16+1> p{}; std::memcpy(p.data(),"NXFC",4); put16(p.data()+4,1); put16(p.data()+6,40);
    put64(p.data()+8,seq); put64(p.data()+16,host?host:os_monotonic_get_ns()); put64(p.data()+24,gen); p[33]=std::byte(count); p[34]=std::byte(1);
    for (unsigned i=0;i<count;++i) { auto * r=p.data()+40+i*16; put16(r, 10); putf(r+4,x); }
    return p;
}
static void send_packet(int fd,const auto & p,size_t size) { if (send(fd,p.data(),size,0)!=(ssize_t)size) std::exit(10); }
static std::array<std::byte, 40+5*32+1> camera_packet(uint64_t seq,uint64_t gen,uint8_t count,float x=1.f)
{
    std::array<std::byte,40+5*32+1> p{}; std::memcpy(p.data(),"NXFC",4); put16(p.data()+4,1); put16(p.data()+6,40);
    put64(p.data()+8,seq); put64(p.data()+16,os_monotonic_get_ns()); put64(p.data()+24,gen); p[32]=std::byte(1); p[33]=std::byte(count); p[34]=std::byte(1);
    for (unsigned i=0;i<count;++i) { auto * r=p.data()+40+i*32; put16(r,10); putf(r+4,x); putf(r+16,0); putf(r+20,0); putf(r+24,0); putf(r+28,1); }
    return p;
}
static from_headset::bd_body body()
{
    from_headset::bd_body b{}; b.joints[XR_BODY_JOINT_PELVIS_BD].position={1,2,3}; b.joints[XR_BODY_JOINT_PELVIS_BD].orientation=packed_quaternion::from_quaternion({.w=1}); b.joints[XR_BODY_JOINT_PELVIS_BD].flags=0x33; return b;
}
static bool moved(nx_fuse_corrections & c,uint64_t gen,float expected)
{
    auto b=body(); const auto before=b; c.apply(b,gen);
    for (size_t i=0;i<b.joints.size();++i) if (i != XR_BODY_JOINT_PELVIS_BD && std::memcmp(&b.joints[i], &before.joints[i], sizeof(b.joints[i]))) return false;
    auto & p=b.joints[XR_BODY_JOINT_PELVIS_BD];
    return std::abs(p.position.x-(1+expected))<.001f && p.flags==0x33 && p.orientation.value == packed_quaternion::from_quaternion({.w=1}).value;
}
int main(int argc,char ** argv)
{
    if (argc!=2 && argc!=3) return 2;
    setenv("NX_FUSE_CORRECTIONS",argv[1],1); unlink(argv[1]);
    nx_fuse_corrections c; int s=sender(argv[1]); if(s<0) return 3; const uint64_t g=77;
    if (argc == 3) {
        std::puts("READY"); std::fflush(stdout);
        char expected_line[64]{}; if (!std::fgets(expected_line, sizeof(expected_line), stdin)) return 18;
        const uint64_t expected_sample = std::strtoull(expected_line, nullptr, 10);
        // The Python production sender supplies the packets in this mode.
        for (int attempt=0; attempt<100; ++attempt) {
            c.poll(g);
            std::array<nx_fuse_corrections::camera_pose,5> camera{}; uint64_t sample=0;
            if (std::strcmp(argv[2], "camera_only") == 0 && c.camera_snapshot(g,camera,&sample)==1 && camera[0].role==10 && std::abs(camera[0].position.x-.2f)<.001f && std::abs(camera[0].orientation.w-1.f)<.001f && camera[0].position.y==1.f && camera[0].position.z==0.f && camera[0].orientation.x==0.f && camera[0].orientation.y==0.f && camera[0].orientation.z==0.f && moved(c,g,0.f) && sample == expected_sample) {
                auto empty = packet(UINT64_MAX-1,g,0); send_packet(s,empty,40); c.poll(g); if (c.camera_snapshot(g,camera)!=0) return 24;
                auto fresh = camera_packet(UINT64_MAX,g,1,2.f); send_packet(s,fresh,72); c.poll(g); if (c.camera_snapshot(g,camera)!=1) return 25;
                c.invalidate(g+1); if (c.camera_snapshot(g+1,camera)!=0) return 26;
                close(s); return 0;
            }
            auto b=body(); c.apply(b,g);
            const float correction=b.joints[XR_BODY_JOINT_PELVIS_BD].position.x-1;
            if (std::strcmp(argv[2], "assist") == 0 && correction > .001f && correction <= .1f) { close(s); return 0; }
            usleep(5000);
        }
        close(s); return 18;
    }
    auto p=packet(1,g,1,.1f); send_packet(s,p,56); c.poll(g); if(!moved(c,g,.1f)) return 4;
    usleep(180000); if(!moved(c,g,0)) return 5;
    p=packet(2,g,1,.2f); send_packet(s,p,56); c.poll(g); if(!moved(c,g,.2f)) return 6;
    p=packet(3,g,2,.1f); send_packet(s,p,72); c.poll(g); if(!moved(c,g,.2f)) return 7;
    p=packet(2,g,1,.1f); send_packet(s,p,56); c.poll(g); if(!moved(c,g,.2f)) return 8;
    p=packet(4,g+1,1,.1f); send_packet(s,p,56); c.poll(g); if(!moved(c,g,.2f)) return 9;
    p=packet(5,g,0); send_packet(s,p,40); c.poll(g); if(!moved(c,g,0)) return 10;
    p=packet(6,g,1,.2f); send_packet(s,p,56); c.poll(g); if(!moved(c,g,.2f)) return 11;
    p=packet(7,g,1,.1f); putf(p.data()+44,std::nanf("")); send_packet(s,p,56); c.poll(g); if(!moved(c,g,.2f)) return 12;
    p=packet(8,g,1,.3f); putf(p.data()+44,.26f); send_packet(s,p,56); c.poll(g); if(!moved(c,g,.2f)) return 13;
    p=packet(9,g,1,.1f); send_packet(s,p,169); c.poll(g); if(!moved(c,g,.2f)) return 14;
    auto cp=camera_packet(10,g,1,2.f); send_packet(s,cp,72); c.poll(g);
    std::array<nx_fuse_corrections::camera_pose,5> poses{}; uint64_t sample=0;
    if (c.camera_snapshot(g,poses,&sample)!=1 || poses[0].role!=10 || std::abs(poses[0].position.x-2.f)>.001f || poses[0].orientation.w!=1.f) return 19;
    usleep(180000); if (c.camera_snapshot(g,poses)!=0) return 20;
    cp=camera_packet(11,g,1,3.f); putf(cp.data()+40+28,std::nanf("")); send_packet(s,cp,72); c.poll(g); if (c.camera_snapshot(g,poses)!=0) return 21;
    cp=camera_packet(13,g,1,3.f); send_packet(s,cp,201); c.poll(g); if (c.camera_snapshot(g,poses)!=0) return 23;
    p=packet(12,g,0); send_packet(s,p,40); c.poll(g); if (c.camera_snapshot(g,poses)!=0) return 22;
    { nx_fuse_corrections competing; if(access(argv[1],F_OK)!=0) return 15; }
    if(access(argv[1],F_OK)!=0) return 16;
    c.invalidate(g+1); if(!moved(c,g+1,0)) return 17;
    close(s); return 0;
}
