/* OpenCL built-in library: clz()

   Copyright (c) 2011 Universidad Rey Juan Carlos
   
   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:
   
   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.
   
   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/

#include "templates.h"

/* These implementations return 8*sizeof(TYPE) when the input is 0 */

/* The explicit implementations are taken from
   <http://aggregate.org/MAGIC/>:
   
   @techreport{magicalgorithms,
   author={Henry Gordon Dietz},
   title={{The Aggregate Magic Algorithms}},
   institution={University of Kentucky},
   howpublished={Aggregate.Org online technical report},
   date={2013-03-25},
   URL={http://aggregate.org/MAGIC/}
   }
*/


/* __builtin_clz() is undefined for 0 */

#if __has_builtin(__builtin_clzc)
#  define __builtin_clz0hh(n)                           \
  ({ char __n=(n); __n==0 ? 8 : __builtin_clzc(__n); })
#elif __has_builtin(__builtin_clzs)
#  define __builtin_clz0hh(n)                   \
  ((char)(__builtin_clz0h((char)(n)) - 8))
#elif __has_builtin(__builtin_clz)
#  define __builtin_clz0hh(n)                   \
  ((char)(__builtin_clz0((char)(n)) - 24))
#else
#  define __builtin_clz0hh(n) ({                \
      ushort __n=(n);                           \
      __n |= __n >> 1;                          \
      __n |= __n >> 2;                          \
      __n |= __n >> 4;                          \
      8 - popcount(__n);                        \
    })
#endif

#if __has_builtin(__builtin_clzs)
#  define __builtin_clz0h(n)                                    \
  ({ short __n=(n); __n==0 ? 16 : __builtin_clzs(__n); })
#elif __has_builtin(__builtin_clz)
#  define __builtin_clz0h(n)                    \
  ((short)(__builtin_clz0((short)(n)) - 16))
#else
#  define __builtin_clz0h(n) ({                 \
      ushort __n=(n);                           \
      __n |= __n >> 1;                          \
      __n |= __n >> 2;                          \
      __n |= __n >> 4;                          \
      __n |= __n >> 8;                          \
      16 - popcount(__n);                       \
    })
#endif

#if __has_builtin(__builtin_clz)
#  define __builtin_clz0(n)                               \
  ({ int __n=(n); __n==0 ? 32 : __builtin_clz(__n); })
#else
#  define __builtin_clz0(n) ({                  \
      uint __n=(n);                             \
      __n |= __n >> 1;                          \
      __n |= __n >> 2;                          \
      __n |= __n >> 4;                          \
      __n |= __n >> 8;                          \
      __n |= __n >> 16;                         \
      32 - popcount(__n);                       \
    })
#endif

#if __has_builtin(__builtin_clzl)
#  define __builtin_clz0l(n)                                      \
  ({ long __n=(n); __n==0 ? 64 : __builtin_clzl(__n); })
#else
#  define __builtin_clz0l(n) ({                 \
      ulong __n=(n);                            \
      __n |= __n >> 1;                          \
      __n |= __n >> 2;                          \
      __n |= __n >> 4;                          \
      __n |= __n >> 8;                          \
      __n |= __n >> 16;                         \
      __n |= __n >> 32;                         \
      64 - popcount(__n);                       \
    })
#endif

#define __builtin_clz0uhh(n) __builtin_clz0hh(n)
#define __builtin_clz0uh(n)  __builtin_clz0h(n)
#define __builtin_clz0u(n)   __builtin_clz0(n)
#define __builtin_clz0ul(n)  __builtin_clz0l(n)

#define clz0 clz
DEFINE_BUILTIN_G_G(clz0)
#undef clz0
