#ifndef CVM_H
#define CVM_H

#include "tools/hashtable.h"
#include "chunk.h"
#include "debug.h"


typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR
} INTERPRET_RESULT;

#define VM_STACK_MAX 256
#define VM_FRAMES_MAX 64

typedef struct {
    Obj* function;
    ObjClosure* closure;
    //ObjFunction* function;
    uint8_t* ip;
    Value* slots; // stack
 } CallFrame;

typedef struct {
    CallFrame frames[VM_FRAMES_MAX];
    int frames_count;
    //Chunk* chunk;
    //uint8_t* instr_ptr;
    // --- stack ----
    Value stack[VM_STACK_MAX];
    Value* stack_top;
    Obj* objects;
    // ---- strings ----
    Hashtable strings;
    Hashtable globals;
    // ----- upvalues ---
    ObjUpvalue* open_upvalues;

    // gray stack for gc
    int gray_count;
    int gray_capacity;
    Obj** gray_stack;
} VM;

extern VM vm;

void vm_init();
void vm_destroy();
INTERPRET_RESULT vm_interpret_source(const char* source);

void vm_stack_push(Value v);
Value vm_stack_pop();

#endif