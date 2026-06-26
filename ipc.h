//communication centre - handles live data
#ifndef IPC_H
#define IPC_H

#include "common.h"

static int g_shm_id = -1;// id to shared memory
static SharedBalance *g_shm  = NULL;//pointer to data in RAM-shm

static int ipc_shm_create(void) {//creates and attaches shared memory
    g_shm_id = shmget(SHM_KEY, sizeof(SharedBalance), IPC_CREAT | 0666);//creates/accesses shared memory
    if (g_shm_id == -1) return ERR;
    g_shm = (SharedBalance *)shmat(g_shm_id, NULL, 0);//attaches the shared memory to process address space
    //NULL-OS chooses the address,0-default attach mode
    if (g_shm == (void *)-1) return ERR;
    return OK;
}

static void ipc_shm_detach(void) {//disconnects process from shared memory
    if (g_shm) { shmdt(g_shm); g_shm = NULL; }//detaches,reset
}

static void ipc_shm_destroy(void) {
    ipc_shm_detach();//disconnect first
    if (g_shm_id != -1) { shmctl(g_shm_id, IPC_RMID, NULL); g_shm_id = -1; }
}

static void ipc_shm_update_custom(int paid_by_idx, int *split_idx, double *amounts, int split_count, int is_settling) {//debt matrix update function
    if (!g_shm) return;
    pthread_mutex_lock(&g_shm->lock);
    for (int i = 0; i < split_count; i++) {
        int debtor_idx = split_idx[i];
        if (debtor_idx == paid_by_idx) continue;
        
        if (is_settling)
            g_shm->debt_matrix[debtor_idx][paid_by_idx] -= amounts[i];
        else
            g_shm->debt_matrix[debtor_idx][paid_by_idx] += amounts[i];
    }
    pthread_mutex_unlock(&g_shm->lock);
}

static int g_mqid = -1;
static int ipc_mq_create(void) { g_mqid = msgget(MSG_KEY, IPC_CREAT | 0666); return (g_mqid == -1) ? ERR : OK; }
static void ipc_mq_destroy(void) { if (g_mqid != -1) msgctl(g_mqid, IPC_RMID, NULL); }//deletes queue from kernel,NULL-no need for permissions
static int ipc_mq_send(long mtype, const char *text) {
    if (g_mqid == -1) return ERR;
    IpcMessage msg; msg.mtype = mtype;//creating msg struct
    strncpy(msg.mtext, text, sizeof(msg.mtext)-1);
    return msgsnd(g_mqid, &msg, sizeof(msg.mtext), IPC_NOWAIT);//sends msg
}
static int ipc_mq_recv(IpcMessage *out) {
    if (g_mqid == -1) return ERR;
    return (msgrcv(g_mqid, out, sizeof(out->mtext), 0, IPC_NOWAIT) > 0) ? OK : ERR;//0-receive any msg type
}
static sem_t *g_notify_sem = SEM_FAILED;//named semaphore
static int ipc_sem_create(void) { g_notify_sem = sem_open(SEM_NOTIFY, O_CREAT, 0666, 0); return (g_notify_sem == SEM_FAILED) ? ERR : OK; }//0-initial sem value
static void ipc_sem_destroy(void) { if (g_notify_sem != SEM_FAILED) { sem_close(g_notify_sem); sem_unlink(SEM_NOTIFY); } }
static void ipc_sem_post(void) { if (g_notify_sem != SEM_FAILED) sem_post(g_notify_sem); }//increments semaphore value by 1

static int ipc_init(void) { return (ipc_shm_create() == OK && ipc_mq_create() == OK && ipc_sem_create() == OK) ? OK : ERR; }
static void ipc_teardown(void) { ipc_sem_destroy(); ipc_mq_destroy(); ipc_shm_destroy(); }

#endif