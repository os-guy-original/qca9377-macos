#ifndef STUB_IOBMD_H
#define STUB_IOBMD_H
#include <sys/types.h>
#define kIODirectionInOut 0x3
#define kIOMemoryPhysicallyContiguous 0x1000000
#define kIOMapInhibitCache 0x400
#define kIOReturnSuccess 0
extern void *kernel_task;
class IOBufferMemoryDescriptor {
public:
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(
        void *task, unsigned options, unsigned long long size,
        unsigned long long mask);
    int prepare();
    void complete();
    void *getBytesNoCopy();
    void release();
};
#endif
