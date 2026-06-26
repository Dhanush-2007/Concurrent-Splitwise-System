//for concurrency control
#ifndef FILELOCK_H
#define FILELOCK_H

#include "common.h"

/*
 * filelock.h – Advisory POSIX file locking (fcntl).
 *
 * Two lock types:
 *   FL_READ  – shared   (multiple readers allowed, no writer)
 *   FL_WRITE – exclusive (only one writer, no readers)
 *
 * The caller opens the *lock file* (a separate sentinel file),
 * applies the lock, does file I/O on the data file, then releases.
 */

typedef enum { FL_READ = F_RDLCK, FL_WRITE = F_WRLCK } FlockType;

typedef struct {
    int fd;          /* fd of the lock file */
} FileLock;

/* Open (or create) the lock file and return a FileLock handle */
static int filelock_open(FileLock *fl) {
    fl->fd = open(LOCK_FILE, O_CREAT | O_RDWR, 0644);//file to coordinate safe multi-user access
    if (fl->fd < 0) {
        perror("[FILELOCK] open");
        return ERR;
    }
    return OK;
}

/* Acquire a read (shared) or write (exclusive) lock – blocks until granted */
static int filelock_acquire(FileLock *fl, FlockType type) {
    struct flock lk;//tells what kind of lock, where to lock
    memset(&lk, 0, sizeof(lk));//clears garbage values
    lk.l_type   = (short)type;//read/write
    lk.l_whence = SEEK_SET;//start of the file
    lk.l_start  = 0;
    lk.l_len    = 0;//lock the entire file

    if (fcntl(fl->fd, F_SETLKW, &lk) == -1) {//blocking lock
        perror("[FILELOCK] acquire");
        return ERR;
    }
    return OK;
}

/* Release the lock */
static int filelock_release(FileLock *fl) {
    struct flock lk;
    memset(&lk, 0, sizeof(lk));
    lk.l_type   = F_UNLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start  = 0;
    lk.l_len    = 0;

    if (fcntl(fl->fd, F_SETLK, &lk) == -1) {
        perror("[FILELOCK] release");
        return ERR;
    }
    return OK;
}

/* Close the lock file descriptor */
static void filelock_close(FileLock *fl) {
    if (fl->fd >= 0) { close(fl->fd); fl->fd = -1; }
}

/* ── Convenience wrappers for expense file I/O ───────────────── */

static int expenses_load(Expense *exps, int *count) {//responsible for convertung binary data into RAM
    FILE *f = fopen(EXPENSES_FILE, "rb");
    if (!f) { *count = 0; return ERR; }
    *count = (int)fread(exps, sizeof(Expense), MAX_EXPENSES, f);
    fclose(f);
    return OK;
}

static int expenses_save(const Expense *exps, int count) {//writes to the expenses
    FILE *f = fopen(EXPENSES_FILE, "wb");
    if (!f) return ERR;
    fwrite(exps, sizeof(Expense), count, f);
    fclose(f);
    return OK;
}

/* ── Audit log ───────────────────────────────────────────────── */

static void audit_log(const char *username, const char *action) {
    /* Use a write lock on the lock file before appending */
    int fd = open(LOG_FILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) return;

    struct flock lk;
    memset(&lk, 0, sizeof(lk));
    lk.l_type   = F_WRLCK;
    lk.l_whence = SEEK_SET;
    fcntl(fd, F_SETLKW, &lk);//short term write lock

    time_t now = time(NULL);
    char buf[256];
    char *ts = ctime(&now);//converts timestamp into readable string
    ts[strlen(ts)-1] = '\0';   /* strip newline */
    snprintf(buf, sizeof(buf), "[%s] USER=%s ACTION=%s\n", ts, username, action);
    write(fd, buf, strlen(buf));

    lk.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &lk);
    close(fd);
}

#endif /* FILELOCK_H */
