#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <ctype.h>
#include <strings.h>

/*
 * EN: CLI to manage per-user QEMU VMs and a camera bridge.
 * DE: CLI zum Verwalten von QEMU-VMs pro Benutzer und einer Kamera-Bridge.
 */

#define ACCOUNTS_DIR "./vm/userdata/accounts"   /* EN: per-account folders / DE: Ordner pro Account */
#define BASE_DIR "./vm"                          /* EN: shared VM assets / DE: gemeinsame VM-Assets */
#define VM_BASE_QCOW2 "./vm/base.qcow2"         /* EN: base qcow2 image / DE: Basis-qcow2-Image */
#define VM_LAUNCH_BIN "./vm/launch"             /* EN: helper binary to copy / DE: Hilfs-Binary */
#define VM_PROVISIONER "./vm/provision_base.sh" /* EN: base provision script / DE: Basis-Provisionierung */
#define SSH_PORT_FILE "ssh.port"
#define SSH_V6_PID_FILE "sshv6.pid"
#define SSH_V6_LOG_NAME "sshv6.log"
#define CAMERA_OUT_NAME "camera.mjpg"
#define CAMERA_LOG_NAME "cam.log"
#define CAMERA_PID_NAME "cam.pid"
#define CAMERA_PORT_FILE "camera.port"
#define CONFIG_PATH "./config.cfg"

#define DEFAULT_BASE_IMAGE_URL "https://dl.rockylinux.org/pub/rocky/9/images/x86_64/Rocky-9-GenericCloud.latest.x86_64.qcow2"
#define DEFAULT_IP_MODE "ipv6"

enum ip_mode { IP_MODE_IPV6 = 0, IP_MODE_IPV4 = 1 };

struct Config {
    char base_image_url[PATH_MAX];
    enum ip_mode ip_mode;
};

static struct Config g_cfg = { DEFAULT_BASE_IMAGE_URL, IP_MODE_IPV6 };

/* EN: core commands. / DE: Kernbefehle. */
int selectAccount(char *accountName);
int ensureBaseImage(void);
int ensureBaseProvisioned(void);
int ensureAccountsFolder(void);
void listAccounts(void);
void createUser(void);
void removeUser(void);
void checkUser(void);
void userInfo(void);
void cloneUser(void);
void resetUser(void);
void rebuildBase(void);
pid_t startCameraBridge(const char *accountDir, int preferredStartPort, int *outPort);
pid_t startIPv6Forward(const char *accountDir, int port);
void startVM(void);
void stopVM(void);
 
int deployLaunchBinary(const char *accountDir);
void stopCameraBridge(const char *accountDir);
void stopIPv6Forward(const char *accountDir);

/* EN: helpers. / DE: Hilfsfunktionen. */
int findFreePortFrom(int startPort);
 
void showHelp(void);
void menu(void);
void loadConfig(void);

/* EN: confirm via stdin; empty input means YES. / DE: bestaetigen; leer = JA. */
static int ask_yes_default_yes(const char *prompt) {
    char buf[32];
    printf("%s", prompt);
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    buf[strcspn(buf, "\n")] = '\0';
    if (buf[0] == '\0') return 1; /* EN: default yes / DE: Standard = ja */
    if (buf[0] == 'y' || buf[0] == 'Y') return 1;
    return 0;
}

void showServerIP(void);

/* EN: pidfile helpers. / DE: pidfile-Helfer. */
static pid_t pidfile_read(const char *path);
static int process_is_running(pid_t pid);
static void trim_trailing_ws(char *s);
static void trim_leading_ws(char **p);

/* EN: recursively create a directory path (mkdir -p). / DE: rekursiv Verzeichnis anlegen (mkdir -p). */
static int ensure_dir(const char *path) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    char tmp[PATH_MAX];
    size_t len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len == 0 || len >= sizeof(tmp)) { errno = ENAMETOOLONG; return -1; }
    while (len > 1 && tmp[len-1] == '/') { tmp[len-1] = '\0'; --len; }
    for (char *p = tmp + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0) {
            if (errno != EEXIST) return -1;
        }
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0) {
        if (errno != EEXIST) return -1;
    }
    return 0;
}

/* EN: drop trailing whitespace (incl. newlines). / DE: Leerraum am Ende entfernen. */
static void trim_trailing_ws(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        --len;
    }
}

/* EN: advance pointer past leading whitespace. / DE: Zeiger hinter fuehrenden Leerraum setzen. */
static void trim_leading_ws(char **p) {
    if (!p || !*p) return;
    while (**p && isspace((unsigned char)**p)) (*p)++;
}

/* EN: read pid from pidfile; 0 on error. / DE: PID lesen; 0 bei Fehler. */
static pid_t pidfile_read(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long p = 0;
    if (fscanf(f, "%ld", &p) != 1) p = 0;
    fclose(f);
    if (p <= 0) return 0;
    return (pid_t)p;
}

/* EN: check if pid is alive (EPERM counts as alive). / DE: Prozess lebt? (EPERM gilt). */
static int process_is_running(pid_t pid) {
    if (pid <= 0) return 0;
    if (kill(pid, 0) == 0) return 1;
    if (errno == EPERM) return 1;
    return 0;
}

