#ifndef CVM_VALUES_H
#define CVM_VALUES_H

#include "common.h"
#include "memory.h"
#include "object.h"


typedef enum  ValueType {
    VALUE_BOOL,
    VALUE_NUMBER,
    VALUE_NULL,
    VALUE_OBJ,
} ValueType;

//typedef struct Obj Obj;
typedef struct ObjString ObjString;

typedef struct Value {
    ValueType type;
    union {
        double number;
        bool boolean;
        struct Obj* obj;
    } as;
} Value;

typedef struct ValueArray {
    int count;
    int capacity;
    Value* values;
} ValueArray;

#define IS_BOOL(value) ((value).type == VALUE_BOOL)
#define IS_NUMBER(value) ((value).type == VALUE_NUMBER)
#define IS_NULL(value) ((value).type == VALUE_NULL)
#define IS_OBJ(value) ((value).type == VALUE_OBJ)
#define IS_FUNCTION(value) is_obj_type(value, OBJ_FUNCTION)
#define IS_NATIVE(value) is_obj_type(value, OBJ_NATIVE)
#define IS_CLOSURE(value) is_obj_type(value, OBJ_CLOSURE)

#define BOOL_VAl(value) ((Value){VALUE_BOOL, {.boolean = value}}) 
#define NUMBER_VAL(value) ((Value){VALUE_NUMBER, {.number = value}}) 
#define NULL_VAL ((Value){VALUE_NULL, {.number = 0}}) 
#define OBJ_VAL(object) ((Value){VALUE_OBJ, {.obj = (Obj*)object}}) 

#define AS_BOOL(value) ((value).as.boolean)
#define AS_NUMBER(value) ((value).as.number)
#define AS_OBJ(value) ((value).as.obj)
#define AS_FUNCTION(value) ((ObjFunction*)AS_OBJ(value))
#define AS_NATIVE(value) (((ObjNative*)AS_OBJ(value))->function)
#define AS_CLOSURE(value) ((ObjClosure*)AS_OBJ(value))

ValueArray* valueArray_alloc();

void valueArray_init(ValueArray* t, int array_length);

void valueArray_write(ValueArray* t, Value value);

void valueArray_destroy(ValueArray* t);

#endif