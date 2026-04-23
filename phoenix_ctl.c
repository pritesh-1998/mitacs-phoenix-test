#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>

#define DEVICE "/dev/phoenix_ctl"

#define PHX_MAGIC 'P'
#define IOCTL_SET_MODE    _IOW(PHX_MAGIC, 1, int)
#define IOCTL_SET_SYSCALL _IOW(PHX_MAGIC, 2, int)
#define IOCTL_SET_PID     _IOW(PHX_MAGIC, 3, int)

#define MODE_OFF   0
#define MODE_LOG   1
#define MODE_BLOCK 2

#define SYSCALL_NONE  0
#define SYSCALL_WRITE 1

int main(int argc, char *argv[])
{
    int fd;
    int mode = -1;
    int syscall_choice = SYSCALL_NONE;
    int pid_value = -2;   /* -2 means user did not provide pid */

    printf("Phoenix controller\n");

    fd = open(DEVICE, O_RDWR);
    if (fd < 0) {
        perror("Cannot open device");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--log") == 0) {
            mode = MODE_LOG;
        } else if (strcmp(argv[i], "--off") == 0) {
            mode = MODE_OFF;
        } else if (strcmp(argv[i], "--syscall") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --syscall needs a value\n");
                close(fd);
                return 1;
            }

            i++;

            if (strcmp(argv[i], "write") == 0) {
                syscall_choice = SYSCALL_WRITE;
            } else {
                printf("Error: only 'write' is supported in this step\n");
                close(fd);
                return 1;
            }
        } else if (strcmp(argv[i], "--pid") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --pid needs a value\n");
                close(fd);
                return 1;
            }

            i++;
            pid_value = atoi(argv[i]);
        }
    }

    if (mode != -1) {
        printf("Setting mode %d\n", mode);
        if (ioctl(fd, IOCTL_SET_MODE, &mode) < 0) {
            perror("ioctl SET_MODE failed");
            close(fd);
            return 1;
        }
    }

    if (syscall_choice != SYSCALL_NONE) {
        printf("Setting syscall %d\n", syscall_choice);
        if (ioctl(fd, IOCTL_SET_SYSCALL, &syscall_choice) < 0) {
            perror("ioctl SET_SYSCALL failed");
            close(fd);
            return 1;
        }
    }

    if (pid_value != -2) {
        printf("Setting pid %d\n", pid_value);
        if (ioctl(fd, IOCTL_SET_PID, &pid_value) < 0) {
            perror("ioctl SET_PID failed");
            close(fd);
            return 1;
        }
    }

    close(fd);
    printf("Done\n");
    return 0;
}