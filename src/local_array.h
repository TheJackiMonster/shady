#ifndef SHADY_LOCAL_ARRAY_H
#define SHADY_LOCAL_ARRAY_H

#include <stdlib.h>

//#define LARRAY(T, name, size) T name[size]
#define LARRAY(T, name, size) T* name = alloca(sizeof(T) * (size))

#endif