/* EN: prompt for account name and verify it exists. / DE: Accountname abfragen und pruefen. */
int selectAccount(char *accountName) {
    listAccounts();
    printf("Enter account name: ");
    if (!fgets(accountName, PATH_MAX, stdin)) {
        return 0;
    }
    accountName[strcspn(accountName, "\n")] = 0;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", ACCOUNTS_DIR, accountName);
    DIR *dir = opendir(path);
    if (!dir) { printf("Error: account '%s' does not exist\n", accountName); return 0; }
    closedir(dir);
    return 1;
}

 
/*
 * EN: ensure base qcow2 exists at VM_BASE_QCOW2.
 * DE: Basis-qcow2 unter VM_BASE_QCOW2 sicherstellen.
 */
int ensureBaseImage(void) {
    if (ensure_dir(BASE_DIR) != 0) { perror("mkdir base"); return -1; }

    if (access(VM_BASE_QCOW2, F_OK) != 0) {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s/base-cloudimg.qcow2", BASE_DIR);
        fprintf(stderr, "Base qcow2 not found at %s. Attempting download...\n", VM_BASE_QCOW2);
        const char *url = g_cfg.base_image_url[0] ? g_cfg.base_image_url : DEFAULT_BASE_IMAGE_URL;
        char cmd[PATH_MAX * 2];
        int r = snprintf(cmd, sizeof(cmd), "wget -q -O '%s' -o /dev/null '%s'", tmp, url);
        if (r < 0 || r >= (int)sizeof(cmd)) { fprintf(stderr, "URL too long for command buffer\n"); return -1; }
        if (system(cmd) != 0) { fprintf(stderr, "Failed to download base image\n"); return -1; }
        if (rename(tmp, VM_BASE_QCOW2) != 0) perror("rename base image");
    }
    return ensureBaseProvisioned();
}

/*
 * EN: run provisioning script once per process.
 * DE: Provisioning-Skript einmal pro Prozess ausfuehren.
 */
int ensureBaseProvisioned(void) {
    static int already_ran = 0;
    if (already_ran) return 0;

    if (access(VM_PROVISIONER, X_OK) != 0) {
        fprintf(stderr, "Provisioner script missing or not executable: %s\n", VM_PROVISIONER);
        return -1;
    }

    char cmd[PATH_MAX * 2];
    int r = snprintf(cmd, sizeof(cmd), "%s '%s'", VM_PROVISIONER, VM_BASE_QCOW2);
    if (r < 0 || r >= (int)sizeof(cmd)) {
        fprintf(stderr, "Provision command too long\n");
        return -1;
    }
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "Provisioner failed with code %d\n", rc);
        return -1;
    }
    already_ran = 1;
    return 0;
}

 
/* EN: list existing account directories. / DE: vorhandene Accounts auflisten. */
void listAccounts(void) {
    struct dirent *entry;
    DIR *dp = opendir(ACCOUNTS_DIR);
    if (!dp) { printf("No accounts found\n"); return; }
    printf("Available accounts:\n");
    int idx = 1;
    while ((entry = readdir(dp))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", ACCOUNTS_DIR, entry->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) printf("%d) %s\n", idx++, entry->d_name);
    }
    closedir(dp);
}

 
/* EN: create ./vm/userdata/accounts if needed (mkdir -p). / DE: Ordner bei Bedarf anlegen. */
int ensureAccountsFolder(void) {
    if (ensure_dir(ACCOUNTS_DIR) != 0) { perror("mkdir accounts"); return -1; }
    return 0;
}

 
/* EN: allow only safe account names (alnum + -_.). / DE: nur sichere Namen (alnum + -_.). */
static int validateName(const char *name) {
    if (!name || !*name) return 0;
    if (strchr(name, '/')) return 0;
    for (const char *p = name; *p; ++p) {
        unsigned char c = *p;
        if (!(c == '-' || c == '_' || c == '.' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return 0;
    }
    return 1;
}

 
/* EN: copy a single file, overwriting destination. / DE: Datei kopieren (ueberschreiben). */
static int copyFile(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return -1; }
    char buf[8192];
    ssize_t r;
    while ((r = read(in, buf, sizeof(buf))) > 0) {
        ssize_t w = write(out, buf, (size_t)r);
        if (w != r) { close(in); close(out); return -1; }
    }
    close(in); close(out);
    return (r == 0) ? 0 : -1;
}

 
/* EN: rm -rf equivalent for files/dirs/symlinks. / DE: rm -rf fuer Dateien/Ordner/Symlinks. */
static int remove_recursive(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) { perror("lstat"); return -1; }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) { perror("opendir"); return -1; }
        struct dirent *entry;
        int rc = 0;
        while ((entry = readdir(d))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char child[PATH_MAX * 2];
            if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child)) { rc = -1; break; }
            if (remove_recursive(child) != 0) rc = -1;
        }
        closedir(d);
        if (rmdir(path) != 0) { perror("rmdir"); return -1; }
        return rc;
    }
    
    if (unlink(path) != 0) { perror("unlink"); return -1; }
    return 0;
}

 
/* EN: cp -a equivalent (recurse, keep symlinks/modes). / DE: cp -a (rekursiv, Symlinks/Modi). */
static int copy_recursive(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) != 0) { perror("lstat src"); return -1; }
    
    if (S_ISLNK(st.st_mode)) {
        char buf[PATH_MAX]; ssize_t r = readlink(src, buf, sizeof(buf)-1);
        if (r < 0) { perror("readlink"); return -1; }
        buf[r] = '\0';
        if (symlink(buf, dst) != 0) { perror("symlink"); return -1; }
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, 0755) != 0 && errno != EEXIST) { perror("mkdir dst"); return -1; }
        DIR *d = opendir(src);
        if (!d) { perror("opendir src"); return -1; }
        struct dirent *entry;
        int rc = 0;
        while ((entry = readdir(d))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char child_src[PATH_MAX * 2];
            char child_dst[PATH_MAX * 2];
            if (snprintf(child_src, sizeof(child_src), "%s/%s", src, entry->d_name) >= (int)sizeof(child_src)) { rc = -1; break; }
            if (snprintf(child_dst, sizeof(child_dst), "%s/%s", dst, entry->d_name) >= (int)sizeof(child_dst)) { rc = -1; break; }
            if (copy_recursive(child_src, child_dst) != 0) rc = -1;
        }
        closedir(d);
        return rc;
    }
    
    int in = open(src, O_RDONLY);
    if (in < 0) { perror("open src"); return -1; }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (out < 0) { perror("open dst"); close(in); return -1; }
    char buf[8192]; ssize_t r;
    while ((r = read(in, buf, sizeof(buf))) > 0) {
        ssize_t w = write(out, buf, (size_t)r);
        if (w != r) { perror("write"); close(in); close(out); return -1; }
    }
    close(in); close(out);
    return (r == 0) ? 0 : -1;
}

