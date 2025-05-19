#include "compiler.h"
#include "debug.h"
#include "object.h"
#include "string.h"

#define DEBUG_PRINT_CODE

#define UINT8_COUNT (UINT8_MAX + 1)
#define SWITCH_MAX_CASES 64
#define FUNCTION_MAX_PARAMS 255

typedef struct {
    int depth;
    Token name;
    bool is_captured; // has upvalue references
} Local;

typedef enum {
    FTYPE_FUNCTION,
    FTYPE_SCRIPT,
} FunctionType;

typedef struct {
    uint8_t index;
    bool is_local;
} ClosureUpvalue;


typedef struct {
    struct Compiler* enclosing;
    
    ObjFunction* function;
    FunctionType function_type;

    int local_count;
    int scope_depth;
    Local locals[UINT8_COUNT];

    ClosureUpvalue upvalues[UINT8_COUNT];
    
} Compiler;

typedef struct {
    Token current;
    Token previous;
    bool had_error;
    bool panic_mode;
} Parser;

typedef enum {
    PREC_NONE,
    PREC_ASSIGMENT, // =
    PREC_OR, // or
    PREC_AND, // and
    PREC_EQUALITY, // == !=
    PREC_COMPARISON, // < > <= >=
    PREC_TERM, // + -
    PREC_FACTOR, // * /
    PREC_UNARY, // ! -
    PREC_CALL, // .()
    PREC_PRIMARY,
} PrecedenceOrder;

typedef void (*ParseFunc)(bool canAssign);

typedef struct  {
    ParseFunc prefix;
    ParseFunc infix;
    PrecedenceOrder prec;
} ParseRule;

Parser parser;
Compiler* current_comp = NULL;
Chunk* compiling_chunk;
Hashtable string_constants;

int deepest_loop_offset = -1;
int deepest_loop_depth = -1;

void error_at(Token* token, const char* msg) {
    fprintf(stderr, "[line %d] Error", token->line);

    if (parser.panic_mode) return;
    parser.panic_mode = true;

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type == TOKEN_ERROR) {
        // nothing
    } else {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", msg);
    parser.had_error = true;
} 

void error(const char* msg) {
    error_at(&parser.previous, msg);
}

void error_at_current(const char* msg) {
    error_at(&parser.current, msg);
}

void consume(TOKEN_TYPE type, const char* msg) {
    if (parser.current.type == type) {
        advance();
        return;
    }

    error_at_current(msg);
};

bool check(TOKEN_TYPE type) {
    return parser.current.type == type;
};

bool match_token(TOKEN_TYPE type) {
    if (!check(type)) return false;
    advance();
    return true;
};

// ---- Compiler tools
// popping local variables after scope end.
void shrink_local_variables() {
    while(current_comp->local_count > 0 &&
        current_comp->locals[current_comp->local_count-1].depth >
        current_comp->scope_depth) {
            COMPILER_DEBUG_LOG("shrink_local_variables\n");
            
            if (current_comp->locals[current_comp->local_count-1].is_captured) {
                emit_byte(OP_CLOSE_UPVALUE);
            }
            else {
                emit_byte(OP_POP);
            }

            current_comp->local_count--;
        }
};

bool identifier_equal(Token* a, Token* b) {
    if (a->length != b->length) return false;
    return (strncmp(a->start, b->start, a->length) == 0);
};

// try to find local variable and return they index.
int resolve_local(Compiler* comp, Token* name) {
    for (int i = comp->local_count - 1; i >= 0; i--) {
        Local* local = &comp->locals[i];
        if (identifier_equal(name, &local->name)) {
            return i;
        }
    }

    return -1;
}

void scope_begin() {
    current_comp->scope_depth++;
};

void scope_end() {
    current_comp->scope_depth--;
    shrink_local_variables();
};

void add_local(Token name) {
    
    if (current_comp->local_count == UINT8_COUNT) {
        error("Too many local variables in function.");
        return;
    }

    Local* local = &current_comp->locals[current_comp->local_count++];
    local->name = name;
    local->depth = current_comp->scope_depth;
    local->is_captured = false;
};

// ------------ CHUNK TOOLS

Chunk* current_chunk() {
    return &current_comp->function->chunk;
};

