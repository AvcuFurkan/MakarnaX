#ifndef TYPEDEFS_H
#define TYPEDEFS_H

#include <stddef.h>

typedef unsigned int   uint32_t;
typedef          int   int32_t;
typedef unsigned short uint16_t;
typedef          short int16_t;
typedef unsigned char  uint8_t;
typedef          char  int8_t;

typedef char* ptr_t;

typedef int key_t;

#define asmlink extern "C"

#endif /* TYPEDEFS_H */