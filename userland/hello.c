/* SPDX-License-Identifier: BSD-2-Clause */
/* hello.c - try the compiler inside the guest:
 *   tcc -run hello.c            compile to memory and run
 *   tcc -o hello hello.c && ./hello
 */
#include <stdio.h>
#include <sys/utsname.h>

int main(void)
{
    struct utsname u;
    uname(&u);
    printf("hello from a program compiled inside %s %s by %s\n",
           u.sysname, u.release, __TINYC__ ? "tinycc" : "?");
    return 0;
}
