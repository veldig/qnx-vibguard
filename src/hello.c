#include <stdio.h>
#include <sys/utsname.h>

int main(void) {
    struct utsname u;
    if (uname(&u) == 0) {
        printf("Hello from QNX! %s %s %s (%s)\n", u.sysname, u.release, u.version, u.machine);
    } else {
        printf("Hello from QNX!\n");
    }
    return 0;
}
