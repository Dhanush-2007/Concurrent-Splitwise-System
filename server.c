/*
 * server.c – Concurrent Expense Settlement System
 *
 * Architecture
 * ============
 * 1. Listens on TCP PORT for incoming client connections.
 * 2. Each accepted connection is handed to a new POSIX thread.
 * 3. A dedicated IPC-listener thread watches the message queue
 *    and named semaphore; it logs every broadcast event.
 * 4. File I/O on expenses.dat is protected by both a pthread_mutex
 *    (intra-process) and a POSIX advisory fcntl lock (inter-process).
 * 5. Shared-memory balances (SharedBalance) use an embedded
 *    PTHREAD_PROCESS_SHARED mutex.
 * 6. SIGINT / SIGTERM trigger graceful cleanup.
 *
 * OS Concepts demonstrated
 * ========================
 *  4.1 Role-Based Authorization  – auth.h
 *  4.2 File Locking              – filelock.h  (fcntl F_RDLCK / F_WRLCK)
 *  4.3 Concurrency Control       – pthread_mutex, semaphore (ipc.h / expense.h)
 *  4.4 Data Consistency          – optimistic conflict detection in expense_settle()
 *  4.5 Socket Programming        – accept() loop, thread-per-client
 *  4.6 IPC                       – shared memory + message queue + named semaphore
 */

#include "common.h"
#include "auth.h"
#include "filelock.h"
#include "ipc.h"
#include "expense.h"

/* Globals */

static int g_server_fd  = -1;//server socket fd
static volatile int g_running = 1;//controls whether server loops continue running

/* Named semaphore to cap concurrent clients (macOS-compatible) */
#define CONN_SEM_NAME "/expense_conn_sem"
static sem_t *g_conn_sem = SEM_FAILED;//pointer

/* ── Per-client thread arg ───────────────────────────────────── */

typedef struct {
    int  client_fd;
    char client_ip[INET_ADDRSTRLEN];
} ClientArg;

/* ── Signal handler ──────────────────────────────────────────── */

static void sig_handler(int sig) {//ctrl+c/sigterm
    (void)sig;
    g_running = 0;
    /* Wake up accept() by closing the server socket */
    if (g_server_fd != -1) { close(g_server_fd); g_server_fd = -1; }
}

/* ── Utility: send a Response packet ────────────────────────── */

static void send_response(int fd, int status, const char *message) {//send server response to client
    Response r;
    r.status = status;
    strncpy(r.message, message, sizeof(r.message) - 1);
    r.message[sizeof(r.message)-1] = '\0';
    send(fd, &r, sizeof(r), 0);
}

/* Dispatch a client packet */