uint8_t make_constant(Value reprValue) {
    int const_index = chunk_add_constant(current_chunk(), reprValue);

    if (const_index > UINT8_MAX) {
        error("too many constants in one chunk.");
        return 0;
    }

    return (uint8_t)const_index;
};


// ------------ EMITTING BYTECODE

void emit_byte(uint8_t byte) {
    chunk_write(current_chunk(), byte, parser.previous.line);
};

void emit_op_return() {
    emit_byte(OP_NULL);
    emit_byte(OP_RET);
};

void emit_bytes(uint8_t byte1, uint8_t byte2) {
    emit_byte(byte1);
    emit_byte(byte2);
};

void emit_constant(Value value) {
    emit_bytes(OP_CONST, make_constant(value));
}

ObjFunction* end_compiler() {
    emit_op_return();

    ObjFunction* function = current_comp->function;

    #ifdef DEBUG_PRINT_CODE
    if (!parser.had_error) {
        disassembleChunk(current_chunk(), function->name != NULL ?
            function->name->chars : "<script>");
    }
    #endif

    // clear constant cache
    //destroy_hashtable(&string_constants);

    // unlink function compiler
    // get back to previous function call compiler
    current_comp = current_comp->enclosing; 

    return function;
};

// -------------------- PARSING

void comp_number(bool canAssign) {
    double v = strtod(parser.previous.start, NULL);
    Value vv = NUMBER_VAL(v);
    emit_constant(vv);
};

void comp_string(bool canAssign) {
    emit_constant(OBJ_VAL(copy_string(
        parser.previous.start+1, parser.previous.length-2)));
};

uint8_t make_id_constant(Token* name) {

    /*
    ObjString* str = copy_string(name->start, name->length);
    Value index;
    if (hashtable_get(&string_constants, str, &index)) {
        return (uint8_t)AS_NUMBER(index);
    }
   
    uint8_t const_index = make_constant(OBJ_VAL(str));
    hashtable_set(&string_constants, str, NUMBER_VAL((double)const_index));
    
    return const_index;
    */

    return make_constant(OBJ_VAL(copy_string(name->start,
        name->length)));
};

// ----------------- JUMBS
// put jump  instr to chunk code, reserve two bytes for jump OFFSET
int emit_jump(uint8_t instruction) {
    emit_byte(instruction);
    // supports long jump
    emit_byte(0xff);
    emit_byte(0xff);

    return current_chunk()->count - 2;
};

// set jump current distance by offset 
void patch_jump(int offset) {
    // -2 for itself
    int jump = current_chunk()->count - offset - 2;
    if (jump > UINT16_MAX) {
        error("Too much code to jump over.");
    }

    current_chunk()->code[offset] = (jump >> 8) & 0xff; // set sec byte
    current_chunk()->code[offset + 1] = jump & 0xff; // set first byte
};

void emit_loop(int loopStart) {
    emit_byte(OP_LOOP);
    int offset = current_chunk()->count - loopStart + 2;
    if (offset > UINT16_MAX) error("While Loop body too large");

    emit_byte((offset >> 8) & 0xff);
    emit_byte(offset & 0xff);
};


void unary(bool canAssign);
void binary();
void grouping(bool canAssign);
void literal(bool canAssign);
void variable(bool canAssign);
void and_(bool canAssign);
void or_(bool canAssign);
void call(bool canAssign);

