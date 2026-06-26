#include "common.h"

/* ── Named pipe paths (IPC / Pipes demo) ──────────────────────── */
#define NOTIFY_PIPE "/tmp/expense_notify.fifo"

static int g_server_fd   = -1;
static int g_logged_in   = 0;
static int g_role        = -1;
static int g_uid         = -1;
static char g_token[32]  = {0};
static char g_username[MAX_NAME] = {0};

/* ── Utility ──────────────────────────────────────────────────── */

static void clear_screen(void) {
    printf("\033[2J\033[H");
    fflush(stdout);
}

static void pause_prompt(void) {
    printf("\n[Press ENTER to continue]");
    fflush(stdout);
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {}
}

static void send_packet(OpCode op, const char *payload) {
    Packet pkt;
    CLEAR(pkt);
    pkt.op = op;
    pkt.user_id = g_uid;
    strncpy(pkt.token, g_token, 31);
    if (payload)
        strncpy(pkt.payload, payload, sizeof(pkt.payload) - 1);
    send(g_server_fd, &pkt, sizeof(pkt), 0);
}

static int recv_response(char *msg, size_t len) {
    Response r;
    ssize_t  n = recv(g_server_fd, &r, sizeof(r), 0);
    if (n <= 0) {
        snprintf(msg, len, "Connection lost.");
        return 1;
    }
    strncpy(msg, r.message, len - 1);
    msg[len-1] = '\0';
    return r.status;
}

/* ── Named-pipe notification thread ─────────────────────────── */

static volatile int g_pipe_running = 1;

static void *pipe_reader_thread(void *arg) {//listens for notifications
    (void)arg;
    mkfifo(NOTIFY_PIPE, 0666);//named pipe
    int fd = open(NOTIFY_PIPE, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return NULL;

    char buf[256];
    while (g_pipe_running) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("\n[NOTIFICATION] %s\n", buf);
            fflush(stdout);
        }
        usleep(200000);
    }
    close(fd);
    unlink(NOTIFY_PIPE);
    return NULL;
}

/* ── Login ────────────────────────────────────────────────────── */

static void do_login(void) {
    char uname[MAX_NAME], pass[MAX_PASS], payload[MAX_NAME + MAX_PASS + 2];
    printf("Username: "); fflush(stdout);
    if (!fgets(uname, sizeof(uname), stdin)) return;
    uname[strcspn(uname, "\n")] = '\0';

    printf("Password: "); fflush(stdout);
    if (!fgets(pass, sizeof(pass), stdin)) return;
    pass[strcspn(pass, "\n")] = '\0';

    snprintf(payload, sizeof(payload), "%s %s", uname, pass);
    send_packet(OP_LOGIN, payload);

    char msg[BUFFER_SIZE - 8];
    int rc = recv_response(msg, sizeof(msg));
    printf("%s\n", msg);

    if (rc == 0) {
        g_logged_in = 1;
        strncpy(g_username, uname, MAX_NAME - 1);
        if (strstr(msg, "Role: admin"))      g_role = ROLE_ADMIN;
        else if (strstr(msg, "Role: user"))  g_role = ROLE_USER;
        else                                  g_role = ROLE_GUEST;
        char *tp = strstr(msg, "Token: ");
        if (tp) strncpy(g_token, tp + 7, 31);
    }
}

/* ── Expense operations ───────────────────────────────────────── */

// UPDATED: Logic to support custom split amounts[cite: 10]
static void do_add_expense(void) {
    char desc[MAX_DESC], split_payload[512], amt_str[32], payload[BUFFER_SIZE - 64];

    printf("Description: "); fflush(stdout);
    if (!fgets(desc, sizeof(desc), stdin)) return;
    desc[strcspn(desc, "\n")] = '\0';

    printf("Total Amount: "); fflush(stdout);
    if (!fgets(amt_str, sizeof(amt_str), stdin)) return;
    amt_str[strcspn(amt_str, "\n")] = '\0';

    printf("Enter splits as 'amount:user,amount:user' (include yourself if applicable): ");
    fflush(stdout);
    if (!fgets(split_payload, sizeof(split_payload), stdin)) return;
    split_payload[strcspn(split_payload, "\n")] = '\0';

    /* Format: "total_amount|description|amount1:user1,amount2:user2" */
    snprintf(payload, sizeof(payload), "%s|%s|%s", amt_str, desc, split_payload);
    send_packet(OP_ADD_EXPENSE, payload);

    char msg[BUFFER_SIZE - 8];
    recv_response(msg, sizeof(msg));
    printf("%s\n", msg);
}

static void do_settle_expense(void) {
    char id_str[16];
    printf("Expense ID to settle: "); fflush(stdout);
    if (!fgets(id_str, sizeof(id_str), stdin)) return;
    id_str[strcspn(id_str, "\n")] = '\0';
    send_packet(OP_SETTLE_EXPENSE, id_str);

    char msg[BUFFER_SIZE - 8];
    recv_response(msg, sizeof(msg));
    printf("%s\n", msg);
}

static void do_view_expenses(void) {
    send_packet(OP_VIEW_EXPENSES, "");
    char msg[BUFFER_SIZE - 8];
    recv_response(msg, sizeof(msg));
    printf("%s\n", msg);
}

