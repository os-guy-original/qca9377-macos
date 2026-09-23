#ifndef STUB_IOREGISTRYENTRY_H_0
#define STUB_IOREGISTRYENTRY_H_0
// stub of the real IOKit/IORegistryEntry.h surface we use:
//   static fromPath, static getPlane, setProperty, release.
// NOTE: the real SDK header does NOT declare gIODTPlane as an extern
// global (xnu removed it) and does NOT export setProperty(const char*,
// OSObject*) to kexts. The driver uses getPlane("IODT") instead (KC:
// __ZN15IORegistryEntry8getPlaneEPKc) — mirrored here so local gates
// match the SDK surface.
class IORegistryPlane;
class IORegistryEntry {
public:
    static IORegistryEntry *fromPath(const char *path, const IORegistryPlane *plane);
    static const IORegistryPlane *getPlane(const char *name);
    bool setProperty(const char *aKey, const char *value);
    bool setProperty(const char *aKey, unsigned long long value, unsigned int numberOfBits);
    void release();
};
#endif
