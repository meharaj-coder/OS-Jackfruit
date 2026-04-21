#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#define SOCK_PATH "/tmp/engine.sock"
#define STACK_SIZE (1024 * 1024)
#define MAX_CONTAINERS 32

typedef struct {
    char id[32];
    pid_t pid;
    char rootfs[128];
    char cmd[128];
    int running;
} container_t;

container_t containers[MAX_CONTAINERS];
int container_count = 0;

/* ---------------- Container Function ---------------- */

static int container_main(void *arg) {
    char **argv = (char **)arg;

    // Set hostname
    sethostname("container", 9);

    // Change root filesystem
    if (chroot(argv[0]) != 0) {
        perror("chroot failed");
        return -1;
    }
    chdir("/");

    // Mount /proc
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc failed");
    }

    // Execute command
    execlp(argv[1], argv[1], NULL);
    perror("exec failed");
    return -1;
}

/* ---------------- Start Container ---------------- */

void start_container(char *id, char *rootfs, char *cmd) {
    if (container_count >= MAX_CONTAINERS) {
        printf("Max containers reached\n");
        return;
    }

    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc");
        return;
    }

    char *args[2];
    args[0] = rootfs;
    args[1] = cmd;

    pid_t pid = clone(container_main,
                      stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      args);

    if (pid < 0) {
        perror("clone failed");
        return;
    }

    printf("Container %s started with PID %d\n", id, pid);

    strcpy(containers[container_count].id, id);
    strcpy(containers[container_count].rootfs, rootfs);
    strcpy(containers[container_count].cmd, cmd);
    containers[container_count].pid = pid;
    containers[container_count].running = 1;

    container_count++;
}

/* ---------------- Stop Container ---------------- */

void stop_container(char *id) {
    for (int i = 0; i < container_count; i++) {
        if (strcmp(containers[i].id, id) == 0 && containers[i].running) {
            kill(containers[i].pid, SIGTERM);
            waitpid(containers[i].pid, NULL, 0);
            containers[i].running = 0;
            printf("Container %s stopped\n", id);
            return;
        }
    }
    printf("Container not found\n");
}

/* ---------------- List Containers ---------------- */

void list_containers() {
    printf("ID\tPID\tSTATE\n");
    for (int i = 0; i < container_count; i++) {
        printf("%s\t%d\t%s\n",
               containers[i].id,
               containers[i].pid,
               containers[i].running ? "RUNNING" : "STOPPED");
    }
}

/* ---------------- Supervisor ---------------- */

void run_supervisor(char *base_rootfs) {
    int server_fd, client_fd;
    struct sockaddr_un addr;

    unlink(SOCK_PATH);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("Supervisor running...\n");

    while (1) {
        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        char buffer[256] = {0};
        read(client_fd, buffer, sizeof(buffer));

        printf("Received: %s\n", buffer);

        char cmd[32], id[32], rootfs[128], exec_cmd[128];

        sscanf(buffer, "%s %s %s %s", cmd, id, rootfs, exec_cmd);

        if (strcmp(cmd, "start") == 0) {
            start_container(id, rootfs, exec_cmd);
        } else if (strcmp(cmd, "stop") == 0) {
            stop_container(id);
        } else if (strcmp(cmd, "ps") == 0) {
            list_containers();
        }

        close(client_fd);

        // Reap zombies
        while (waitpid(-1, NULL, WNOHANG) > 0);
    }
}

/* ---------------- CLI ---------------- */

void send_command(char *cmdline) {
    int sock;
    struct sockaddr_un addr;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        exit(1);
    }

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        exit(1);
    }

    write(sock, cmdline, strlen(cmdline));
    close(sock);
}

/* ---------------- Main ---------------- */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage:\n");
        printf("  engine supervisor <rootfs>\n");
        printf("  engine start <id> <rootfs> <cmd>\n");
        printf("  engine stop <id>\n");
        printf("  engine ps\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        run_supervisor(argv[2]);
    } else {
        char buffer[256] = {0};

        for (int i = 1; i < argc; i++) {
            strcat(buffer, argv[i]);
            strcat(buffer, " ");
        }

        send_command(buffer);
    }

    return 0;
}