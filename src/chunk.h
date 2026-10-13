/* EXBIN internal: bytecode definition. */
#ifndef EXBIN_CHUNK_H
#define EXBIN_CHUNK_H

#include "value.h"

/* Operand kinds. U16 operands are big-endian. */
typedef enum {
  OPK_NONE,    /* no operand */
  OPK_U8,      /* 1 byte (local/upvalue slot, argc) */
  OPK_U16,     /* 2 bytes (element count) */
  OPK_CONST,   /* u16 constant index */
  OPK_NAME,    /* u16 constant index, must be a string */
  OPK_JUMP,    /* u16 forward offset (from the end of the instruction) */
  OPK_LOOP,    /* u16 backward offset */
  OPK_INVOKE,  /* u16 name constant + u8 argc */
  OPK_CLOSURE  /* u16 function constant + (u8 isLocal, u8 index) per upvalue */
} OpKind;

/* name, operand kind */
#define OPCODE_LIST(X)                                                        \
  X(CONSTANT, OPK_CONST)                                                      \
  X(NIL, OPK_NONE) X(TRUE, OPK_NONE) X(FALSE, OPK_NONE)                       \
  X(POP, OPK_NONE) X(DUP, OPK_NONE) X(DUP2, OPK_NONE)                         \
  X(GET_LOCAL, OPK_U8) X(SET_LOCAL, OPK_U8)                                   \
  X(GET_GLOBAL, OPK_NAME) X(DEFINE_GLOBAL, OPK_NAME) X(SET_GLOBAL, OPK_NAME)  \
  X(GET_UPVALUE, OPK_U8) X(SET_UPVALUE, OPK_U8) X(CLOSE_UPVALUE, OPK_NONE)    \
  X(GET_INDEX, OPK_NONE) X(SET_INDEX, OPK_NONE)                               \
  X(GET_FIELD, OPK_NAME) X(SET_FIELD, OPK_NAME)                               \
  X(EQUAL, OPK_NONE) X(NOT_EQUAL, OPK_NONE)                                   \
  X(LESS, OPK_NONE) X(LESS_EQUAL, OPK_NONE)                                   \
  X(GREATER, OPK_NONE) X(GREATER_EQUAL, OPK_NONE)                             \
  X(ADD, OPK_NONE) X(SUB, OPK_NONE) X(MUL, OPK_NONE) X(DIV, OPK_NONE)         \
  X(MOD, OPK_NONE) X(POW, OPK_NONE) X(NEG, OPK_NONE) X(NOT, OPK_NONE)         \
  X(BAND, OPK_NONE) X(BOR, OPK_NONE) X(BXOR, OPK_NONE) X(BNOT, OPK_NONE)      \
  X(SHL, OPK_NONE) X(SHR, OPK_NONE)                                           \
  X(JUMP, OPK_JUMP) X(JUMP_IF_FALSE, OPK_JUMP) X(JUMP_IF_TRUE, OPK_JUMP)      \
  X(LOOP, OPK_LOOP)                                                           \
  X(CALL, OPK_U8) X(INVOKE, OPK_INVOKE) X(RETURN, OPK_NONE)                   \
  X(CLOSURE, OPK_CLOSURE)                                                     \
  X(BUILD_LIST, OPK_U16) X(BUILD_MAP, OPK_U16)                                \
  X(ITER_INIT, OPK_NONE) X(FOR_NEXT, OPK_JUMP)

typedef enum {
#define X(n, k) OP_##n,
  OPCODE_LIST(X)
#undef X
      OP__COUNT
} OpCode;

const char* op_name(int op);
OpKind op_kind(int op);
int op_from_name(const char* name); /* -1 if unknown (case-insensitive) */

/* Length in bytes of the instruction at code[offset] (-1 if malformed). */
int op_length(const Chunk* c, int offset);

/* Static verification of a function (recursively): valid opcodes, operands in
 * range, jumps landing on instruction boundaries, and a full stack-depth
 * dataflow analysis (no underflow, consistent depth at every merge point,
 * local slots in range). Also computes fn->maxStack. Verified code cannot
 * corrupt the VM stack. */
bool chunk_verify(ObjFunction* fn, char* err, size_t errsz);

/* Human readable listing (recursive over nested functions). */
void chunk_disassemble(const ObjFunction* fn, Buf* out);

int chunk_line(const Chunk* c, int offset);

#endif