/* EN: read an integer from a file. / DE: Integer aus Datei lesen. */
static int read_int_file(const char *path, int *out) {
    if (!out) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = 0; int rc = fscanf(f, "%d", &v);
    fclose(f);
    if (rc == 1) { *out = v; return 0; }
    return -1;
}

static int write_int_file(const char *path, int value) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", value);
    ssize_t w = write(fd, buf, (size_t)len);
    close(fd);
    return (w == len) ? 0 : -1;
}

/* EN: copy launch helper into account dir. / DE: Launch-Helfer ins Account-Verz. */
int deployLaunchBinary(const char *accountDir) {
    char dst[PATH_MAX];
    if (snprintf(dst, sizeof(dst), "%s/launch", accountDir) >= (int)sizeof(dst)) return -1;
    if (access(VM_LAUNCH_BIN, X_OK) != 0) {
        fprintf(stderr, "launch helper missing at %s\n", VM_LAUNCH_BIN);
        return -1;
    }
    if (copyFile(VM_LAUNCH_BIN, dst) != 0) {
        perror("copy launch helper");
        return -1;
    }
    chmod(dst, 0755);
    return 0;
}

/*
 * EN: start per-account camera bridge via launch helper.
 * DE: Kamera-Bridge pro Account via Launch-Helfer starten.
 */
pid_t startCameraBridge(const char *accountDir, int preferredStartPort, int *outPort) {
    char bin[PATH_MAX], out[PATH_MAX], logp[PATH_MAX], pidp[PATH_MAX], portfile[PATH_MAX];
    if (snprintf(bin, sizeof(bin), "%s/launch", accountDir) >= (int)sizeof(bin)) return -1;
    if (snprintf(out, sizeof(out), "%s/%s", accountDir, CAMERA_OUT_NAME) >= (int)sizeof(out)) return -1;
    if (snprintf(logp, sizeof(logp), "%s/%s", accountDir, CAMERA_LOG_NAME) >= (int)sizeof(logp)) return -1;
    if (snprintf(pidp, sizeof(pidp), "%s/%s", accountDir, CAMERA_PID_NAME) >= (int)sizeof(pidp)) return -1;
    if (snprintf(portfile, sizeof(portfile), "%s/%s", accountDir, CAMERA_PORT_FILE) >= (int)sizeof(portfile)) return -1;

    pid_t existing = pidfile_read(pidp);
    if (existing && process_is_running(existing)) {
        fprintf(stderr, "Camera bridge already running (pid=%d).\n", (int)existing);
        if (outPort) {
            int port = -1;
            (void)read_int_file(portfile, &port);
            *outPort = port;
        }
        return existing;
    }
    unlink(pidp);
    unlink(portfile);

    if (deployLaunchBinary(accountDir) != 0) {
        fprintf(stderr, "Failed to deploy launch helper into %s\n", accountDir);
        return -1;
    }

    int port = findFreePortFrom(preferredStartPort);
    if (port <= 0) {
        fprintf(stderr, "No free port for camera bridge\n");
        return -1;
    }
    int pw[2];
    if (pipe(pw) != 0) { perror("pipe"); return -1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(pw[0]); close(pw[1]); return -1; }

    if (pid == 0) {
        close(pw[0]);
        int flags = fcntl(pw[1], F_GETFD);
        if (flags != -1) fcntl(pw[1], F_SETFD, flags | FD_CLOEXEC);

        int fd = open(logp, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }

        char portStr[32]; snprintf(portStr, sizeof(portStr), "%d", port);
        char *const argv[] = { bin, "--camera-port", portStr, "--out", out, "--log", logp, "--pid-file", pidp, NULL };
        execv(bin, argv);

        /* EN: report execv failure to parent. / DE: execv-Fehler an Parent melden. */
        int save_errno = errno;
        (void)write(pw[1], &save_errno, sizeof(save_errno));
        close(pw[1]);
        _exit(127);
    }

    close(pw[1]);
    int child_errno = 0;
    ssize_t r = read(pw[0], &child_errno, sizeof(child_errno));
    close(pw[0]);

    if (r == 0) {
        if (write_int_file(portfile, port) != 0) {
            fprintf(stderr, "Warning: failed to write camera port file\n");
        }
        if (outPort) *outPort = port;
        return pid;
    }
    if (r > 0) {
        int status = 0; waitpid(pid, &status, 0);
        fprintf(stderr, "Failed to start camera bridge: errno=%d\n", child_errno);
        unlink(portfile);
    } else {
        int saved = errno; fprintf(stderr, "Failed to start camera bridge: pipe read error: %s\n", strerror(saved));
        waitpid(pid, NULL, 0);
        unlink(portfile);
    }
    return -1;
}

