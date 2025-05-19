#ifndef CVM_OBJECT_H
#define CVM_OBJECT_H

#include "values.h"
#include "common.h"
#include "memory.h"
#include "chunk.h"

typedef enum ObjType {
    OBJ_STRING,
    OBJ_FUNCTION,
    OBJ_NATIVE,
    OBJ_CLOSURE,
    OBJ_UPVALUE
} ObjType;

typedef struct Obj {
    ObjType type;
    struct Obj* next;
    bool is_gc_marked;
} Obj;

typedef struct  ObjString {
    Obj obj;
    int length;
    char* chars;
    uint32_t hash;
} ObjString;

typedef struct {
    Obj obj;
    int arity;
    Chunk chunk;
    ObjString* name;
    int upvalue_count;
} ObjFunction; 

typedef Value(*NativeFn)(int argCount, Value* args, bool* success);

typedef struct ObjUpvalue {
    Obj obj;
    Value* location;
    struct ObjUpvalue* next;
    Value closed;
} ObjUpvalue;

typedef struct {
    Obj obj;
    ObjFunction* function;
    ObjUpvalue** upvalues;
    int upvalues_count;
} ObjClosure;

typedef struct {
    Obj obj;
    NativeFn function;
} ObjNative;

ObjFunction* new_function();
ObjNative* new_native(NativeFn function);
ObjClosure* new_closure(ObjFunction* function);
ObjUpvalue* new_upvalue(Value* slot);

ObjString* copy_string(const char* chars, int length);
ObjString* new_string(const char* chars, int length);
bool is_obj_type(Value v, ObjType type);

#define OBJ_TYPE(value) (AS_OBJ(value)->type)
#define IS_STRING(value) (is_obj_type(value, OBJ_STRING))


#define AS_STRING(value) ((ObjString*)AS_OBJ(value))
#define AS_CSTRING(value) (((ObjString*)AS_OBJ(value))->chars)

void freeObj(Obj* t);

#endif