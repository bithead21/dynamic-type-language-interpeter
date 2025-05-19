#include "object.h"
#include "stdio.h"
#include "string.h"
#include "vm.h"

bool is_obj_type(Value v, ObjType type) {
    return IS_OBJ(v) && AS_OBJ(v)->type == type;
};

void free_upvalues(ObjClosure* closure) {
    for (int i = 0; i < closure->upvalues_count; i++) {
        free(closure->upvalues[i]);
    }
}

Obj* allocate_obj(size_t size, ObjType type) {

    Obj* t = malloc(size);
    t->type = type;
    t->is_gc_marked = false;
    t->next = vm.objects;
    vm.objects = t;

    #ifdef DEBUG_LOG_GC
    printf("[gc] %p allocate %zu for %d\n", (void*)t, size, type);
    #endif

    return t;
};

void freeObj(Obj* t) {

    #ifdef DEBUG_LOG_GC
    printf("[gc] %p free obj %d\n", (void*)t, t->type);
    #endif

    switch (t->type)
    {
    case OBJ_STRING:
        ObjString* str = (ObjString*)t;
        free(str->chars);
        FREE(ObjString, t);
        break;
    
    case OBJ_FUNCTION:
        ObjFunction* func = (ObjFunction*)t;
        chunk_destroy(&func->chunk);
        FREE(ObjFunction, func);
        break;

    case OBJ_NATIVE:
        FREE(ObjNative, t);
        break;

    case OBJ_CLOSURE:
        ObjClosure* closure = (ObjClosure*)t;
        //free_upvalues(closure);
        free(closure->upvalues);
        FREE(ObjClosure, t);
        break;

    case OBJ_UPVALUE:
        FREE(ObjUpvalue, t);
        break;

    default:
        break;
    }
};

#define ALLOCATE_OBJ(type, objType)  \
    allocate_obj(sizeof(type), objType)


ObjFunction* new_function() {
    ObjFunction* f = ALLOCATE_OBJ(ObjFunction, OBJ_FUNCTION);
    f->arity = 0;
    f->name = NULL;
    f->upvalue_count = 0;
    chunk_init(&f->chunk, 24);
    return f;
};


ObjString* allocate_string(char* chars, int length, uint32_t hash) {
    ObjString* s = ALLOCATE_OBJ(ObjString, OBJ_STRING);
    s->length = length;
    s->hash = hash;
    s->chars = chars;

    // vm string collect
    hashtable_set(&vm.strings, s, NULL_VAL);

    return s;
};

uint32_t hash_string(const char* key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 167777619;
    }

    return hash;
};

ObjString* new_string(const char* chars, int length) {
    uint32_t hash = hash_string(chars, length);

    ObjString* intern_str = hashtable_find_string(&vm.strings, chars, length, hash);
    if (intern_str != NULL) return intern_str;

    return allocate_string(chars, length, hash);
};

ObjString* copy_string(const char* chars, int length) {
    uint32_t hash = hash_string(chars, length);

    ObjString* intern_str = hashtable_find_string(&vm.strings, chars, length, hash);
    if (intern_str != NULL) return intern_str;

    char* heapChars = ALLOCATE(char, length+1);
    memcpy(heapChars, chars, length);
    heapChars[length] = '\0';

    return allocate_string(heapChars, length, hash);
};

ObjNative* new_native(NativeFn function) {
    ObjNative* native = ALLOCATE_OBJ(ObjNative, OBJ_NATIVE);
    native->function = function;
    return native;
};

ObjClosure* new_closure(ObjFunction* function) {
    ObjClosure* closure = ALLOCATE_OBJ(ObjClosure, OBJ_CLOSURE);
    
    int upvalues_count = function->upvalue_count;
    ObjUpvalue** upvalues = ALLOCATE(ObjUpvalue*, upvalues_count);
    for (int i = 0; i < upvalues_count; i++) {
        upvalues[i] = NULL;
    }

    closure->function = function;
    closure->upvalues = upvalues;
    closure->upvalues_count = upvalues_count;
    
    return closure;
};

ObjUpvalue* new_upvalue(Value* slot) {
    ObjUpvalue* upv = ALLOCATE_OBJ(ObjUpvalue, OBJ_UPVALUE);
    upv->location = slot;
    upv->next = NULL;
    upv->closed = NULL_VAL;
    return upv;
}