#ifndef AUTH_H
#define AUTH_H

#include "common.h"

/* Simple token store (server-side, protected by mutex)*/
#define MAX_SESSIONS 32

typedef struct {
    char  token[32];//token for user instead of checking pswd every time
    int   user_id;
    int   role;
    time_t created;
} Session;

static Session g_sessions[MAX_SESSIONS];//tracks who is online
static int g_session_count = 0;
static pthread_mutex_t g_session_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── Helpers ────────────────────────────────────────────────── */

/* Generate a simple pseudo-random token */
static void generate_token(char *out, int user_id) {
    snprintf(out, 32, "TK%d%ld", user_id, (long)time(NULL));//creates unique string,snprintf-prevents buffer overflow
}

static void hash_password(const char *plain, char *hashed) {
    unsigned long h = 5381;
    const char *p = plain;
    while (*p) { h = ((h << 5) + h) ^ (unsigned char)(*p++); }//scrambles the pswd
    snprintf(hashed, MAX_PASS, "%lu", h);//converts numeric hash into string
}

/* ── User file I/O (protected by caller's file lock) ─────── */

static int load_users(User *users, int *count) {
    FILE *f = fopen(USERS_FILE, "rb");
    if (!f) { *count = 0; return ERR; }
    *count = (int)fread(users, sizeof(User), MAX_USERS, f);
    fclose(f);
    return OK;
}

static int save_users(const User *users, int count) {//writes users to file
    FILE *f = fopen(USERS_FILE, "wb");
    if (!f) return ERR;
    fwrite(users, sizeof(User), count, f);
    fclose(f);
    return OK;
}

/* ── Public API ─────────────────────────────────────────────── */

/*
 * auth_login – verify credentials, create session token.
 * Returns token string on success, NULL on failure.
 */
static const char *auth_login(const char *username, const char *password,
                               int *out_role, int *out_uid) {
    User users[MAX_USERS];//loaded users
    int  count = 0;
    char hashed[MAX_PASS];

    hash_password(password, hashed);
    load_users(users, &count);//reads users from file

    for (int i = 0; i < count; i++) {
        if (users[i].active &&
            strcmp(users[i].username, username) == 0 &&
            strcmp(users[i].password, hashed)   == 0) {

            pthread_mutex_lock(&g_session_lock);//lock

            /* Reuse slot if same user already logged in */
            for (int j = 0; j < g_session_count; j++) {
                if (g_sessions[j].user_id == users[i].id) {
                    generate_token(g_sessions[j].token, users[i].id);//generate a token
                    g_sessions[j].role = users[i].role;
                    g_sessions[j].created = time(NULL);
                    *out_role = users[i].role;
                    *out_uid  = users[i].id;
                    const char *tok = g_sessions[j].token;
                    pthread_mutex_unlock(&g_session_lock);
                    return tok;
                }
            }

            if (g_session_count < MAX_SESSIONS) {//if a new user is logging in it crates a new token
                Session *s = &g_sessions[g_session_count++];
                generate_token(s->token, users[i].id);
                s->user_id = users[i].id;
                s->role    = users[i].role;
                s->created = time(NULL);
                *out_role = users[i].role;
                *out_uid  = users[i].id;
                const char *tok = s->token;
                pthread_mutex_unlock(&g_session_lock);
                return tok;
            }

            pthread_mutex_unlock(&g_session_lock);
            return NULL;   /* session table full */
        }
    }
    return NULL;
}

/*
 * auth_validate – check token, fill role & uid.
 */
static int auth_validate(const char *token, int *out_role, int *out_uid) __attribute__((unused));//if unused no warning
static int auth_validate(const char *token, int *out_role, int *out_uid) {//checks everytime if its the correct user or not
    pthread_mutex_lock(&g_session_lock);//lock
    for (int i = 0; i < g_session_count; i++) {
        if (strcmp(g_sessions[i].token, token) == 0) {
            *out_role = g_sessions[i].role;
            *out_uid  = g_sessions[i].user_id;
            pthread_mutex_unlock(&g_session_lock);
            return OK;
        }
    }
    pthread_mutex_unlock(&g_session_lock);
    return ERR;
}

/*
 * auth_logout – invalidate token.
 */
static int auth_logout(const char *token) {
    pthread_mutex_lock(&g_session_lock);//lock
    for (int i = 0; i < g_session_count; i++) {
        if (strcmp(g_sessions[i].token, token) == 0) {
            /* Swap-remove */
            g_sessions[i] = g_sessions[--g_session_count];//take the last token and copy it directly - swap remove
            pthread_mutex_unlock(&g_session_lock);
            return OK;
        }
    }
    pthread_mutex_unlock(&g_session_lock);
    return ERR;
}

/*
 * auth_require_role – return OK only if actual_role <= required_role.
 * (lower number = higher privilege: ADMIN=0 > USER=1 > GUEST=2)
 */
static int auth_require_role(int actual_role, int required_role) {
    return (actual_role <= required_role) ? OK : ERR;
}

/*
 * auth_add_user – admin creates a new user (saves to file).
 */
static int auth_add_user(const char *username, const char *password, int role) {
    User users[MAX_USERS];
    int  count = 0;

    load_users(users, &count);

    /* Duplicate check */
    for (int i = 0; i < count; i++) {
        if (strcmp(users[i].username, username) == 0 && users[i].active)
            return ERR;
    }

    if (count >= MAX_USERS) return ERR;

    User *u = &users[count++];
    u->id = count;
    strncpy(u->username, username, MAX_NAME - 1);
    hash_password(password, u->password);
    u->role = role;
    u->active = 1;

    return save_users(users, count);
}

/*
 * auth_delete_user – admin soft-deletes a user.
 */
static int auth_delete_user(const char *username) {
    User users[MAX_USERS];
    int count = 0;

    load_users(users, &count);
    for (int i = 0; i < count; i++) {
        if (strcmp(users[i].username, username) == 0 && users[i].active) {
            users[i].active = 0;
            return save_users(users, count);
        }
    }
    return ERR;
}

/*
 * auth_init – seed default admin if users file is empty.
 */
static void auth_init(void) {
    User users[MAX_USERS];
    int  count = 0;
    load_users(users, &count);
    if (count == 0) {
        printf("[AUTH] No users found – creating default admin (admin/admin123)\n");
        auth_add_user("admin", "admin123", ROLE_ADMIN);
    }
}

#endif /* AUTH_H */
