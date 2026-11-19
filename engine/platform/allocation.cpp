#include <core/defines.h>
#include <core/support.h>

// Allocation/zeroing/placement now use the WHOLE original Spt::Class.cpp.
// Modern C++ requires matching class-local deallocation for those overloads.
namespace Spt {
void Class::operator delete(void* p) noexcept { Mem::Manager::sHandle().Delete(p); }
void Class::operator delete[](void* p) noexcept { Mem::Manager::sHandle().Delete(p); }
}