ParseRule rules[] = {
      [TOKEN_LEFT_PAREN]    = {grouping,    call,   PREC_CALL},
      [TOKEN_RIGHT_PAREN]   = {NULL,        NULL,   PREC_NONE},
      [TOKEN_LEFT_BRACE]    = {NULL,        NULL,   PREC_NONE},
      [TOKEN_RIGHT_BRACE]   = {NULL,        NULL,   PREC_NONE},
      [TOKEN_COMMA]         = {NULL,        NULL,   PREC_NONE},
      [TOKEN_DOT]           = {NULL,        NULL,   PREC_NONE},
      [TOKEN_MINUS]         = {unary,       binary, PREC_TERM},
      [TOKEN_PLUS]          = {NULL,        binary, PREC_TERM},
      [TOKEN_SEMICOLON]     = {NULL,        NULL,   PREC_NONE},
      [TOKEN_SLASH]         = {NULL,        binary, PREC_FACTOR},
      [TOKEN_STAR]          = {NULL,        binary, PREC_FACTOR},
      [TOKEN_BANG]          = {unary,        NULL,   PREC_NONE},
      [TOKEN_BANG_EQUAL]    = {NULL,        binary,   PREC_EQUALITY},
      [TOKEN_EQ]            = {NULL,        NULL,   PREC_NONE},
      [TOKEN_EQ_EQ]         = {NULL,        binary,   PREC_EQUALITY},
      [TOKEN_GT]            = {NULL,        binary,   PREC_COMPARISON},
      [TOKEN_GT_EQ]         = {NULL,        binary,   PREC_COMPARISON},
      [TOKEN_LESS]          = {NULL,        binary,   PREC_COMPARISON},
      [TOKEN_LESS_EQ]       = {NULL,        binary,   PREC_COMPARISON},
      [TOKEN_ID]            = {variable,        NULL,   PREC_NONE},
      [TOKEN_STRING]        = {comp_string, NULL,   PREC_NONE},
      [TOKEN_NUMBER]        = {comp_number, NULL,   PREC_NONE},
      [TOKEN_AND]           = {NULL,        and_,   PREC_AND},
      [TOKEN_CLASS]         = {NULL,        NULL,   PREC_NONE},
      [TOKEN_ELSE]          = {NULL,        NULL,   PREC_NONE},
      [TOKEN_FALSE]         = {literal,        NULL,   PREC_NONE},
      [TOKEN_FOR]           = {NULL,        NULL,   PREC_NONE},
      [TOKEN_FUN]           = {NULL,        NULL,   PREC_NONE},
      [TOKEN_IF]            = {NULL,        NULL,   PREC_NONE},
      [TOKEN_NULL]          = {literal,        NULL,   PREC_NONE},
      [TOKEN_OR]            = {NULL,        or_,    PREC_OR},
      [TOKEN_PRINT]         = {NULL,        NULL,   PREC_NONE},
      [TOKEN_RETURN]        = {NULL,        NULL,   PREC_NONE},
      [TOKEN_SUPER]         = {NULL,        NULL,   PREC_NONE},
      [TOKEN_THIS]          = {NULL,        NULL,   PREC_NONE},
      [TOKEN_TRUE]          = {literal,        NULL,   PREC_NONE},
      [TOKEN_VAR]           = {NULL,        NULL,   PREC_NONE},
      [TOKEN_WHILE]         = {NULL,        NULL,   PREC_NONE},
      [TOKEN_ERROR]         = {NULL,        NULL,   PREC_NONE},
      [TOKEN_EOF]           = {NULL,        NULL,   PREC_NONE},

};

ParseRule* get_rule(TOKEN_TYPE type) {
    return &rules[type];
};

void parse_precedence(PrecedenceOrder prec) {
    advance();

    ParseFunc prefix = get_rule(parser.previous.type)->prefix;
    if (prefix == NULL) {
        error("Expect expression (prefix).");
        return;
    }

    COMPILER_DEBUG_LOG("parse_precedence\n");

    bool can_assign = prec <= PREC_ASSIGMENT;
    prefix(can_assign);
    //printf("PREFIX='%.*s'\n", parser.previous.length, parser.previous.start);

    while(prec <= get_rule(parser.current.type)->prec) {
        COMPILER_DEBUG_LOG("parse_precedence.infix\n");
        advance();
        ParseFunc infix = get_rule(parser.previous.type)->infix;
        infix(can_assign);
        //printf("INFIX='%.*s'\n", parser.previous.length, parser.previous.start);
    }

    if (can_assign && match_token(TOKEN_EQ)) {
        error("Invalid assigment target.");
    }
};

void literal(bool canAssign) {
    switch (parser.previous.type)
    {
    case TOKEN_TRUE: emit_byte(OP_TRUE); break;
    case TOKEN_FALSE: emit_byte(OP_FALSE); break;
    case TOKEN_NULL: emit_byte(OP_NULL); break;

    default:
        return;
    }
}

void unary(bool canAssign) {
    // -a

    TOKEN_TYPE operator = parser.previous.type;
    
    // compile operand
    parse_precedence(PREC_UNARY);

    switch (operator)
    {
        case TOKEN_MINUS: emit_byte(OP_NEGATE); break;
        case TOKEN_BANG: emit_byte(OP_NOT); break;
        default: return;
    }
};

