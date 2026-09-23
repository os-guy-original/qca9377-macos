#ifndef STUB_IOLIB_H
#define STUB_IOLIB_H
#include <libkern/c++/OSObject.h>
#include <libkern/OSByteOrder.h>
extern "C" void IODelay(unsigned long);
extern "C" void IOSleep(unsigned long);
extern "C" void OSSynchronizeIO(void);
#endif
