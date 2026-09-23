#ifndef CRASH_LABELS_H
#define CRASH_LABELS_H

/* True causes of the crashes made on purpose for the training data
   (collect mode). Shared by the kernel, the AI space and user programs. */

#define LABEL_NONE 0
#define LABEL_NULL_POINTER 1
#define LABEL_BAD_POINTER 2
#define LABEL_KERNEL_ACCESS 3
#define LABEL_STACK_OVERFLOW 4
#define LABEL_WRITE_READONLY 5
#define LABEL_ILLEGAL_INSTRUCTION 6
#define LABEL_BAD_JUMP 7
#define LABEL_SUSPICIOUS 8
#define LABEL_DRIVER_BUG 9
#define LABEL_KERNEL_POINTER 10
#define LABEL_KERNEL_PANIC 11
#define LABEL_FREEZE 12
#define LABEL_CODE_CORRUPTION 13
#define LABEL_COUNT 14

#define CRASH_LABEL_NAMES                                                     \
    {"NONE", "NULL_POINTER", "BAD_POINTER", "KERNEL_ACCESS", "STACK_OVERFLOW", \
     "WRITE_READONLY", "ILLEGAL_INSTRUCTION", "BAD_JUMP", "SUSPICIOUS",        \
     "DRIVER_BUG", "KERNEL_POINTER", "KERNEL_PANIC", "FREEZE", "CODE_CORRUPTION"}

#endif
