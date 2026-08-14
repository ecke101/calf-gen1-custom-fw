#ifndef NGCD_TARGET_SETJMP_H
#define NGCD_TARGET_SETJMP_H

/* glibc 2.33 AArch64 ABI, matching the target firmware libc. */
typedef unsigned long int __ngcd_jmp_registers[22];
typedef struct {
    unsigned long int values[16];
} __ngcd_sigset;

struct __ngcd_jmp_buffer {
    __ngcd_jmp_registers registers;
    int mask_was_saved;
    __ngcd_sigset saved_mask;
};

typedef struct __ngcd_jmp_buffer jmp_buf[1];

extern int setjmp(jmp_buf environment);
extern void longjmp(jmp_buf environment, int value)
    __attribute__((noreturn));

#endif
