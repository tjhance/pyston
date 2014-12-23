namespace v8 {
namespace internal {

#include <cinttypes>

// Random stuff from other header files in V8 that I'm cramming in here

// A macro is used for defining the base class used for embedded instances.
// The reason is some compilers allocate a minimum of one word for the
// superclass. The macro prevents the use of new & delete in debug mode.
// In release mode we are not willing to pay this overhead.
#ifdef DEBUG
// Superclass for classes with instances allocated inside stack
// activations or inside other objects.
class Embedded {
 public:
  void* operator new(size_t size);
  void  operator delete(void* p); 
};
#define BASE_EMBEDDED : public Embedded
#else
#define BASE_EMBEDDED
#endif

constexpr int kPointerSize = sizeof(void*);
typedef uint8_t byte;

}
}
