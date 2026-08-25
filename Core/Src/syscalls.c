/**
 * @file    syscalls.c
 * @brief   Minimal newlib syscall stubs for a bare-metal build (no OS files,
 *          no semihosting). FreeRTOS uses heap_4 for its allocations; _sbrk is
 *          provided only for any stray newlib malloc.
 */
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>

extern int errno;

/* Provided by the linker script. */
extern char _end;              /* end of .bss / start of heap */
extern char _estack;

register char *stack_ptr asm("sp");

void *_sbrk(ptrdiff_t incr)
{
    static char *heap_end;
    char *prev;

    if (heap_end == 0) {
        heap_end = &_end;
    }
    prev = heap_end;
    if (heap_end + incr > stack_ptr) {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap_end += incr;
    return (void *)prev;
}

int _write(int file, char *ptr, int len)
{
    (void)file;
    (void)ptr;
    return len;   /* discard */
}

int _read(int file, char *ptr, int len)
{
    (void)file;
    (void)ptr;
    (void)len;
    return 0;
}

int _close(int file)         { (void)file; return -1; }
int _fstat(int file, struct stat *st) { (void)file; st->st_mode = S_IFCHR; return 0; }
int _isatty(int file)        { (void)file; return 1; }
int _lseek(int file, int ptr, int dir) { (void)file; (void)ptr; (void)dir; return 0; }
int _getpid(void)            { return 1; }
int _kill(int pid, int sig)  { (void)pid; (void)sig; errno = EINVAL; return -1; }
void _exit(int status)       { (void)status; for (;;) {} }