/* EN: stop camera bridge and clean pid/port files. / DE: Kamera-Bridge stoppen, pid/port aufraeumen. */
void stopCameraBridge(const char *accountDir) {
    char pidp[PATH_MAX], portfile[PATH_MAX];
    if (snprintf(pidp, sizeof(pidp), "%s/%s", accountDir, CAMERA_PID_NAME) >= (int)sizeof(pidp)) return;
    if (snprintf(portfile, sizeof(portfile), "%s/%s", accountDir, CAMERA_PORT_FILE) >= (int)sizeof(portfile)) return;
    pid_t pid = pidfile_read(pidp);
    if (pid && process_is_running(pid)) {
        kill(pid, SIGTERM);
    }
    unlink(pidp);
    unlink(portfile);
}

/* EN: start IPv6->IPv4 SSH forward via socat. / DE: IPv6->IPv4 SSH-Forward via socat. */
pid_t startIPv6Forward(const char *accountDir, int port) {
    if (g_cfg.ip_mode != IP_MODE_IPV6) return 0;
    char pidp[PATH_MAX], logp[PATH_MAX];
    if (snprintf(pidp, sizeof(pidp), "%s/%s", accountDir, SSH_V6_PID_FILE) >= (int)sizeof(pidp)) return -1;
    if (snprintf(logp, sizeof(logp), "%s/%s", accountDir, SSH_V6_LOG_NAME) >= (int)sizeof(logp)) return -1;

    pid_t existing = pidfile_read(pidp);
    if (existing && process_is_running(existing)) return existing;
    unlink(pidp);

    const char *socat_bin = NULL;
    if (access("/usr/bin/socat", X_OK) == 0) socat_bin = "/usr/bin/socat";
    else if (access("/usr/sbin/socat", X_OK) == 0) socat_bin = "/usr/sbin/socat";
    if (!socat_bin) {
        fprintf(stderr, "Warning: socat not found; IPv6 SSH forward not started\n");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        int fd = open(logp, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }

        char lspec[128];
        char rspec[128];
        snprintf(lspec, sizeof(lspec), "TCP6-LISTEN:%d,bind=[::],fork,reuseaddr,ipv6only=1", port);
        snprintf(rspec, sizeof(rspec), "TCP4:127.0.0.1:%d", port);

        char *const argv[] = { (char *)socat_bin, lspec, rspec, NULL };
        execv(socat_bin, argv);
        _exit(127);
    }

    FILE *f = fopen(pidp, "w");
    if (f) { fprintf(f, "%d\n", (int)pid); fclose(f); }
    return pid;
}

/* EN: stop IPv6 forwarder if running. / DE: IPv6-Forward stoppen. */
void stopIPv6Forward(const char *accountDir) {
    char pidp[PATH_MAX];
    if (snprintf(pidp, sizeof(pidp), "%s/%s", accountDir, SSH_V6_PID_FILE) >= (int)sizeof(pidp)) return;
    pid_t pid = pidfile_read(pidp);
    if (pid && process_is_running(pid)) {
        kill(pid, SIGTERM);
    }
    unlink(pidp);
}

 
/* EN: bind-scan free TCP port from startPort. / DE: freien TCP-Port ab startPort suchen. */
int findFreePortFrom(int startPort) {
    int s = socket(AF_INET6, SOCK_STREAM, 0);
    if (s < 0) return -1;

    int v6only = 0;
    (void)setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;

    if (startPort < 1) startPort = 1;
    for (int p = startPort; p <= 65535; ++p) {
        addr.sin6_port = htons(p);
        int b = bind(s, (struct sockaddr*)&addr, sizeof(addr));
        if (b == 0) {
            close(s);
            return p;
        }
    }
    close(s);
    return -1;
}
/* EN: create account dir with fresh disk + helper. / DE: Account mit frischer Disk + Helfer. */
void createUser(void) {
    if (ensureAccountsFolder() != 0) { printf("accounts folder missing and cannot be created\n"); return; }
    char name[128];
    printf("Enter new account name: ");
    if (!fgets(name, sizeof(name), stdin)) {
        return;
    }
    name[strcspn(name, "\n")] = 0;
    int only_ws = 1;
    for (char *p = name; *p; ++p) {
        if (!isspace((unsigned char)*p)) { only_ws = 0; break; }
    }
    if (name[0] == '\0' || only_ws) { printf("No account name provided\n"); return; }
    if (!validateName(name)) { printf("Invalid account name\n"); return; }
    char accountPath[PATH_MAX];
    if (snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name) >= (int)sizeof(accountPath)) { printf("Name too long\n"); return; }
    DIR *d = opendir(accountPath); if (d) { closedir(d); printf("Error: Account '%s' already exists!\n", name); return; }
    if (mkdir(accountPath, 0755) != 0 && errno != EEXIST) { perror("mkdir user"); return; }
    char diskPath[PATH_MAX]; if (snprintf(diskPath, sizeof(diskPath), "%s/disk.qcow2", accountPath) >= (int)sizeof(diskPath)) return;
    if (access(VM_BASE_QCOW2, F_OK) == 0) {
        if (copyFile(VM_BASE_QCOW2, diskPath) != 0) fprintf(stderr, "Warning: failed to copy base to %s\n", diskPath);
    }
    if (deployLaunchBinary(accountPath) != 0) {
        fprintf(stderr, "Warning: launch helper not deployed to %s\n", accountPath);
    }
    printf("Account '%s' created at %s\n", name, accountPath);
}

