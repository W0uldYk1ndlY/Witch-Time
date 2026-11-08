/* Compile with the target toolchain: the runtime and lifted C share this ABI. */
#include <setjmp.h>
typedef jmp_buf native_jmp_buf;
#define __jmp_buf lifted___jmp_buf
#define __sigset_t lifted___sigset_t
#define __jmp_buf_tag lifted___jmp_buf_tag
#define jmp_buf lifted_jmp_buf
#undef setjmp
#define setjmp lifted_setjmp
#include "../runtime/freestanding/setjmp.h"
#undef setjmp
#undef jmp_buf
_Static_assert(sizeof(native_jmp_buf) == sizeof(lifted_jmp_buf), "lifted/runtime jmp_buf size mismatch");
_Static_assert(_Alignof(native_jmp_buf) == _Alignof(lifted_jmp_buf), "lifted/runtime jmp_buf alignment mismatch");