void binary() {
    TOKEN_TYPE operator = parser.previous.type;
    ParseRule* rule = get_rule(operator);
    parse_precedence((PrecedenceOrder)(rule->prec + 1));

    switch (operator)
    {
    case TOKEN_PLUS: emit_byte(OP_ADD); break;
    case TOKEN_MINUS: emit_byte(OP_SUB); break;
    case TOKEN_STAR: emit_byte(OP_MUL); break;
    case TOKEN_SLASH: emit_byte(OP_DIV); break;

    // comparison
    case TOKEN_BANG_EQUAL: emit_bytes(OP_EQUAL, OP_NOT); break;
    case TOKEN_LESS_EQ: emit_bytes(OP_GREATER, OP_NOT); break;
    case TOKEN_GT_EQ: emit_bytes(OP_LESS, OP_NOT); break;
    case TOKEN_LESS: emit_byte(OP_LESS); break;
    case TOKEN_GT: emit_byte(OP_GREATER); break;
    case TOKEN_EQ_EQ: emit_byte(OP_EQUAL); break;

    default:
        return;
    }
};

void grouping(bool canAssign) {
    // ( expr )
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
};




void expression() {
    parse_precedence(PREC_ASSIGMENT);
};



void advance() {
    parser.previous = parser.current;
    
    for (;;) {
        parser.current = scan_token();
        if (parser.current.type != TOKEN_ERROR) break;

        error_at_current(parser.current.start);
    }
}

void print_statement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
    emit_byte(OP_PRINT);
};

void expression_statement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after expression (expression_statement).");
    emit_byte(OP_POP);
};

void block() {
    while(!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        declaration();
    }

    consume(TOKEN_RIGHT_BRACE, "Expect ';' after block.");
};



void if_statement() {
    // if (expression)
    consume(TOKEN_LEFT_PAREN, "Expect '(' in if.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' in if.");

    // parse it as a block.
    consume(TOKEN_LEFT_BRACE, "Expect '{' after expr in If");

    int them_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);
    
    block();

    // если мы здесь, скипаем else ветку джампом без условия
    int else_jump = emit_jump(OP_JUMP);
    patch_jump(them_jump);
    emit_byte(OP_POP);

    if (match_token(TOKEN_ELSE)) {
        consume(TOKEN_LEFT_BRACE, "Expect '{' after expr in Else");
        block();
        patch_jump(else_jump);
    }
};

// ------------------ LOOPS

void while_statement() {
    // while () {}
    int loop_start = current_chunk()->count;

    // supports continue
    int old_loop_offset = deepest_loop_offset;
    int old_loop_depth = deepest_loop_depth;
    deepest_loop_offset = loop_start;
    deepest_loop_depth = current_comp->scope_depth;

    consume(TOKEN_LEFT_PAREN, "Expect '(' in while.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' in while.");

    int exit_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);
    consume(TOKEN_LEFT_BRACE, "Expect '{' to begin While body");
    
    block();

    emit_loop(loop_start);

    patch_jump(exit_jump);
    emit_byte(OP_POP);

    deepest_loop_depth = old_loop_depth;
    deepest_loop_offset = old_loop_offset;
};

void for_statement() {
    // for (init; cond; op) {}
    scope_begin();

    consume(TOKEN_LEFT_PAREN, "Expect '(' to begin For loop");
    // <init> section
    if (match_token(TOKEN_SEMICOLON)) {
        // no initializer
    }
    else if (match_token(TOKEN_VAR)) {
        var_decl();
    }
    else {
        expression_statement();
    }

    int loop_start = current_chunk()->count;

    // supports continue
    int old_loop_offset = deepest_loop_offset;
    int old_loop_depth = deepest_loop_depth;
    deepest_loop_offset = loop_start;
    deepest_loop_depth = current_comp->scope_depth;

    // <condition> section
    int exit_jump = -1;
    if (!match_token(TOKEN_SEMICOLON)) {
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' in <condition> For loop");

        // jump if the condition is false
        exit_jump = emit_jump(OP_JUMP_IF_FALSE);
        emit_byte(OP_POP);
    } 

    // <increment> section
    if (!match_token(TOKEN_RIGHT_PAREN)) {
        int body_jump = emit_jump(OP_JUMP);
        
        int inc_start = current_chunk()->count;
        expression();
        emit_byte(OP_POP);
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after increment section in For loop");

        emit_loop(loop_start);
        loop_start = inc_start;

        deepest_loop_offset = loop_start;

        patch_jump(body_jump);
    }

    // body
    statement();

    emit_loop(loop_start);

    if (exit_jump != -1) {
        patch_jump(exit_jump);
        emit_byte(OP_POP);
    }

    deepest_loop_depth = old_loop_depth;
    deepest_loop_offset = old_loop_offset;

    scope_end();
};

