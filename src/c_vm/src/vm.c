#include "vm.h"
#include "compiler.h"
#include "stdarg.h"
#include "object.h"

#include "builtin_natives/clock.h"
#include "builtin_natives/math.h"

VM vm;

Value stack_peek(int distance) {
    return vm.stack_top[-1 - distance];
};

ObjFunction* get_frame_function(CallFrame* frame) {
    if (frame->function->type == OBJ_FUNCTION) {
        return (ObjFunction*)frame->function;
    } 
    else if (frame->function->type == OBJ_CLOSURE) {
        return ((ObjClosure*)frame->function)->function;
    }
};

void runtime_error(const char* format, ...) {
    printf("\n");
    fprintf(stderr, "--------- Runtime error ---------\n");
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    // stack trace
    for (int i = vm.frames_count - 1; i >= 0; i--) {
        CallFrame* frame = &vm.frames[i];
        ObjFunction* func = get_frame_function(frame);
        size_t instr = frame->ip - func->chunk.code - 1;
        int line = func->chunk.lines[instr];
        ObjFunction* fn = func;
        fprintf(stderr, "[line %d] in %s\n", line, (fn->name != NULL ? fn->name->chars : "script"));
    }

    vm_reset_stack();
};

void define_native(const char* name, NativeFn function) {
    vm_stack_push(OBJ_VAL(copy_string(name, (int)strlen(name))));
    vm_stack_push(OBJ_VAL(new_native(function)));
    hashtable_set(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);
    vm_stack_pop();
    vm_stack_pop();
};

// ------------ TOOLS
bool bool_is_falsey(Value v) {
    return IS_NULL(v) || (IS_BOOL(v) && !AS_BOOL(v));
};

bool strings_equal(ObjString* a, ObjString* b) {
    if (a->length == b->length) {
        return memcmp(a->chars, b->chars, a->length) == 0;
    }

    return false;
}

bool valuesEqual(Value a, Value b) {
    if (a.type != b.type) {
        return false;
    }

    switch (a.type)
    {
    case VALUE_BOOL: return AS_BOOL(a) == AS_BOOL(b);
    case VALUE_NULL: return true;
    case VALUE_NUMBER: return AS_NUMBER(a) == AS_NUMBER(b);
    case VALUE_OBJ: return AS_OBJ(a) == AS_OBJ(b); // cmp by ptr
    
    default:
        return false;
    }
};

ObjString* strings_concat() {
    ObjString* b = AS_STRING(vm_stack_pop());
    ObjString* a = AS_STRING(vm_stack_pop());
    int len = a->length + b->length;
    char* chars = ALLOCATE(char, len + 1);
    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[len] = '\0';

    ObjString* str = new_string(chars, len);
    return str;
};

// call closure
// call func


static bool vm_call_func(Obj* callee, ObjFunction* function, int argCount) {
  if (argCount != function->arity) {
    runtime_error("Expected %d arguments but got %d.",
        function->arity, argCount);
    return false;
  }

  if (vm.frames_count == VM_FRAMES_MAX) {
    runtime_error("Stack overflow.");
    return false;
  }

  CallFrame* frame = &vm.frames[vm.frames_count++];
  frame->function = (Obj*)callee;
  frame->ip = function->chunk.code;
  frame->closure = NULL;
  frame->slots = vm.stack_top - argCount - 1;
  return true;
}

static bool call_closure(ObjClosure* closure, int argCount) {
  return vm_call_func((Obj*)closure, closure->function, argCount);
}

static bool call_function(ObjFunction* function, int argCount) {
  return vm_call_func((Obj*)function, function, argCount);
}

bool call_value(Value callee, int argCount) {
    
    // check call type
    if (IS_OBJ(callee)) {
        switch ((OBJ_TYPE(callee)))
        {
        case OBJ_FUNCTION:
            return call_function(AS_FUNCTION(callee), argCount);

        case OBJ_CLOSURE:
            return call_closure(AS_CLOSURE(callee), argCount);

        case OBJ_NATIVE:
            NativeFn native = AS_NATIVE(callee);
            
            bool success;
            Value result = native(argCount, vm.stack_top - argCount, &success);
            if (!success) {
                runtime_error(AS_STRING(vm.stack_top[-argCount - 1])->chars);
                return false;
            }
            else {
                // we no need t\o work with frame. no bytecode by native. 
                //Value result = native(argCount, vm.stack_top - argCount);
                vm.stack_top -= argCount + 1;
                vm_stack_push(result);
            }

            return true;
            
        default:
            break; // non collable type.
        }
    }

    runtime_error("Can only call functions and classes.");
    return false;
};

ObjUpvalue* capture_upvalue(Value* local) {
    // lookup for this upvalue.
    ObjUpvalue* prev= NULL;
    ObjUpvalue* upv = vm.open_upvalues;
    while(upv !=  NULL && upv->location > local) {
        prev = upv;
        upv = upv->next;
    }

    if (upv != NULL && upv->location == local) {
        return upv;
    }

    ObjUpvalue* new_upv = new_upvalue(local);
    new_upv->next = upv;

    if (prev == NULL) {
        vm.open_upvalues = new_upv;
    } else {
        prev->next = new_upv;
    }

    return new_upv;
};

