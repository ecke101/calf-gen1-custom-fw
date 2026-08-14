extern int main(int argc, char **argv);
extern void _exit(int status) __attribute__((noreturn));

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile(
        "mov x29, xzr\n"
        "mov x30, xzr\n"
        "ldr x0, [sp]\n"
        "add x1, sp, #8\n"
        "bl main\n"
        "bl _exit\n");
}
