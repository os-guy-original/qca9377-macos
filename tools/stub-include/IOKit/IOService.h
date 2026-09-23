#ifndef STUB_IOSERVICE_H_0
#define STUB_IOSERVICE_H_0
#include <libkern/c++/OSObject.h>
#include <libkern/OSByteOrder.h>
#include <IOKit/IOLib.h>
class OSDictionary;
class OSMetaClass {};
typedef unsigned long long IOByteCount;
class OSObject { public: virtual ~OSObject(){} };
// v0.9.3: IODTNVRAM service route (KC-verified exports)
//   __ZN9IOService15serviceMatchingEPKcP12OSDictionary
//   __ZN9IOService14waitForServiceEP12OSDictionaryP13mach_timespec
struct mach_timespec { unsigned int tv_sec; unsigned int tv_nsec; };
typedef struct mach_timespec mach_timespec_t;
class IOService {
public:
    virtual bool init(OSDictionary *d = 0);
    virtual void free();
    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    void registerService();
    void release(); // real: inherited from OSObject
    // v0.9.3 service lookup (real signatures per IOKit headers)
    static OSDictionary *serviceMatching(const char *name, OSDictionary *table = 0);
    static IOService *waitForService(OSDictionary *matching, mach_timespec_t *timeout = 0);
    bool getProperty(const char *aKey, const char **value); // real: OSString unwrap
    // real SDK: IORegistryEntry setProperty overloads (const char* keys auto-wrap)
    bool setProperty(const char *aKey, const char *value);
    bool setProperty(const char *aKey, unsigned long long value, unsigned int numberOfBits);
    bool setProperty(const char *aKey, const void *bytes, unsigned int length);
};
#define OSDeclareDefaultStructors(name) \
    static class OSMetaClass * sMeta; \
    virtual const OSMetaClass * getMetaClass() const; \
    name(); virtual ~name();
#define OSDefineMetaClassAndStructors(cls, super) \
    OSMetaClass * cls::sMeta = 0; \
    const OSMetaClass * cls::getMetaClass() const { return 0; } \
    cls::cls() {} cls::~cls() {}
class OSDictionary { public: static OSDictionary *withCapacity(int); void release(); };
// IORegistryEntry now stubbed in its own header (mirrors real SDK layout)
#define OSDynamicCast(type, inst) ((type *)(inst))
#endif
