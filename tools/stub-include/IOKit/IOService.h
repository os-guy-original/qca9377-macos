#ifndef STUB_IOSERVICE_H_0
#define STUB_IOSERVICE_H_0
#include <libkern/c++/OSObject.h>
#include <libkern/OSByteOrder.h>
#include <IOKit/IOLib.h>
class OSDictionary;
class OSMetaClass {};
typedef unsigned long long IOByteCount;
class OSObject { public: virtual ~OSObject(){} };
class IOService {
public:
    virtual bool init(OSDictionary *d = 0);
    virtual void free();
    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    void registerService();
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
class IORegistryEntry {
public:
    static IORegistryEntry *fromPath(const char *path, const void *plane);
    bool setProperty(const char *aKey, const char *value);
    void release();
};
extern const void *gIODTPlane;
#define OSDynamicCast(type, inst) ((type *)(inst))
#endif
