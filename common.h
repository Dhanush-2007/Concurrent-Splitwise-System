//common.h ensures that both the client and server speak the same language by defining structures
#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>//mem alloc
#include <string.h>
#include <unistd.h>//read write close posix api
#include <pthread.h>//threads
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/stat.h>//file permissions, metadata
#include <fcntl.h>
#include <semaphore.h>
#include <arpa/inet.h>//networking sockets, ip addresses
#include <time.h>
#include <signal.h>

#define PORT 8080
#define MAX_CLIENTS 10
#define MAX_USERS 10
#define MAX_EXPENSES 100
#define MAX_NAME 64
#define MAX_PASS 64
#define MAX_DESC 128
#define BUFFER_SIZE 4096

//defines where the data is stored in the harddrive
#define USERS_FILE    "data/users.dat"
#define EXPENSES_FILE "data/expenses.dat"
#define LOCK_FILE     "data/expenses.lock"
#define LOG_FILE      "data/audit.log"

#define SHM_KEY    0x1234
#define MSG_KEY    0x5678
#define SEM_NOTIFY "/expense_notify"//names semaphore to notify clients when new expense is added

#define OK  0
#define ERR 1

typedef enum { ROLE_ADMIN = 0, ROLE_USER = 1, ROLE_GUEST = 2 } Role;//user role
typedef enum { 
    OP_LOGIN, OP_LOGOUT, OP_ADD_EXPENSE, OP_SETTLE_EXPENSE, 
    OP_VIEW_EXPENSES, OP_VIEW_BALANCES, OP_ADD_USER, 
    OP_DELETE_USER, OP_VIEW_USERS, OP_VIEW_LOG 
} OpCode;//command list

typedef enum { 
    MSG_EXPENSE_ADDED = 1, 
    MSG_EXPENSE_SETTLED, 
    MSG_BALANCE_UPDATE 
} MsgType;//for ipc notifications

typedef struct {
    int id;
    char username[MAX_NAME];
    char password[MAX_PASS];
    int role;
    int active;
} User;//user structure

typedef struct {
    int id;
    char description[MAX_DESC];
    double amount;
    int paid_by;
    int split_among[MAX_USERS];
    double split_amounts[MAX_USERS]; 
    int split_count;
    int settled;
    time_t timestamp;
} Expense;//expense struicture

typedef struct {
    OpCode op;
    int user_id;
    char token[32];//security token to prove that they are logged in
    char payload[BUFFER_SIZE - 64];//actual data
} Packet;//client to server packets

typedef struct {
    int status;
    char message[BUFFER_SIZE - 8];
} Response;//server reply

typedef struct {
    long mtype;
    char mtext[256];
} IpcMessage;

typedef struct {
    pthread_mutex_t lock;
    double debt_matrix[MAX_USERS][MAX_USERS]; 
    int user_count;
} SharedBalance;

#define CLEAR(x) memset(&x, 0, sizeof(x))

#endif