static void do_view_balances(void) {
    send_packet(OP_VIEW_BALANCES, "");
    char msg[BUFFER_SIZE - 8];
    recv_response(msg, sizeof(msg));
    printf("%s\n", msg);
}

/* ── Admin operations ─────────────────────────────────────────── */

static void do_add_user(void) {
    char uname[MAX_NAME], pass[MAX_PASS], role_str[4], payload[256];
    printf("New username: "); fflush(stdout);
    if (!fgets(uname, sizeof(uname), stdin)) return;
    uname[strcspn(uname, "\n")] = '\0';

    printf("Password: "); fflush(stdout);
    if (!fgets(pass, sizeof(pass), stdin)) return;
    pass[strcspn(pass, "\n")] = '\0';

    printf("Role [0=admin, 1=user, 2=guest]: "); fflush(stdout);
    if (!fgets(role_str, sizeof(role_str), stdin)) return;
    role_str[strcspn(role_str, "\n")] = '\0';

    snprintf(payload, sizeof(payload), "%s %s %s", uname, pass, role_str);
    send_packet(OP_ADD_USER, payload);

    char msg[BUFFER_SIZE - 8];
    recv_response(msg, sizeof(msg));
    printf("%s\n", msg);
}

static void do_delete_user(void) {
    char uname[MAX_NAME];
    printf("Username to delete: "); fflush(stdout);
    if (!fgets(uname, sizeof(uname), stdin)) return;
    uname[strcspn(uname, "\n")] = '\0';
    send_packet(OP_DELETE_USER, uname);

    char msg[BUFFER_SIZE - 8];
    recv_response(msg, sizeof(msg));
    printf("%s\n", msg);
}

static void do_view_users(void) {
    send_packet(OP_VIEW_USERS, "");
    char msg[BUFFER_SIZE - 8];
    recv_response(msg, sizeof(msg));
    printf("%s\n", msg);
}

static void do_view_log(void) {
    send_packet(OP_VIEW_LOG, "");
    char msg[BUFFER_SIZE - 8];
    recv_response(msg, sizeof(msg));
    printf("%s\n", msg);
}

/* ── Menus ────────────────────────────────────────────────────── */

static void print_banner(void) {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║   Concurrent Expense Settlement System               ║\n");
    printf("║   EGC 301P – Operating Systems Lab Mini Project      ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
}

static void print_main_menu(void) {
    print_banner();
    if (g_logged_in) {
        const char *role_str =
            (g_role == ROLE_ADMIN) ? "Admin" :
            (g_role == ROLE_USER)  ? "User"  : "Guest";
        printf("  Logged in as: %s  [%s]\n\n", g_username, role_str);
    }

    printf("  [1] Add Expense\n");
    printf("  [2] Settle Expense\n");
    printf("  [3] View Expenses\n");
    printf("  [4] View Balances\n");
    if (g_role == ROLE_ADMIN) {
        printf("  [5] Add User (Admin)\n");
        printf("  [6] Delete User (Admin)\n");
        printf("  [7] View All Users (Admin)\n");
        printf("  [8] View Audit Log (Admin)\n");
    }
    printf("  [0] Logout & Exit\n");
    printf("\nChoice: ");
    fflush(stdout);
}

/* ── main ─────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    int         port = (argc > 2) ? atoi(argv[2]) : PORT;

    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &srv.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", host);
        return 1;
    }

    if (connect(g_server_fd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("connect");
        return 1;
    }
    printf("[CLIENT] Connected to server at %s:%d\n\n", host, port);

    pthread_t pipe_tid;
    g_pipe_running = 1;
    pthread_create(&pipe_tid, NULL, pipe_reader_thread, NULL);

    clear_screen();
    print_banner();
    printf("\n  Please log in.\n\n");
    do_login();
    if (!g_logged_in) {
        g_pipe_running = 0;
        pthread_join(pipe_tid, NULL);
        close(g_server_fd);
        return 1;
    }

    char choice[8];
    while (g_logged_in) {
        clear_screen();
        print_main_menu();

        if (!fgets(choice, sizeof(choice), stdin)) break;
        int c = atoi(choice);

        switch (c) {
        case 1: do_add_expense();    pause_prompt(); break;
        case 2: do_settle_expense(); pause_prompt(); break;
        case 3: do_view_expenses();  pause_prompt(); break;
        case 4: do_view_balances();  pause_prompt(); break;
        case 5:
            if (g_role == ROLE_ADMIN) { do_add_user();    pause_prompt(); }
            break;
        case 6:
            if (g_role == ROLE_ADMIN) { do_delete_user(); pause_prompt(); }
            break;
        case 7:
            if (g_role == ROLE_ADMIN) { do_view_users();  pause_prompt(); }
            break;
        case 8:
            if (g_role == ROLE_ADMIN) { do_view_log();    pause_prompt(); }
            break;
        case 0:
            send_packet(OP_LOGOUT, "");
            g_logged_in = 0;
            break;
        default:
            printf("Invalid choice.\n");
            break;
        }
    }

    g_pipe_running = 0;
    pthread_join(pipe_tid, NULL);
    close(g_server_fd);
    printf("Goodbye!\n");
    return 0;
}