void switch_() {
    // switch (exopr)
    consume(TOKEN_LEFT_PAREN, "Expect '(' to begin Switch expr");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' to end Switch expr");
    consume(TOKEN_LEFT_BRACE, "Expect '{' to begin Switch body");

    int prev_case_jump = -1;
    int cases[SWITCH_MAX_CASES];
    int index = 0;

    // parse case
    while(match_token(TOKEN_CASE) || match_token(TOKEN_DEFAULT) && index < SWITCH_MAX_CASES) {
        // patch previous case.
        if (prev_case_jump > 0) {
            patch_jump(prev_case_jump);
            emit_byte(OP_POP);
        }

        if (parser.previous.type == TOKEN_DEFAULT) {
            consume(TOKEN_COLON, "Expect ':' to begin Switch-Case body");
    
            // case body.
            statement();
        }
        else {
            emit_byte(OP_DUP);

            expression();
    
            // if false, go next.
            emit_byte(OP_EQUAL);
            prev_case_jump =  emit_jump(OP_JUMP_IF_FALSE);

            emit_byte(OP_POP);
            
            consume(TOKEN_COLON, "Expect ':' to begin Switch-Case body");
    
            // case body.
            statement();
        }
        
        // go out of switch
        cases[index++] = emit_jump(OP_JUMP);
        //emit_byte(OP_POP);
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' to end Switch body");

    // patch all outs.
    for (int i = 0; i < index-1; i++) {
        patch_jump(cases[i]);
    }

    // patch last case.
    patch_jump(cases[index-1]);
    emit_byte(OP_POP);  
};

void continue_() {
    
    if (deepest_loop_offset == -1) {
        error("Can't use 'continue' out loop.");
    }

    consume(TOKEN_SEMICOLON, "Expect ';' after continue.");

    // shrink locals in loop
    for (int i = current_comp->local_count-1;
         i >= 0 && current_comp->locals[i].depth > deepest_loop_depth;
        i--) {
        emit_byte(OP_POP);
    }

    emit_loop(deepest_loop_offset);
};  

void return_statement() {
    if (current_comp->function_type == FTYPE_SCRIPT) {
        error("Can't return from top-level code.");
    }

    if (match_token(TOKEN_SEMICOLON)) {
        emit_op_return();
    }
    else {
        // return value
        expression();

        consume(TOKEN_SEMICOLON, "Expect ';' after return statement.");
        emit_byte(OP_RET);
    }
};

void statement() {
    if (match_token(TOKEN_PRINT)) {
        print_statement();
    } 
    else if (match_token(TOKEN_RETURN)) {
        return_statement();
    }
    else if (match_token(TOKEN_IF)) {
        if_statement();
    }
    else if (match_token(TOKEN_WHILE)) {
        while_statement();
    }
    else if (match_token(TOKEN_CONTINUE)) {
        continue_();
    }
    else if (match_token(TOKEN_SWITCH)) {
        switch_();
    }
    else if (match_token(TOKEN_FOR)) {
        for_statement();
    }
    else if (match_token(TOKEN_LEFT_BRACE)) {
        scope_begin();
        block();
        scope_end();
    }
    else {
        expression_statement();
    }
};

// sync compiler parser state when getting some error.
void compiler_sync() {
    parser.panic_mode = false;
    while(parser.current.type != TOKEN_EOF) {
        if (parser.previous.type == TOKEN_SEMICOLON) return;
        switch (parser.current.type)
        {
        case TOKEN_CLASS:
        case TOKEN_FUN:
        case TOKEN_VAR:
        case TOKEN_FOR:
        case TOKEN_IF:
        case TOKEN_WHILE:
        case TOKEN_PRINT:
        case TOKEN_RETURN:
            return;
        
        default:
        }

        advance();
    }
};

// ----------- VAR
void declare_variable() {
    // skip global decls
    if (current_comp->scope_depth == 0) return;

    Token* name = &parser.previous;

    // check var in same scope
    for (int i = current_comp->local_count -1; i >= 0; i--) {
        Local* local = &current_comp->locals[i];
        if (local->depth != -1 && local->depth < current_comp->scope_depth) {
            break;
        }

        if (identifier_equal(name, &local->name)) {
            error("Already a variable with this name in this scope.");
        }
    }

    add_local(*name);
};

uint8_t parse_variable(const char* errorMsg) {
    consume(TOKEN_ID, errorMsg);

    declare_variable();
    if (current_comp->scope_depth > 0) return 0;

    return make_id_constant(&parser.previous);
};

int add_upvalue(Compiler* compiler, uint8_t index, bool isLocal) {
    // add new upvalue for this closure.
    int upvalueCount = compiler->function->upvalue_count;

    // check this compiler already contains upvalue to this index.
    for (int i = 0; i < upvalueCount; i++) {
        ClosureUpvalue* upv = &compiler->upvalues[i];
        if (upv->index == index && upv->is_local == isLocal) {
            return i;
        }
    }

    if (upvalueCount > UINT8_COUNT) {
        error("Too many closure variables in function.");
        return 0;
    }

    // this function contain new upvalue.
    compiler->upvalues[upvalueCount].index = index;
    compiler->upvalues[upvalueCount].is_local = isLocal;

    return compiler->function->upvalue_count++;
};

int resolve_upvalue(Compiler* compiler, Token* name) {
    if (compiler->enclosing == NULL) return -1;

    int local = resolve_local(compiler->enclosing, name);
    if (local != -1) {
        // now local has a reference by upvalue.
        ((Compiler*)compiler->enclosing)->locals[local].is_captured = true;  
        //compiler->enclosing->locals[local].is_captured = true;  
        return add_upvalue(compiler, (uint8_t)local, true);
    }

    int upvalue = resolve_upvalue(compiler->enclosing, name);
    if (upvalue != -1) {
        return add_upvalue(compiler, (uint8_t)upvalue, false);
    }

    return -1;
};

void named_variable(Token token, bool canAssign) {
    uint8_t getOp, setOp;

    int arg = resolve_local(current_comp, &token);
    if (arg != -1) {
        // its local var
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
    }
    else if ((arg = resolve_upvalue(current_comp, &token)) != -1) {
        // its upvalue
        getOp = OP_GET_UPVALUE;
        setOp = OP_SET_UPVALUE;
    }
    else {
        // its global var
        arg = make_id_constant(&token);
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
    }

    if (canAssign && match_token(TOKEN_EQ)) {
        expression(); // parse arg
        emit_bytes(setOp, (uint8_t)arg);
    }
    else {
        emit_bytes(getOp, (uint8_t)arg);
    }
} 

void variable(bool canAssign) {
    named_variable(parser.previous, canAssign);
}

void markInitialized() {
    //> Calls and Functions check-depth
    if (current_comp->scope_depth == 0) return;
    //< Calls and Functions check-depth
    current_comp->locals[current_comp->local_count - 1].depth = current_comp->scope_depth;
}

void define_variable(uint8_t global) {

    // pass code for local vars
    if (current_comp->scope_depth > 0) {
        markInitialized();
        return;
    }

    emit_bytes(OP_DEFINE_GLOBAL, global);
}

void var_decl() {
    uint8_t global = parse_variable("Expect a variable name.");
    if (match_token(TOKEN_EQ)) {
        // var foo = ...;
        COMPILER_DEBUG_LOG("var_decl.expr()\n");
        expression();
    } else {
        // var foo;
        COMPILER_DEBUG_LOG("var_decl.null\n");
        emit_byte(OP_NULL); // default value for variable
    }

    consume(TOKEN_SEMICOLON, "Expect ';' after var_decl.");
    define_variable(global);
};

// ------------ FUNCTION
void function_impl(FunctionType type) {
    Compiler compiler;
    compiler_init(&compiler, type);
    
    scope_begin();
    consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");

    // compile params
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            current_comp->function->arity++;
            if (current_comp->function->arity > FUNCTION_MAX_PARAMS) {
                error_at_current("Can't have more than 255 params in function.");
            }

            uint8_t constant = parse_variable("Expect a parameter name.");
            define_variable(constant); // define variable in CURRENT function
        } while(match_token(TOKEN_COMMA));
    }


    consume(TOKEN_RIGHT_PAREN, "Expect ')' after function name.");
    consume(TOKEN_LEFT_BRACE, "Expect '{' to begin function body.");
    
    block();

    ObjFunction* function = end_compiler();

    if (function->upvalue_count == 0) {
        emit_bytes(OP_CONST, make_constant(OBJ_VAL(function)));
    }
    else {
        emit_bytes(OP_CLOSURE, make_constant(OBJ_VAL(function)));
    
        // put upvalues on stack.
        for(int i = 0; i < function->upvalue_count; i++) {
            emit_byte(compiler.upvalues[i].is_local ? 1 : 0);
            emit_byte(compiler.upvalues[i].index);
        }
    }
}

