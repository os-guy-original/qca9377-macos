#ifndef STUB_IOPCI_H
#define STUB_IOPCI_H
#include <IOKit/IOService.h>
struct IOMemoryMap {
    void *getVirtualAddress();
    unsigned long long getPhysicalAddress();
};
class IOMemoryDescriptor_ {
public:
    virtual unsigned long long getLength();
    virtual unsigned long long getPhysicalAddress();
    IOMemoryMap *map();
};
class IODeviceMemory : public IOMemoryDescriptor_ {};
class IOPCIDevice : public IOService {
public:
    unsigned short configRead16(unsigned off);
    unsigned char  configRead8(unsigned off);
    void configWrite16(unsigned off, unsigned short v);
    IODeviceMemory *getDeviceMemoryWithIndex(int i);
};
#endif
