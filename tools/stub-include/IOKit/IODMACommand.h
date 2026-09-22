#ifndef STUB_IODMAC_H
#define STUB_IODMAC_H
#include <stdint.h>
#define kIOReturnSuccess 0
enum Output { kIODMACommandOutputHost64 = 1 };
class IODMACommand {
public:
    enum Mapping { kMapped };
    struct Segment64 { uint64_t fIOVMAddr; uint64_t fLength; };
    static IODMACommand *withSpecification(Output, unsigned char, unsigned long long,
        Mapping, unsigned long long, unsigned, void*, void*);
    int setMemoryDescriptor(void*, bool);
    void clearMemoryDescriptor(bool);
    int gen64IOVMSegments(uint64_t*, Segment64*, unsigned*);
    void release();
};
#endif
