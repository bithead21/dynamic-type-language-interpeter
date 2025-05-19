#include "memory.h"

extern VM vm;

void* realloc_ptr(void* old_ptr, size_t old_size, size_t new_size) {
    
    if (new_size > old_size) {
        #ifdef DEBUG_STRESS_GC
        collect_garbage();
        #endif
    }

    if (old_ptr == NULL) {
        exit(1);
        return NULL;
    }

    if (new_size == 0) {
        free(old_ptr);
        return NULL;
    }

    void* new_ptr = realloc(old_ptr, new_size);
    if (new_ptr == NULL) {
        exit(1);
        return NULL;
    }

    return new_ptr;
};


void mark_array(ValueArray* arr) {
    for (int i = 0; i < arr->count; i++) {
        mark_value(arr->values[i]);
    }
}

void blacken_object(Obj* object) {
    
    #ifdef DEBUG_LOG_GC
    printf("[gc] blacken %p\n", object);
    print_value(OBJ_VAL(object));
    printf("\n");
    #endif
    
    switch (object->type)
    {
    case OBJ_NATIVE:
    case OBJ_STRING:
        break;

    case OBJ_UPVALUE:
        mark_value(((ObjUpvalue*)object)->closed);
        break;

    case OBJ_FUNCTION:
        ObjFunction* fn = (ObjFunction*)object;
        mark_object((Obj*)fn->name);
        mark_array(&fn->chunk.constants);
        break;
        
    case OBJ_CLOSURE:
        ObjClosure* clos = (ObjClosure*)object;
        mark_object((Obj*)clos->function);
        for (int i = 0; i < clos->upvalues_count; i++) {
            mark_object((Obj*)clos->upvalues[i]);
        }
        break;

    default:
        break;
    }
}

void trace_refs() {
    while(vm.gray_count >0) {
        Obj* obj = vm.gray_stack[--vm.gray_count];
        blacken_object(obj);
    }
};

void mark_object(Obj* object) {
    if (object == NULL) return;
    if (object->is_gc_marked) return;

    object->is_gc_marked = true;

    if (vm.gray_capacity < vm.gray_count + 1) {
        vm.gray_capacity = GROW_CAPACITY(vm.gray_capacity);
        vm.gray_stack = (Obj**)realloc(vm.gray_stack, sizeof(Obj*) * vm.gray_capacity);
    }

    if (vm.gray_stack == NULL) exit(1);

    vm.gray_stack[vm.gray_count++] = object;

    #ifdef DEBUG_LOG_GC
    printf("[gc] mark %p\n", object);
    print_object(OBJ_VAL(object));
    printf("\n");
    #endif

}

void mark_value(Value v) {
    if (IS_OBJ(v)) mark_object(AS_OBJ(v));
}

void mark_table(Hashtable* table) {
    for (int i = 0; i< table->capacity; i++) {
        Entry* entry = &table->entries[i];
        mark_object((Obj*)entry->key);
        mark_value(entry->value);
    }
}

void mark_roots() {
    for (Value* slot = vm.stack; slot < vm.stack_top; slot++) {
        mark_value(*slot);
    }

    // upvalues still reach
    for (ObjUpvalue* upv = vm.open_upvalues; upv != NULL; upv=upv->next) {
        mark_object((Obj*)upv);
    }

    // marking closures on stack frame
    for (int i = 0; i < vm.frames_count; i++) {
        mark_object((Obj*)vm.frames[i].closure);
    };

    // mark compiler self oobjects
    gc_mark_compiler_roots();

    // marking global values
    mark_table(&vm.globals);
};



void sweep() {
    Obj* prev= NULL;
    Obj* object =vm.objects;
    while(object != NULL) {
        if (object->is_gc_marked) {
            object->is_gc_marked = false;
            prev= object;
            object = object->next;
        }
        else {
            Obj* unreached = object;
            object = object->next;
            if (prev != NULL) {
                prev->next = object;
            }
            else {
                vm.objects = object;
            }

            freeObj(unreached);
        }
    }
};

void collect_garbage() {
    #ifdef DEBUG_LOG_GC
    printf("--- gc begin\n");
    #endif

    mark_roots();
    gc_table_remove_white(&vm.strings);
    trace_refs();

    #ifdef DEBUG_LOG_GC
    printf("--- gc end\n");
    #endif
}