/* EN: delete account after confirmation if VM is stopped. / DE: Account loeschen, wenn VM aus ist. */
void removeUser(void) {
    char name[50];
    printf("Enter account name to delete: ");
    if (!fgets(name, sizeof(name), stdin)) return;
    name[strcspn(name, "\n")] = 0;

    
    if (!validateName(name)) { printf("Invalid account name\n"); return; }

    char accountPath[PATH_MAX]; snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name);
    DIR *d = opendir(accountPath);
    if (!d) { printf("Error: Account '%s' does not exist!\n", name); return; }
    closedir(d);

    /* EN: refuse to delete while VM is running. / DE: nicht loeschen, wenn VM laeuft. */
    char userPid[PATH_MAX]; snprintf(userPid, sizeof(userPid), "%s/%s/vm.pid", ACCOUNTS_DIR, name);
    pid_t existing = pidfile_read(userPid);
    if (existing && process_is_running(existing)) {
        printf("Error: VM for '%s' appears to be running (pid=%d). Stop it before deleting.\n", name, (int)existing);
        return;
    }

    char prompt[256];
    snprintf(prompt, sizeof(prompt), "Are you sure you want to delete '%s'? [Y/n]: ", name);
    if (!ask_yes_default_yes(prompt)) { printf("Aborted.\n"); return; }

    if (remove_recursive(accountPath) != 0) { printf("Error: Failed to delete account '%s'\n", name); return; }
    printf("Account '%s' deleted.\n", name);
}

/* EN: quick existence check. / DE: schneller Existenz-Check. */
void checkUser(void) {
    char name[128]; printf("Enter account name to check: ");
    if (!fgets(name, sizeof(name), stdin)) return;
    name[strcspn(name, "\n")] = 0;
    char accountPath[PATH_MAX]; snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name);
    DIR *d = opendir(accountPath); if (d) { closedir(d); printf("Account '%s' exists.\n", name); } else printf("Account '%s' does not exist.\n", name);
}

/* EN: show account disk path and SSH port. / DE: Disk-Pfad und SSH-Port anzeigen. */
void userInfo(void) {
    char name[128]; printf("Enter account name: ");
    if (!fgets(name, sizeof(name), stdin)) return;
    name[strcspn(name, "\n")] = 0;
    char accountPath[PATH_MAX]; snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name);
    DIR *d = opendir(accountPath); if (!d) { printf("Account '%s' does not exist.\n", name); return; } closedir(d);
    char disk[PATH_MAX * 2];
    if (snprintf(disk, sizeof(disk), "%s/disk.qcow2", accountPath) >= (int)sizeof(disk)) { printf("Internal path too long\n"); return; }
    int port = -1;
    char sshportpath[PATH_MAX];
    if (snprintf(sshportpath, sizeof(sshportpath), "%s/%s", accountPath, SSH_PORT_FILE) < (int)sizeof(sshportpath)) {
        (void)read_int_file(sshportpath, &port);
    }
    if (port <= 0) {
        port = 2200;
        for (int i = 0; name[i]; i++) port += (unsigned char)name[i];
    }
    printf("User: %s\nDisk: %s\nSSH Port: %d\nDisk exists: %s\n", name, disk, port, access(disk, F_OK) == 0 ? "yes" : "no");
}

/* EN: deep copy account dir to new name. / DE: Account-Verzeichnis tief kopieren. */
void cloneUser(void) {
    char src[128], dest[128]; printf("Enter source user: ");
    if (!fgets(src, sizeof(src), stdin)) return;
    src[strcspn(src, "\n")] = 0;
    printf("Enter new user name: ");
    if (!fgets(dest, sizeof(dest), stdin)) return;
    dest[strcspn(dest, "\n")] = 0;
    if (!validateName(dest) || !validateName(src)) { printf("Invalid user name\n"); return; }
    char srcPath[PATH_MAX], destPath[PATH_MAX]; snprintf(srcPath, sizeof(srcPath), "%s/%s", ACCOUNTS_DIR, src); snprintf(destPath, sizeof(destPath), "%s/%s", ACCOUNTS_DIR, dest);
    DIR *d = opendir(srcPath); if (!d) { printf("Source user does not exist.\n"); return; } closedir(d);
    d = opendir(destPath); if (d) { closedir(d); printf("Destination user already exists.\n"); return; }
    
    if (copy_recursive(srcPath, destPath) != 0) { printf("Error: Failed to clone user data\n"); return; }
    if (deployLaunchBinary(destPath) != 0) {
        fprintf(stderr, "Warning: launch helper not deployed to %s\n", destPath);
    }
    printf("User '%s' cloned to '%s'.\n", src, dest);
}

