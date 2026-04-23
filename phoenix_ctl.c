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
#define IOCTL_GET_EVENT   _IOR(PHX_MAGIC, 4, int)

#define MODE_OFF   0
#define MODE_LOG   1
#define MODE_BLOCK 2

#define SYSCALL_NONE  0
#define SYSCALL_WRITE 1
#define SYSCALL_READ  2

#define MAX_STATES 16

static int syscall_from_name(const char *name)
{
    if (strcmp(name, "write") == 0)
        return SYSCALL_WRITE;

    if (strcmp(name, "read") == 0)
        return SYSCALL_READ;

    return SYSCALL_NONE;
}

static const char *syscall_to_name(int syscall_id)
{
    if (syscall_id == SYSCALL_WRITE)
        return "write";

    if (syscall_id == SYSCALL_READ)
        return "read";

    return "none";
}

/* Very simple beginner JSON parser.
   It only looks for words "write" and "read" inside the file. */
static int load_fsm_file(const char *filename, int states[])
{
    FILE *fp;
    char buffer[1024];
    int count = 0;

    fp = fopen(filename, "r");
    if (!fp) {
        perror("Cannot open FSM file");
        return -1;
    }

    while (fscanf(fp, "%1023s", buffer) == 1) {
        if (strstr(buffer, "write") != NULL) {
            if (count >= MAX_STATES) {
                printf("Error: too many FSM states\n");
                fclose(fp);
                return -1;
            }

            states[count] = SYSCALL_WRITE;
            count++;
        } else if (strstr(buffer, "read") != NULL) {
            if (count >= MAX_STATES) {
                printf("Error: too many FSM states\n");
                fclose(fp);
                return -1;
            }

            states[count] = SYSCALL_READ;
            count++;
        }
    }

    fclose(fp);
    return count;
}

static int set_mode_ioctl(int fd, int mode)
{
    printf("Setting mode %d\n", mode);

    if (ioctl(fd, IOCTL_SET_MODE, &mode) < 0) {
        perror("ioctl SET_MODE failed");
        return -1;
    }

    return 0;
}

static int set_syscall_ioctl(int fd, int syscall_choice)
{
    printf("Setting syscall %s\n", syscall_to_name(syscall_choice));

    if (ioctl(fd, IOCTL_SET_SYSCALL, &syscall_choice) < 0) {
        perror("ioctl SET_SYSCALL failed");
        return -1;
    }

    return 0;
}

static int set_pid_ioctl(int fd, int pid_value)
{
    printf("Setting pid %d\n", pid_value);

    if (ioctl(fd, IOCTL_SET_PID, &pid_value) < 0) {
        perror("ioctl SET_PID failed");
        return -1;
    }

    return 0;
}

static int get_event_ioctl(int fd)
{
    int event_value = SYSCALL_NONE;

    if (ioctl(fd, IOCTL_GET_EVENT, &event_value) < 0) {
        perror("ioctl GET_EVENT failed");
        return -1;
    }

    return event_value;
}

static int run_fsm(int fd, const char *file_name, int pid_value)
{
    int states[MAX_STATES];
    int state_count;
    int current_state = 0;
    int mode = MODE_LOG;

    state_count = load_fsm_file(file_name, states);

    if (state_count <= 0) {
        printf("Error: FSM file must contain at least one state: write or read\n");
        return 1;
    }

    printf("Loaded FSM with %d states\n", state_count);

    if (set_mode_ioctl(fd, mode) < 0)
        return 1;

    if (pid_value != -2) {
        if (set_pid_ioctl(fd, pid_value) < 0)
            return 1;
    }

    if (set_syscall_ioctl(fd, states[current_state]) < 0)
        return 1;

    printf("FSM started. Current state: %s\n", syscall_to_name(states[current_state]));
    printf("Press Ctrl+C to stop.\n");

    while (1) {
        int event_value;

        sleep(1);

        event_value = get_event_ioctl(fd);

        if (event_value == -1)
            return 1;

        if (event_value == states[current_state]) {
            printf("Observed syscall: %s\n", syscall_to_name(event_value));

            current_state++;

            if (current_state >= state_count)
                current_state = 0;

            printf("Transitioning to state: %s\n", syscall_to_name(states[current_state]));

            if (set_syscall_ioctl(fd, states[current_state]) < 0)
                return 1;
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    int fd;
    int mode = -1;
    int syscall_choice = SYSCALL_NONE;
    int pid_value = -2;
    int get_event = 0;
    char *fsm_file = NULL;

    printf("Phoenix controller\n");

    fd = open(DEVICE, O_RDWR);
    if (fd < 0) {
        perror("Cannot open device");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--log") == 0) {
            mode = MODE_LOG;
        } else if (strcmp(argv[i], "--block") == 0) {
            mode = MODE_BLOCK;
        } else if (strcmp(argv[i], "--off") == 0) {
            mode = MODE_OFF;
        } else if (strcmp(argv[i], "--get-event") == 0) {
            get_event = 1;
        } else if (strcmp(argv[i], "--file") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --file needs a filename\n");
                close(fd);
                return 1;
            }

            i++;
            fsm_file = argv[i];
        } else if (strcmp(argv[i], "--syscall") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --syscall needs a value\n");
                close(fd);
                return 1;
            }

            i++;

            syscall_choice = syscall_from_name(argv[i]);

            if (syscall_choice == SYSCALL_NONE) {
                if (strcmp(argv[i], "open") == 0)
                    printf("Error: open is disabled because it is unstable on this kernel\n");
                else
                    printf("Error: supported syscalls are: write, read\n");

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

    if (fsm_file != NULL) {
        if (mode != MODE_LOG) {
            printf("Error: --file can only be used with --log\n");
            close(fd);
            return 1;
        }

        return run_fsm(fd, fsm_file, pid_value);
    }

    if (mode != -1) {
        if (set_mode_ioctl(fd, mode) < 0) {
            close(fd);
            return 1;
        }
    }

    if (syscall_choice != SYSCALL_NONE) {
        if (set_syscall_ioctl(fd, syscall_choice) < 0) {
            close(fd);
            return 1;
        }
    }

    if (pid_value != -2) {
        if (set_pid_ioctl(fd, pid_value) < 0) {
            close(fd);
            return 1;
        }
    }

    if (get_event) {
        int event_value;

        event_value = get_event_ioctl(fd);

        if (event_value == SYSCALL_WRITE) {
            printf("Last observed syscall event: write\n");
        } else if (event_value == SYSCALL_READ) {
            printf("Last observed syscall event: read\n");
        } else {
            printf("No syscall event observed\n");
        }
    }

    close(fd);
    printf("Done\n");
    return 0;
}