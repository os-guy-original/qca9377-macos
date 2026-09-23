#ifndef STUB_IOREGISTRYENTRY_H_0
#define STUB_IOREGISTRYENTRY_H_0
// stub of the real IOKit/IORegistryEntry.h surface we use:
//   static fromPath, setProperty, release.
// NOTE: the real SDK header does NOT declare gIODTPlane (xnu removed the
// extern; the kernel still exports it). The driver declares it itself in
// QCA9377Driver.cpp — the stub deliberately does NOT re-declare it, so a
// future local compile catches a driver that relies on the SDK to provide it.
class IORegistryPlane;
class IORegistryEntry {
public:
    static IORegistryEntry *fromPath(const char *path, const IORegistryPlane *plane);
    bool setProperty(const char *aKey, const char *value);
    bool setProperty(const char *aKey, unsigned long long value, unsigned int numberOfBits);
    void release();
};
#endif