void function_decl() {
    uint8_t global=  parse_variable("Expect function name.");
    markInitialized();
    function_impl(FTYPE_FUNCTION);
    define_variable(global);
};

void declaration() {

    COMPILER_DEBUG_LOG("declaration\n");
    
    if (match_token(TOKEN_VAR)) {
        var_decl();
    }
    else if (match_token(TOKEN_FUN)) {
        function_decl();
    }
    else {
        statement();
    }

    if (parser.panic_mode) compiler_sync();
};

int argument_list() {
    int arg_count = 0;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            // args will be on function stack
            expression();
            arg_count++;

            if (arg_count > FUNCTION_MAX_PARAMS) {
                error("Cant have more than 255 args.");
            }

        } while(match_token(TOKEN_COMMA));
    }

    consume(TOKEN_RIGHT_PAREN, "Expect ')' after args in function.");
    return arg_count;
};

void call(bool canAssign) {
    uint8_t arg_count = argument_list();
    emit_bytes(OP_CALL, arg_count);
};

// ------------ LOGICAL
void and_(bool canAssign) {
    // skip rhs when left is false
    int jump = emit_jump(OP_JUMP_IF_FALSE);
    
    emit_byte(OP_POP);
    parse_precedence(PREC_AND);
    
    patch_jump(jump);
};

