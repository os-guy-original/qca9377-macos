#ifndef STUB_IOREGISTRYENTRY_H_0
#define STUB_IOREGISTRYENTRY_H_0
// stub of the real IOKit/IORegistryEntry.h surface we use:
//   static fromPath, setProperty, release, and the registry plane globals
class IORegistryEntry {
public:
    static IORegistryEntry *fromPath(const char *path, const void *plane);
    bool setProperty(const char *aKey, const char *value);
    bool setProperty(const char *aKey, unsigned long long value, unsigned int numberOfBits);
    void release();
};
extern const void *gIODTPlane;
extern const void *gIOServicePlane;
#endif
