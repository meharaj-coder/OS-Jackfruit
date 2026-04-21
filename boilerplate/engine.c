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

#define SOCK_PATH "/tmp/mini_runtime.sock"
#define STACK_SIZE (1024 * 1024)
#define MAX_CONTAINERS 32

typedef struct {
    char id[32];
    pid_t pid;
    int running;
} container_t;

container_t containers[MAX_CONTAINERS];
int container_count = 0;

/* ---------------- CHILD (CONTAINER) ---------------- */

static int child_fn(void *arg) {
    char **args = (char **)arg;
    char *rootfs = args[0];
    char *cmd = args[1];

    sethostname("container", 9);

    if (chroot(rootfs) != 0) {
        perror("chroot");
        return -1;
    }

    chdir("/");

    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc");
    }

    execlp(cmd, cmd, NULL);
    perror("exec");
    return -1;
}

/* ---------------- START CONTAINER ---------------- */

void start_container(char *id, char *rootfs, char *cmd) {
    if (container_count >= MAX_CONTAINERS) {
        printf("Max containers reached\n");
        return;
    }

    char *stack = malloc(STACK_SIZE);
    char *args[2] = {rootfs, cmd};

    pid_t pid = clone(child_fn,
                      stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      args);

    if (pid < 0) {
        perror("clone");
        return;
    }

    printf("Started container %s (PID %d)\n", id, pid);

    strcpy(containers[container_count].id, id);
    containers[container_count].pid = pid;
    containers[container_count].running = 1;
    container_count++;
}

/* ---------------- STOP ---------------- */

void stop_container(char *id) {
    for (int i = 0; i < container_count; i++) {
        if (strcmp(containers[i].id, id) == 0 && containers[i].running) {
            kill(containers[i].pid, SIGTERM);
            waitpid(containers[i].pid, NULL, 0);
            containers[i].running = 0;
            printf("Stopped %s\n", id);
            return;
        }
    }
    printf("Container not found\n");
}

/* ---------------- PS ---------------- */

void list_containers(int client_fd) {
    char buffer[256];

    for (int i = 0; i < container_count; i++) {
        snprintf(buffer, sizeof(buffer),
                 "%s\t%d\t%s\n",
                 containers[i].id,
                 containers[i].pid,
                 containers[i].running ? "RUNNING" : "STOPPED");

        write(client_fd, buffer, strlen(buffer));
    }
}

/* ---------------- HANDLE COMMAND ---------------- */

void handle_command(int client_fd, char *cmdline) {
    char cmd[32], id[32], rootfs[128], exec_cmd[128];

    sscanf(cmdline, "%s %s %s %s", cmd, id, rootfs, exec_cmd);

    if (strcmp(cmd, "start") == 0) {
        start_container(id, rootfs, exec_cmd);
    }
    else if (strcmp(cmd, "stop") == 0) {
        stop_container(id);
    }
    else if (strcmp(cmd, "ps") == 0) {
        list_containers(client_fd);
    }
}

/* ---------------- SUPERVISOR ---------------- */

void run_supervisor(char *base_rootfs) {
    int server_fd, client_fd;
    struct sockaddr_un addr;

    unlink(SOCK_PATH);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("Supervisor running...\n");

    while (1) {
        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        char buffer[256] = {0};
        read(client_fd, buffer, sizeof(buffer));

        handle_command(client_fd, buffer);

        close(client_fd);

        while (waitpid(-1, NULL, WNOHANG) > 0);
    }
}

/* ---------------- CLIENT ---------------- */

void send_command(char *cmdline) {
    int sock;
    struct sockaddr_un addr;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        exit(1);
    }

    write(sock, cmdline, strlen(cmdline));

    char buffer[512];
    int n = read(sock, buffer, sizeof(buffer)-1);
    if (n > 0) {
        buffer[n] = '\0';
        printf("%s", buffer);
    }

    close(sock);
}

/* ---------------- MAIN ---------------- */

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
