// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the real worker socket, independent AU acknowledgements, frame
 * tokens/PTS, EOS, reset and descriptor reuse against a software reference. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "worker_protocol.h"
#ifndef W
#define W 128
#endif
#ifndef H
#define H 96
#endif
#define BYTES (W*H*3/2)
struct nal { size_t offset, size; unsigned type; };
static unsigned got;
static uint8_t *reference;
static unsigned cycle;
static unsigned frames;
static void die(const char *s) { perror(s); exit(1); }
static void io(int fd, void *buf, size_t n, int sending)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t done = sending ? send(fd, p, n, MSG_NOSIGNAL) : recv(fd, p, n, 0);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) die("socket io");
        p += done; n -= done;
    }
}
static void request(int fd, uint32_t command, uint64_t token, const void *p, size_t size)
{
    struct mt6580_c2_header h = {MT6580_C2_MAGIC, MT6580_C2_VERSION, command, size,
        token, (int64_t)token*42000, cycle+1, command==MT6580_C2_DRAIN ? MT6580_C2_FLAG_EOS:0};
    io(fd, &h, sizeof(h), 1);
    if (size) io(fd, (void *)p, size, 1);
    for (unsigned packets = 0; packets < 256; ++packets) {
        struct mt6580_c2_header r;
        io(fd, &r, sizeof(r), 0);
        if (r.magic != MT6580_C2_MAGIC || r.version != MT6580_C2_VERSION ||
            r.generation != cycle+1 || r.payload_bytes > BYTES+16) die("bad response header");
        uint8_t payload[BYTES+16];
        io(fd, payload, r.payload_bytes, 0);
        if (r.command == MT6580_C2_ERROR) {
            int32_t error = 0; memcpy(&error, payload, r.payload_bytes < 4 ? r.payload_bytes : 4);
            fprintf(stderr, "WORKER_ERROR=%d request=%u cycle=%u\n", error, command, cycle); exit(1);
        }
        if (r.command == MT6580_C2_FORMAT) {
            struct mt6580_c2_format format;
            if (r.payload_bytes != sizeof(format)) die("format size");
            memcpy(&format, payload, sizeof(format));
            if (format.width != W || format.height != H || r.flags) die("format mismatch");
        } else if (r.command == MT6580_C2_FRAME) {
            struct mt6580_c2_frame_info info; memcpy(&info, payload, sizeof(info));
            if (info.width != W || info.height != H || info.bytes != BYTES || info.reserved ||
                got>=frames || r.payload_bytes!=sizeof(info)+BYTES || r.flags ||
                r.token!=got+1 || r.pts!=(int64_t)r.token*42000 ||
                memcmp(payload+sizeof(info), reference+(got%8)*BYTES, BYTES)) {
                fprintf(stderr, "FRAME_MISMATCH cycle=%u index=%u token=%llu pts=%lld\n", cycle, got,
                    (unsigned long long)r.token, (long long)r.pts); exit(1);
            }
            got++;
        } else if (r.command == MT6580_C2_ACK || r.command == MT6580_C2_DRAINED) {
            if (r.token != token || r.payload_bytes ||
                r.command != (command==MT6580_C2_DRAIN ? MT6580_C2_DRAINED:MT6580_C2_ACK) ||
                r.flags != (command==MT6580_C2_DRAIN ? MT6580_C2_FLAG_EOS:0)) die("bad ack");
            return;
        } else die("unknown response");
    }
    die("too many packets");
}
static uint8_t *file(const char *name, size_t *size)
{
    FILE *f=fopen(name,"rb"); if(!f)die("fopen");
    if(fseek(f,0,SEEK_END))die("seek"); long n=ftell(f);
    if(n<=0 || n>32*1024*1024)die("file size");
    rewind(f); uint8_t *p=malloc(n); if(!p || fread(p,1,n,f)!=(size_t)n)die("read");
    fclose(f); *size=n; return p;
}
int main(int argc, char **argv)
{
    if (argc!=5 && argc!=6) { fprintf(stderr,"worker-check SOCKET H264 REFERENCE CYCLES [GOPS]\n"); return 2; }
    size_t size, refbytes; uint8_t *stream=file(argv[2],&size); reference=file(argv[3],&refbytes);
    if(refbytes!=8*BYTES)die("reference length");
    unsigned cycles=atoi(argv[4]); if(!cycles || cycles>1000)return 2;
    unsigned gops=argc==6?atoi(argv[5]):1; if(!gops || gops>128)return 2; frames=8*gops;
    struct nal nals[64]; unsigned count=0;
    for(size_t i=0;i+4<size;) {
        unsigned prefix=!memcmp(stream+i,"\0\0\0\1",4)?4:!memcmp(stream+i,"\0\0\1",3)?3:0;
        if(!prefix){i++;continue;}
        if(count==64)die("NAL count");
        if(count)nals[count-1].size=i-nals[count-1].offset;
        nals[count++]=(struct nal){i,size-i,stream[i+prefix]&31}; i+=prefix;
    }
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0); if(fd<0)die("socket");
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    if(strlen(argv[1])>=sizeof(addr.sun_path))return 2;
    strcpy(addr.sun_path,argv[1]); if(connect(fd,(struct sockaddr *)&addr,sizeof(addr)))die("connect");
    for(cycle=0;cycle<cycles;cycle++) {
        if(cycle)request(fd,MT6580_C2_RESET,0,NULL,0);
        got=0;
        for(unsigned i=0;i<count;i++)if(nals[i].type==7||nals[i].type==8)
            request(fd,MT6580_C2_CONFIG,0,stream+nals[i].offset,nals[i].size);
        unsigned token=0;
        for(unsigned g=0;g<gops;g++)
        for(unsigned i=0;i<count;i++)if(nals[i].type==1||nals[i].type==5)
            request(fd,MT6580_C2_AU,++token,stream+nals[i].offset,nals[i].size);
        request(fd,MT6580_C2_DRAIN,0,NULL,0);
        if(got!=frames)die("missing frames after drain");
        printf("WORKER_CYCLE_MATCH cycle=%u frames=%u\n",cycle,got);
    }
    cycle--;
    request(fd,MT6580_C2_CLOSE,0,NULL,0); close(fd);
    printf("WORKER_SOCKET_MATCH cycles=%u frames=%u pts_match=1 reset_eos=1\n", cycles,cycles*frames);
    return 0;
}