/* EN: reset disk to base image (keep other files). / DE: Disk auf Basis zuruecksetzen. */
void resetUser(void) {
    char name[128];
    printf("Enter account to reset: ");
    if (!fgets(name, sizeof(name), stdin)) return;
    name[strcspn(name, "\n")] = 0;
    if (!validateName(name)) { printf("Invalid account name\n"); return; }
    char accountPath[PATH_MAX], disk[PATH_MAX];
    if (snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name) >= (int)sizeof(accountPath)) return;
    if (snprintf(disk, sizeof(disk), "%s/disk.qcow2", accountPath) >= (int)sizeof(disk)) return;
    DIR *d = opendir(accountPath);
    if (!d) { printf("Account '%s' does not exist\n", name); return; }
    closedir(d);
    if (ensureBaseImage() != 0) { printf("Base image not available\n"); return; }
    if (access(disk, F_OK) == 0) {
        char bak[PATH_MAX + 8];
        snprintf(bak, sizeof(bak), "%s.bak", disk);
        if (rename(disk, bak) != 0) { perror("rename backup"); return; }
    }
    printf("Copying fresh base qcow2...\n");
    if (copyFile(VM_BASE_QCOW2, disk) != 0) { fprintf(stderr, "Failed to copy base -> %s\n", disk); return; }
    if (deployLaunchBinary(accountPath) != 0) {
        fprintf(stderr, "Warning: launch helper not deployed to %s\n", accountPath);
    }
    printf("User '%s' was reset. (disk=%s)\n", name, disk);
}


/* EN: redownload and re-provision base image. / DE: Basis neu laden und provisionieren. */
void rebuildBase(void) {
    printf("Rebuilding base image...\n");
    if (access(VM_BASE_QCOW2, F_OK) == 0) { if (unlink(VM_BASE_QCOW2) != 0) perror("unlink base"); }
    ensureBaseImage();
    printf("Base image rebuilt.\n");
}

/* EN: find non-loopback IPv6/IPv4 for display. / DE: nicht-Loopback IPv6/IPv4 finden. */
void showServerIP(void) {
    struct ifaddrs *ifaddr, *ifa;
    char found6[INET6_ADDRSTRLEN] = "";
    char found4[INET_ADDRSTRLEN] = "";

    if (getifaddrs(&ifaddr) == -1) {
        printf("Server IP: ::1 (fallback)\n");
        return;
    }

    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET6 && !found6[0]) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ifa->ifa_addr;
            if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr)) continue;
            if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) continue;
            char host[INET6_ADDRSTRLEN];
            if (inet_ntop(AF_INET6, &sin6->sin6_addr, host, sizeof(host)) == NULL) continue;
            strncpy(found6, host, sizeof(found6));
            found6[sizeof(found6) - 1] = '\0';
        } else if (ifa->ifa_addr->sa_family == AF_INET && !found4[0]) {
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            char host[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &sin->sin_addr, host, sizeof(host)) == NULL) continue;
            if (strncmp(host, "127.", 4) == 0) continue;
            strncpy(found4, host, sizeof(found4));
            found4[sizeof(found4) - 1] = '\0';
        }
    }

    freeifaddrs(ifaddr);

    if (g_cfg.ip_mode == IP_MODE_IPV4) {
        if (found4[0]) {
            printf("Server IP (ipv4): %s\n", found4);
            return;
        }
        if (found6[0]) {
            printf("Server IP (fallback ipv6): %s\n", found6);
            return;
        }
        printf("Server IP: 127.0.0.1 (fallback)\n");
    } else {
        if (found6[0]) {
            printf("Server IP (ipv6): %s\n", found6);
            return;
        }
        if (found4[0]) {
            printf("Server IP (fallback ipv4): %s\n", found4);
            return;
        }
        printf("Server IP: ::1 (fallback)\n");
    }
}
 

