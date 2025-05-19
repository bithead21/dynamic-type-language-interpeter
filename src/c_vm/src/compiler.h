#ifndef CVM_COMPILER_H
#define CVM_COMPILER_H

//#define COMPILER_DEBUG_TRACE

#ifdef COMPILER_DEBUG_TRACE
#define COMPILER_DEBUG_LOG(msg) printf(msg)
#else 
#define COMPILER_DEBUG_LOG(msg)
#endif


#include "scanner.h"
#include "vm.h"

ObjFunction* compile(const char* source);
void gc_mark_compiler_roots();

#endif