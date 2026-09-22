#ifndef STUB_OSBYTEORDER_H
#define STUB_OSBYTEORDER_H
static inline unsigned short OSReadLittleInt16(const volatile void*b,long o){
    unsigned short v; const volatile char*p=(const volatile char*)b+o;
    v=(unsigned short)p[0]|((unsigned short)p[1]<<8); return v; }
static inline unsigned int   OSReadLittleInt32(const volatile void*b,long o){
    unsigned int v; const volatile char*p=(const volatile char*)b+o;
    v=(unsigned int)(unsigned char)p[0]|((unsigned int)(unsigned char)p[1]<<8)|
      ((unsigned int)(unsigned char)p[2]<<16)|((unsigned int)(unsigned char)p[3]<<24); return v; }
static inline void OSWriteLittleInt32(volatile void*b,long o,unsigned int v){
    volatile char*p=(volatile char*)b+o;
    p[0]=(char)(v&0xff); p[1]=(char)((v>>8)&0xff); p[2]=(char)((v>>16)&0xff); p[3]=(char)(v>>24); }
#endif