/* EN: start VM with SSH forward + camera bridge. / DE: VM mit SSH-Forward + Kamera-Bridge. */
void startVM(void) {
    const char *qemu_bin = NULL;
    if (access("/usr/bin/qemu-system-x86_64", X_OK) == 0) {
        qemu_bin = "/usr/bin/qemu-system-x86_64";
    } else if (access("/usr/libexec/qemu-kvm", X_OK) == 0) {
        qemu_bin = "/usr/libexec/qemu-kvm";
    }
    if (!qemu_bin) {
        fprintf(stderr, "QEMU is not installed or not in PATH\n");
        return;
    }

    if (ensureBaseImage() != 0) { printf("Base image missing and cannot be prepared\n"); return; }
    char accountName[128]; if (!selectAccount(accountName)) return;
    char diskPath[PATH_MAX]; snprintf(diskPath, sizeof(diskPath), "%s/%s/disk.qcow2", ACCOUNTS_DIR, accountName);

    /* EN: check for existing running VM. / DE: laufende VM pruefen. */
    char pidpath[PATH_MAX]; snprintf(pidpath, sizeof(pidpath), "%s/%s/vm.pid", ACCOUNTS_DIR, accountName);
    pid_t existing = pidfile_read(pidpath);
    if (existing) {
        if (process_is_running(existing)) {
            printf("Error: VM for '%s' already running (pid=%d).\n", accountName, (int)existing);
            return;
        } else {
            /* EN: remove stale pidfile. / DE: veraltetes pidfile loeschen. */
            unlink(pidpath);
        }
    }

    char accountDir[PATH_MAX * 2]; if (snprintf(accountDir, sizeof(accountDir), "%s/%s", ACCOUNTS_DIR, accountName) >= (int)sizeof(accountDir)) { printf("Internal path too long\n"); return; }
    char userLog[PATH_MAX * 2]; if (snprintf(userLog, sizeof(userLog), "%s/vm.log", accountDir) >= (int)sizeof(userLog)) { printf("Internal path too long\n"); return; }
    char userPid[PATH_MAX * 2]; if (snprintf(userPid, sizeof(userPid), "%s/vm.pid", accountDir) >= (int)sizeof(userPid)) { printf("Internal path too long\n"); return; }

    if (access(diskPath, F_OK) != 0) {
        printf("Account '%s' disk.qcow2 not found. Copying base image...\n", accountName);
        if (copyFile(VM_BASE_QCOW2, diskPath) != 0) { perror("copyFile"); return; }
    }

    if (deployLaunchBinary(accountDir) != 0) {
        fprintf(stderr, "Warning: launch helper not deployed to %s\n", accountDir);
    }

    int sshPort = -1;
    char sshportpath[PATH_MAX];
    if (snprintf(sshportpath, sizeof(sshportpath), "%s/%s", accountDir, SSH_PORT_FILE) >= (int)sizeof(sshportpath)) { printf("Internal path too long\n"); return; }

    int storedPort = -1;
    (void)read_int_file(sshportpath, &storedPort);

    if (storedPort > 0) {
        sshPort = findFreePortFrom(storedPort);
    } else {
        sshPort = findFreePortFrom(2200);
    }

    if (sshPort <= 0) { printf("No free ports available for SSH\n"); return; }
    if (write_int_file(sshportpath, sshPort) != 0) {
        fprintf(stderr, "Warning: could not persist ssh port to %s\n", sshportpath);
    }

    int cameraPort = -1;
    pid_t camPid = startCameraBridge(accountDir, sshPort + 1, &cameraPort);
    if (camPid <= 0) {
        fprintf(stderr, "Warning: camera bridge not started for '%s'\n", accountName);
    }

    int pw[2];
    if (pipe(pw) != 0) { perror("pipe"); return; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(pw[0]); close(pw[1]); return; }

    if (pid == 0) {
        close(pw[0]);
        int flags = fcntl(pw[1], F_GETFD);
        if (flags != -1) fcntl(pw[1], F_SETFD, flags | FD_CLOEXEC);

        int fd = open(userLog, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }

        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); if (nullfd != STDIN_FILENO) close(nullfd); }

        /* EN: bind SSH on IPv4; IPv6 uses socat bridge. / DE: SSH via IPv4; IPv6 via socat. */
        char netdevarg[192];
        snprintf(netdevarg, sizeof(netdevarg), "user,id=net0,hostfwd=tcp:127.0.0.1:%d-:22", sshPort);
        char drivearg[PATH_MAX + 64];
        snprintf(drivearg, sizeof(drivearg), "file=%s,format=qcow2,if=virtio", diskPath);
        char *const argv[] = {
            (char *)qemu_bin,
            "-m", "512M",
            "-cpu", "host",
            "-nographic",
            "-device", "virtio-rng-pci", /* EN: entropy for sshd. / DE: Entropie fuer sshd. */
            "-netdev", netdevarg,
            "-device", "virtio-net-pci,netdev=net0",
            "-drive", drivearg,
            NULL
        };
        execv(qemu_bin, argv);

        
        /* EN: report execv failure to parent. / DE: execv-Fehler an Parent melden. */
        int save_errno = errno;
        (void)write(pw[1], &save_errno, sizeof(save_errno));
        close(pw[1]);
        _exit(127);
    }

    close(pw[1]);
    int child_errno = 0;
    ssize_t r = read(pw[0], &child_errno, sizeof(child_errno));
    close(pw[0]);

    if (r == 0) {
        FILE *f = fopen(userPid, "w"); if (f) { fprintf(f, "%d\n", pid); fclose(f); }
        int fd = open(userLog, O_CREAT | O_WRONLY | O_APPEND, 0644); if (fd >= 0) close(fd);
        if (g_cfg.ip_mode == IP_MODE_IPV6) {
            pid_t v6pid = startIPv6Forward(accountDir, sshPort);
            if (v6pid <= 0) {
                fprintf(stderr, "Warning: IPv6 SSH forward not started for '%s'\n", accountName);
            }
        }
        if (cameraPort > 0) {
            printf("VM started: ssh=%d camera=%d pid=%d disk=%s log=%s pidfile=%s (cam pid may be %d)\n", sshPort, cameraPort, pid, diskPath, userLog, userPid, (int)camPid);
        } else {
            printf("VM started: ssh=%d pid=%d disk=%s log=%s pidfile=%s\n", sshPort, pid, diskPath, userLog, userPid);
        }
    } else if (r > 0) {
        int status = 0; waitpid(pid, &status, 0);
        fprintf(stderr, "Failed to start qemu: exec failed (errno=%d)\n", child_errno);
        stopCameraBridge(accountDir);
        unlink(sshportpath);
        return;
    } else {
        int saved = errno; fprintf(stderr, "Failed to start qemu: pipe read error: %s\n", strerror(saved));
        waitpid(pid, NULL, 0);
        stopCameraBridge(accountDir);
        unlink(sshportpath);
        return;
    }
    int fd = open(userLog, O_CREAT | O_WRONLY | O_APPEND, 0644); if (fd >= 0) close(fd);
}

