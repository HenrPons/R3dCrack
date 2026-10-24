#pragma once
#include "KeySearchDevice.h"
namespace ResultSafety {
inline void requireComplete(unsigned count, unsigned capacity) {
    if(count > capacity) throw KeySearchException(
        "OpenCL result capacity exceeded; search stopped without advancing the checkpoint. "
        "Reduce the batch size and resume from the last saved checkpoint.");
}
}
