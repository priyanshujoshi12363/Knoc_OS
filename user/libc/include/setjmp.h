#ifndef _KNOC_SETJMP_H
#define _KNOC_SETJMP_H

typedef long jmp_buf[26];

int setjmp(jmp_buf buffer);
void longjmp(jmp_buf buffer, int value) __attribute__((noreturn));

#endif
