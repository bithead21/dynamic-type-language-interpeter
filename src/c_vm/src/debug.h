#ifndef CVM_DEBUG_H
#define CVM_DEBUG_H

#include "common.h"
#include "chunk.h"

void disassembleChunk(Chunk* chunk, const char* name);
int disassembleInstruction(Chunk* chunk, int offset);
void print_object(Value v);
void print_value(Value v);

#endif