// Backend de referência: os dispositivos são os modelos em C++ de src/ (sem Host Fabric). Ver docs/HOST-FABRIC.md.
#include "soc.h"

namespace sf {

void fabric_attach(Soc&) {}
const char* fabric_backend() { return "ref"; }

}  // namespace sf