void or_(bool canAssign) {
    int else_jump = emit_jump(OP_JUMP_IF_FALSE);
    // if we are here, lhs is True, no need to check rhs. jump over it.
    int end_jump = emit_jump(OP_JUMP);

    patch_jump(else_jump);
    emit_byte(OP_POP);

    parse_precedence(PREC_OR);
    patch_jump(end_jump);
};





ObjFunction* compile(const char* source) {
    scanner_init(source);

    Compiler comp;
    compiler_init(&comp, FTYPE_SCRIPT);

    // FIX
    //compiling_chunk = chunk;
    parser.had_error = false;
    parser.panic_mode = false;

    advance();
    
    while(!match_token(TOKEN_EOF)) {
        declaration();
    }

    

    ObjFunction* function = end_compiler();
    return parser.had_error ? NULL : function;
};

void dump_pass() {
    int line = -1;
    for (;;) {
        Token tok = scan_token();
        if (tok.line != line) {
            printf("%4d ", tok.line);
            line = tok.line;
        }
        else {
            printf("    | ");
        }

        printf("%2d '%.*s'\n", tok.type, tok.length, tok.start);
        if (tok.type == TOKEN_EOF) break;
    }
}

void compiler_init(Compiler* comp, FunctionType type) {

    // link previous function compiler.
    comp->enclosing = current_comp;

    COMPILER_DEBUG_LOG("compiler_init\n");
    comp->local_count = 0;
    comp->scope_depth = 0;
    comp->function = new_function();
    comp->function_type = type;
    current_comp = comp;

    if (type != FTYPE_SCRIPT) {
        // copy function name
        current_comp->function->name = copy_string(parser.previous.start, 
            parser.previous.length);
    }

    // init constant cache
    //hashtable_init(&string_constants);

    // INIT ONE LOCAL
    Local* local = &current_comp->locals[current_comp->local_count++];
    local->depth = 0;
    local->name.start = "";
    local->name.length = 0;
    local->is_captured = false;
};

void gc_mark_compiler_roots() {
    Compiler* c = current_comp;
    while(c != NULL) {
        mark_object((Obj*)c->function);
        c = c->enclosing;
    }
}