static void dispatch(int cfd, const Packet *pkt,
                     int *logged_in, int *session_role,
                     int *session_uid, char *session_user,
                     char *session_token) {//take a client packet and call the requested operation
    //cfd-client socket descriptor
    char msg[BUFFER_SIZE - 64];//temporary msg buffer
    msg[0] = '\0';//initially empty

    switch (pkt->op) {

    /* ── LOGIN ────────────────────────────────────────────── */
    case OP_LOGIN: {
        if (*logged_in) {
            send_response(cfd, 1, "Already logged in.");
            return;
        }
        /* payload: "username password" */
        char uname[MAX_NAME] = {0}, pass[MAX_PASS] = {0};
        sscanf(pkt->payload, "%63s %63s", uname, pass);

        int role = -1, uid = -1;
        const char *tok = auth_login(uname, pass, &role, &uid);//generates a token
        if (!tok) {
            send_response(cfd, 1, "ERROR: Invalid credentials.");
            return;
        }
        *logged_in = 1;
        *session_role = role;
        *session_uid = uid;
        strncpy(session_user, uname, MAX_NAME - 1);
        strncpy(session_token, tok, 31);

        const char *role_str =
            (role == ROLE_ADMIN) ? "admin" :
            (role == ROLE_USER)  ? "user"  : "guest";

        snprintf(msg, sizeof(msg),
                 "OK: Welcome %s! Role: %s. Token: %s",
                 uname, role_str, tok);
        audit_log(uname, "LOGIN");
        send_response(cfd, 0, msg);//0-success
        break;
    }

    /* ── LOGOUT ───────────────────────────────────────────── */
    case OP_LOGOUT: {
        if (!*logged_in) { send_response(cfd, 1, "Not logged in."); return; }
        auth_logout(session_token);
        audit_log(session_user, "LOGOUT");
        *logged_in = 0;
        send_response(cfd, 0, "OK: Logged out.");
        break;
    }

    /* ── ADD EXPENSE ──────────────────────────────────────── */
    case OP_ADD_EXPENSE: {
        if (!*logged_in) { send_response(cfd, 1, "Not logged in."); return; }
        /*
         * payload: "amount|description|user1,user2,..."
         */
        char  pay_str[32], desc[MAX_DESC], split[256];
        if (sscanf(pkt->payload, "%31[^|]|%127[^|]|%255[^\n]",
                   pay_str, desc, split) != 3) {//reads characters until '|'
            send_response(cfd, 1, "ERROR: Bad format. Use: amount|desc|u1,u2");
            return;
        }
        double amount = atof(pay_str);//string to float
        if (amount <= 0) { send_response(cfd, 1, "ERROR: Amount must be > 0."); return; }

        int rc = expense_add(*session_role, *session_uid, session_user,
                             desc, amount, split, msg, sizeof(msg));
        send_response(cfd, rc, msg);//rc:0-success,1-error
        break;
    }

    /* ── SETTLE EXPENSE ───────────────────────────────────── */
    case OP_SETTLE_EXPENSE: {
        if (!*logged_in) { send_response(cfd, 1, "Not logged in."); return; }
        int exp_id = atoi(pkt->payload);//ascii to int
        int rc = expense_settle(*session_role, *session_uid, session_user,
                                exp_id, msg, sizeof(msg));
        send_response(cfd, rc, msg);
        break;
    }

    /* ── VIEW EXPENSES ────────────────────────────────────── */
    case OP_VIEW_EXPENSES: {
        if (!*logged_in) { send_response(cfd, 1, "Not logged in."); return; }
        char out[BUFFER_SIZE - 64];
        expense_view(*session_role, *session_uid, out, sizeof(out));
        send_response(cfd, 0, out);
        break;
    }

    /* ── VIEW BALANCES ────────────────────────────────────── */
    case OP_VIEW_BALANCES: {
        if (!*logged_in) { send_response(cfd, 1, "Not logged in."); return; }
        char out[BUFFER_SIZE - 64];
        balance_view(*session_role, *session_uid, out, sizeof(out));
        send_response(cfd, 0, out);
        break;
    }

    /* ── ADMIN: ADD USER ──────────────────────────────────── */
    case OP_ADD_USER: {
        if (!*logged_in) { send_response(cfd, 1, "Not logged in."); return; }
        if (auth_require_role(*session_role, ROLE_ADMIN) != OK) {
            send_response(cfd, 1, "ERROR: Admin only."); return;
        }
        /* payload: "username password role(0/1/2)" */
        char uname[MAX_NAME], pass[MAX_PASS];
        int  role = ROLE_USER;
        sscanf(pkt->payload, "%63s %63s %d", uname, pass, &role);
        if (auth_add_user(uname, pass, role) == OK) {
            snprintf(msg, sizeof(msg), "OK: User '%s' created.", uname);
            audit_log(session_user, msg);
            send_response(cfd, 0, msg);
        } else {
            send_response(cfd, 1, "ERROR: Could not create user (exists or limit).");
        }
        break;
    }

    /* ── ADMIN: DELETE USER ───────────────────────────────── */
    case OP_DELETE_USER: {
        if (!*logged_in) { send_response(cfd, 1, "Not logged in."); return; }
        if (auth_require_role(*session_role, ROLE_ADMIN) != OK) {
            send_response(cfd, 1, "ERROR: Admin only."); return;
        }
        char uname[MAX_NAME];
        sscanf(pkt->payload, "%63s", uname);
        if (auth_delete_user(uname) == OK) {
            snprintf(msg, sizeof(msg), "OK: User '%s' deleted.", uname);
            audit_log(session_user, msg);
            send_response(cfd, 0, msg);
        } else {
            send_response(cfd, 1, "ERROR: User not found.");
        }
        break;
    }

    /* ── ADMIN: VIEW USERS ────────────────────────────────── */
    case OP_VIEW_USERS: {
        if (!*logged_in) { send_response(cfd, 1, "Not logged in."); return; }
        if (auth_require_role(*session_role, ROLE_ADMIN) != OK) {
            send_response(cfd, 1, "ERROR: Admin only."); return;
        }
        User users[MAX_USERS];
        int  count = 0;
        load_users(users, &count);
        size_t pos = 0;
        pos += snprintf(msg + pos, sizeof(msg) - pos,
                        "%-4s %-16s %-8s %-6s\n",
                        "ID","Username","Role","Active");
        pos += snprintf(msg + pos, sizeof(msg) - pos,
                        "%-4s %-16s %-8s %-6s\n",
                        "----","----------------","--------","------");
        for (int i = 0; i < count && pos < sizeof(msg) - 64; i++) {
            const char *rs =
                (users[i].role == ROLE_ADMIN) ? "admin" :
                (users[i].role == ROLE_USER)  ? "user"  : "guest";
            pos += snprintf(msg + pos, sizeof(msg) - pos,
                            "%-4d %-16s %-8s %-6s\n",
                            users[i].id, users[i].username,
                            rs, users[i].active ? "yes" : "no");
        }
        send_response(cfd, 0, msg);
        break;
    }

    /* ── ADMIN: VIEW LOG ──────────────────────────────────── */
    case OP_VIEW_LOG: {
        if (!*logged_in) { send_response(cfd, 1, "Not logged in."); return; }
        if (auth_require_role(*session_role, ROLE_ADMIN) != OK) {
            send_response(cfd, 1, "ERROR: Admin only."); return;
        }
        /* Read last N lines of log file */
        FILE *f = fopen(LOG_FILE, "r");
        if (!f) { send_response(cfd, 1, "ERROR: Log unavailable."); return; }

        /* Collect lines */
        char lines[50][256];//upto 50 log lines
        int  lcount = 0;
        while (lcount < 50 && fgets(lines[lcount], 256, f)) lcount++;
        fclose(f);

        size_t pos = 0;
        for (int i = 0; i < lcount && pos < sizeof(msg) - 64; i++)
            pos += snprintf(msg + pos, sizeof(msg) - pos, "%s", lines[i]);
        send_response(cfd, 0, pos ? msg : "(log empty)");
        break;
    }

    default:
        send_response(cfd, 1, "ERROR: Unknown operation.");
        break;
    }
}