/* EN: stop VM and camera bridge; clean pid files. / DE: VM+Kamera stoppen; pid aufraeumen. */
void stopVM(void) {
    char accountName[128];
    if (!selectAccount(accountName)) return;
    char accountDir[PATH_MAX]; snprintf(accountDir, sizeof(accountDir), "%s/%s", ACCOUNTS_DIR, accountName);
    char userPid[PATH_MAX]; snprintf(userPid, sizeof(userPid), "%s/vm.pid", accountDir);
    pid_t pid = pidfile_read(userPid);
    if (!pid) { printf("No pidfile found for '%s'. Is the VM running?\n", accountName); return; }
    if (!process_is_running(pid)) {
        printf("Stale pidfile found (pid=%d). Removing pidfile.\n", (int)pid);
        if (unlink(userPid) != 0) perror("unlink pidfile");
        stopIPv6Forward(accountDir);
        stopCameraBridge(accountDir);
        return;
    }
    if (kill((pid_t)pid, SIGTERM) != 0) { perror("kill"); return; }
    if (unlink(userPid) != 0) perror("unlink pidfile");
    stopIPv6Forward(accountDir);
    stopCameraBridge(accountDir);
    printf("Sent SIGTERM to pid %d for account '%s' and stopped camera bridge\n", (int)pid, accountName);
}

 
/* EN: print the interactive command list. / DE: interaktive Befehlsliste. */
void showHelp(void) {
    printf("\nAvailable commands:\n");
    printf("checkuser     - Check if an account exists.\n");
    printf("cloneuser     - Clone an existing user.\n");
    printf("createuser    - Create new account.\n");
    printf("exit          - Exit terminal.\n");
    printf("help          - Show this help.\n");
    printf("listuser      - List accounts.\n");
    printf("rebuildbase   - Redownload the base qcow2 image.\n");
    printf("removeuser    - Delete an account.\n");
    printf("resetuser     - Reset a user's disk.qcow2 from base.\n");
    printf("startvm       - Start a VM.\n");
    printf("stopvm        - Stop a VM.\n");
    printf("serverip      - Show server IP address.\n");
    printf("userinfo      - Show info about a user.\n\n");
}

/* EN: REPL-style command loop. / DE: REPL-aehnliche Schleife. */
void menu(void) {
    char input[64];
    printf("For help type 'help'\n");
    while (1) {
        printf("\n> ");
        if (!fgets(input, sizeof(input), stdin)) break;
        input[strcspn(input, "\n")] = '\0';

        if (!strcmp(input, "startvm")) startVM();
        else if (!strcmp(input, "stopvm")) stopVM();
        else if (!strcmp(input, "listuser")) listAccounts();
        else if (!strcmp(input, "createuser")) createUser();
        else if (!strcmp(input, "removeuser")) removeUser();
        else if (!strcmp(input, "checkuser")) checkUser();
        else if (!strcmp(input, "userinfo")) userInfo();
        else if (!strcmp(input, "cloneuser")) cloneUser();
        else if (!strcmp(input, "resetuser")) resetUser();
        else if (!strcmp(input, "rebuildbase")) rebuildBase();
        else if (!strcmp(input, "help")) showHelp();
        else if (!strcmp(input, "serverip")) showServerIP();
        else if (!strcmp(input, "exit")) {
            if (ask_yes_default_yes("Are you sure you want to exit? [Y/n]: ")) exit(0);
            continue;
        }
        else if (strlen(input) == 0) continue;
        else printf("Error: Unknown command '%s'. Type 'help' for available commands.\n", input);
    }
}

/* EN: entry point (prepare folders/base, then menu). / DE: Einstieg (Ordner/Basis, dann Menu). */
int main(void) {
    loadConfig();
    if (ensureAccountsFolder() != 0) {
        fprintf(stderr, "Failed to create accounts directory\n");
        return 1;
    }
    if (ensureBaseImage() != 0) fprintf(stderr, "Warning: base image not available\n");
    menu();
    
    return 0;
}

/* EN: parse config.cfg for base_image_url and ip_mode. / DE: config.cfg fuer base_image_url und ip_mode. */
void loadConfig(void) {
    /* EN: reset defaults. / DE: Standardwerte setzen. */
    snprintf(g_cfg.base_image_url, sizeof(g_cfg.base_image_url), "%s", DEFAULT_BASE_IMAGE_URL);
    g_cfg.ip_mode = IP_MODE_IPV6;

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        trim_leading_ws(&p);
        if (*p == '#' || *p == ';' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        trim_trailing_ws(p);
        trim_trailing_ws(val);
        trim_leading_ws(&val);

        if (strcasecmp(p, "base_image_url") == 0) {
            if (*val) {
                snprintf(g_cfg.base_image_url, sizeof(g_cfg.base_image_url), "%s", val);
            }
        } else if (strcasecmp(p, "ip_mode") == 0) {
            if (strcasecmp(val, "ipv4") == 0) g_cfg.ip_mode = IP_MODE_IPV4;
            else if (strcasecmp(val, "ipv6") == 0) g_cfg.ip_mode = IP_MODE_IPV6;
        }
    }

    fclose(f);
}