void close_copy_upvalues(Value* last) {
    while(vm.open_upvalues != NULL && vm.open_upvalues->location >= last) {
        ObjUpvalue* upv = vm.open_upvalues;
        upv->closed = *upv->location;
        upv->location = &upv->closed;
        vm.open_upvalues  = upv->next;
    }
}

INTERPRET_RESULT run() {
    CallFrame* frame = &vm.frames[vm.frames_count-1];

    register uint8_t* ip = frame->ip;

    #define READ_SHORT() \
        (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))

    #define READ_BYTE() (*ip++)
    #define READ_CONSTANT() (get_frame_function(frame)->chunk.constants.values[READ_BYTE()])
    #define READ_STRING() AS_STRING(READ_CONSTANT())
    #define BINARY_OP(valueType, operation) \
        do {\
            if (IS_STRING(stack_peek(0)) && IS_STRING(stack_peek(1))) { \
                frame->ip = ip; \
                vm_stack_push(OBJ_VAL(strings_concat())); \
                break; \
            } \
            \
            if (!IS_NUMBER(stack_peek(0)) || !IS_NUMBER(stack_peek(1))) { \
                frame->ip = ip; \
                runtime_error("Operands must be numbers in BinaryOp."); \
                return INTERPRET_RUNTIME_ERROR; \
            } \
            double b = AS_NUMBER(vm_stack_pop()); \
            double a = AS_NUMBER(vm_stack_pop()); \
            vm_stack_push(valueType(a operation b)); \
        } while (false) \

    printf("-------- Runtime log ---------\n");

    for (;;) {

        #ifdef DEBUG_TRACE_EXECUTION

        // dump stack each instruction
        printf("       ");
        for (Value* slot = vm.stack; slot < vm.stack_top; slot++) {
            printf("[");
            print_value(*slot);
            printf(" ]");
        }
        printf("\n");

        ObjFunction* fn = get_frame_function(frame);
        disassembleInstruction(&fn->chunk, (int)(ip - fn->chunk.code));
        #endif

        uint8_t code; 
        switch (code = READ_BYTE())
        {
        case OP_RET:
            //printf("_______________OP RET, frame=%i, frame=%p func=%p frameCount=%i\n",  *(frame->ip), frame, frame->function, vm.frames_count);

            // get return value.
            // clear stack args
            // push returned value on top of the stack.
            Value ret_value = vm_stack_pop();
            close_copy_upvalues(frame->slots);
            vm.frames_count--;

            if (vm.frames_count == 0) {
                vm_stack_pop();
                return INTERPRET_OK;
            }

            // slots refer on returned function begin.
            vm.stack_top = frame->slots;
            vm_stack_push(ret_value);

            // set frame to previous.
            frame = &vm.frames[vm.frames_count - 1];
            ip = frame->ip;
            break;

        case OP_NEGATE: 
            // vm_stack_push(-vm_stack_pop()); break;
            if (!IS_NUMBER(stack_peek(0))) {
                frame->ip = ip;
                runtime_error("Operand must be a number.");
                return INTERPRET_RUNTIME_ERROR;
            }

            vm_stack_push(NUMBER_VAL(-AS_NUMBER(vm_stack_pop())));
            break;

        case OP_ADD: BINARY_OP(NUMBER_VAL, +); break;
        case OP_SUB: BINARY_OP(NUMBER_VAL, -); break;
        case OP_MUL: BINARY_OP(NUMBER_VAL, *); break;
        case OP_DIV: BINARY_OP(NUMBER_VAL, /); break;

        case OP_CONST:
            Value constant = READ_CONSTANT();
            vm_stack_push(constant);
            break; 

        case OP_NULL: vm_stack_push(NULL_VAL); break;
        case OP_TRUE: vm_stack_push(BOOL_VAl(true)); break;
        case OP_FALSE: vm_stack_push(BOOL_VAl(false)); break;

        case OP_NOT: 
            vm_stack_push(BOOL_VAl(bool_is_falsey(vm_stack_pop()))); 
            break;

        case OP_EQUAL:
            Value a = vm_stack_pop();
            Value b = vm_stack_pop();
            vm_stack_push(BOOL_VAl(valuesEqual(a, b)));
            break;

        case OP_LESS: BINARY_OP(BOOL_VAl, <); break;
        case OP_GREATER: BINARY_OP(BOOL_VAl, >); break;

        // statements
        case OP_PRINT:
            print_value(vm_stack_pop());
            printf("\n");
            break;

        case OP_DEFINE_GLOBAL:
            //printf(":OP_DEFINE_GLOBAL\n");
            ObjString* name = READ_STRING();
            hashtable_set(&vm.globals, name, stack_peek(0));
            vm_stack_pop();
            break;

        case OP_SET_GLOBAL:
            ObjString* glob_name = READ_STRING();
            if (hashtable_set(&vm.globals, glob_name, stack_peek(0))) {
                frame->ip = ip;
                hashtable_delete(&vm.globals, glob_name); // [delete]
                runtime_error("Undefined variable '%s'.", glob_name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            break;

        case OP_GET_GLOBAL:
            ObjString* get_glob_name = READ_STRING();
            Value value;
            if (!hashtable_get(&vm.globals, get_glob_name, &value)) {
                frame->ip = ip;
                runtime_error("Undefined variable '%s'.", get_glob_name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            
            vm_stack_push(value);
            break;

        case OP_POP: vm_stack_pop(); break;

        case OP_SET_LOCAL:
            uint8_t set_slot = READ_BYTE();
            frame->slots[set_slot] = stack_peek(0);
            break;

        case OP_GET_LOCAL:
            uint8_t get_slot = READ_BYTE();
            vm_stack_push(frame->slots[get_slot]);
            break;

        case OP_JUMP_IF_FALSE:
            uint16_t offset_if_false = READ_SHORT();
            if (bool_is_falsey(stack_peek(0))) ip += offset_if_false;
            break;

        case OP_JUMP:
            uint8_t offset_jump = READ_SHORT();
            ip += offset_jump;
            break;

        case OP_LOOP: 
            uint16_t offset_loop = READ_SHORT();
            // go back to loop condition
            ip -= offset_loop;
            break;

        case OP_DUP: vm_stack_push(stack_peek(0)); break;

        case OP_CALL:
            // stack now: ..., fn, args.., argCount, OP_CALL
            int arg_count = READ_BYTE();
            
            frame->ip = ip;

            if (!call_value(stack_peek(arg_count), arg_count)) {
                return INTERPRET_RUNTIME_ERROR;
            }

            // update frame on new function.
            frame = &vm.frames[vm.frames_count - 1];
            ip = frame->ip;
            break;


        case OP_GET_UPVALUE:
            uint8_t slot = READ_BYTE();
            
            vm_stack_push(*((ObjClosure*)frame->function)->upvalues[slot]->location);
            break;

        case OP_SET_UPVALUE:
            uint8_t slot_set = READ_BYTE();
            *((ObjClosure*)frame->function)->upvalues[slot]->location = stack_peek(0);
            break;

        case OP_CLOSE_UPVALUE:
            close_copy_upvalues(vm.stack_top-1);
            vm_stack_pop();
            break;

        case OP_CLOSURE:
            ObjFunction* func = AS_FUNCTION(READ_CONSTANT());
            ObjClosure* closure = new_closure(func);
            vm_stack_push(OBJ_VAL(closure));

            // copy all upvalues from parent closure.
            for (int i = 0; i < closure->upvalues_count; i++) {
                uint8_t upv_local  = READ_BYTE();
                uint8_t upv_index = READ_BYTE();
                if (upv_local) {
                    // take from local
                    closure->upvalues[i] = capture_upvalue(frame->slots + upv_index);
                } else {
                    // take from parent..
                    closure->upvalues[i] = ((ObjClosure*)frame->function)->upvalues[upv_index];
                }
            }

            break;

        default:
            break;
        }
    }

    #undef READ_STRING
    #undef READ_BYTE
    #undef READ_CONSTANT
    #undef READ_SHORT
};

INTERPRET_RESULT vm_interpret_source(const char* source) {
    ObjFunction* function = compile(source);
    if (function  == NULL) {
        return INTERPRET_COMPILE_ERROR;
    }

    // set MAIN function at top frame to run 
    vm_stack_push(OBJ_VAL(function));
    ObjClosure* closure = new_closure(function);
    vm_stack_pop();
    vm_stack_push(OBJ_VAL(closure));

    // set initialy function.
    call_closure(closure, 0);

    return run();
};

/*
INTERPRET_RESULT vm_interpret(Chunk* t) {
    vm.chunk = t;
    vm.instr_ptr = vm.chunk->code; // ptr to first instruction 
    return run();
};
*/

// --------- STACK

void vm_reset_stack() {
    vm.stack_top = vm.stack;
    vm.frames_count = 0;
    vm.open_upvalues = NULL;
};

void vm_stack_push(Value v) {
    *vm.stack_top = v;
    vm.stack_top++;
};

Value vm_stack_pop() {
    vm.stack_top--;
    return *vm.stack_top;
};

// --------- VM

void vm_add_natives() {
    define_native("clock", _clock);
    define_native("max", _max);
    define_native("min", _min);
};

void vm_init() {
    vm_reset_stack();
    vm.objects = NULL;
    hashtable_init(&vm.strings);
    hashtable_init(&vm.globals);

    // add globals
    vm_add_natives();

    vm.gray_count = 0;
    vm.gray_capacity = 0;
    vm.gray_stack = NULL;
};


void vm_destroy() {
    Obj* t = vm.objects;
    while (t != NULL) {
        Obj* next = t->next;
        freeObj(t);
        t = next;
    }

    destroy_hashtable(&vm.strings);
    destroy_hashtable(&vm.globals);
    //FREE(Hashtable, vm.strings);
    free(vm.gray_stack);
    
};