/* ── Client handler thread ───────────────────────────────────── */

static void *client_thread(void *arg) {//thread-per-client
    ClientArg *ca = (ClientArg *)arg;//typecasting pointer
    int cfd = ca->client_fd;
    free(ca);

    printf("[SERVER] Client connected.\n");

    int logged_in = 0;
    int session_role = -1;
    int session_uid = -1;
    char session_user[MAX_NAME] = {0};
    char session_token[32] = {0};

    Packet pkt;//stores incoming client's request
    while (g_running) {//continuously processes until server shots down or client disconnects
        ssize_t n = recv(cfd, &pkt, sizeof(pkt), 0);//reads from client socket
        if (n <= 0) break;   /* client disconnected */

        dispatch(cfd, &pkt,
                 &logged_in, &session_role, &session_uid,
                 session_user, session_token);
    }

    /* Auto-logout on disconnect */
    if (logged_in) {
        auth_logout(session_token);
        audit_log(session_user, "DISCONNECT_AUTO_LOGOUT");
    }

    close(cfd);
    sem_post(g_conn_sem);//increments by 1
    printf("[SERVER] Client disconnected.\n");
    return NULL;
}

/* ── IPC listener thread ─────────────────────────────────────── */

static void *ipc_listener_thread(void *arg) {//ipc monitoring thread
    (void)arg;
    while (g_running) {
        /* Poll the named semaphore for broadcast events */
        if (sem_trywait(g_notify_sem) == 0) {//semtrywait-decrement semaphore without blocking
            /* Drain all pending messages */
            IpcMessage msg;
            while (ipc_mq_recv(&msg) == OK) {//queue events
                printf("[IPC] Event recorded: %s\n", msg.mtext);
                fflush(stdout);//immediate printing
            }
        } else {
            usleep(100000);   /* 100 ms poll interval */
        }
    }
    return NULL;
}

/* ── main ────────────────────────────────────────────────────── */

int main(void) {
    /* Ensure data directory exists */
    mkdir("data", 0755);

    /* Signal handling */
    struct sigaction sa;//signal behavior
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;//run sighandler when signal arrives
    sigaction(SIGINT,  &sa, NULL);//control c
    sigaction(SIGTERM, &sa, NULL);//graceful termination requests
    signal(SIGPIPE, SIG_IGN);   /* ignore broken pipe-when server writes to disconnected socket */

    /* Init subsystems */
    auth_init();
    if (ipc_init() != OK) return 1;

    /* Named semaphore to cap concurrent clients */
    sem_unlink(CONN_SEM_NAME);//remove old named semaphores
    g_conn_sem = sem_open(CONN_SEM_NAME, O_CREAT | O_EXCL, 0666, MAX_CLIENTS);//connsemname-global sem name

    /* Start IPC listener thread */
    pthread_t ipc_tid;
    pthread_create(&ipc_tid, NULL, ipc_listener_thread, NULL);//independently watches events

    /* TCP server socket */
    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));//allow this port to be reused quickly in case of a crash/stop

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;//accept connections from any interface
    addr.sin_port = htons(PORT);//byte order conversion

    bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr));//associates socket with port
    listen(g_server_fd, MAX_CLIENTS);//listening server socket

    printf("[SERVER] Concurrent Expense Settlement System listening on port %d\n", PORT);

    /* Accept loop */
    while (g_running) {
        struct sockaddr_in cli_addr;//stores connecting client info
        socklen_t cli_len = sizeof(cli_addr);

        int cfd = accept(g_server_fd,
                         (struct sockaddr *)&cli_addr, &cli_len);//creates new socket for client
        if (cfd < 0) break;

        if (sem_trywait(g_conn_sem) != 0) {
            const char *busy = "Server busy.\n";
            send(cfd, busy, strlen(busy), 0);
            close(cfd);
            continue;
        }

        ClientArg *ca = malloc(sizeof(ClientArg));//dynamic memory for thread arguments
        ca->client_fd = cfd;

        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, ca);//starts client thread
        pthread_detach(tid);
    }

    ipc_teardown();
    return 0;
}