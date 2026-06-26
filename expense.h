#ifndef EXPENSE_H
#define EXPENSE_H

#include "common.h"
#include "filelock.h"
#include "ipc.h"

static pthread_mutex_t g_expense_mutex = PTHREAD_MUTEX_INITIALIZER;//for expense modification
static int get_user_idx(int user_id) { return user_id - 1; }

/**
 * expense_add: Validates unequal splits and updates the physical ledger and SHM matrix.
 * Enforces the mathematical constraint: Total Amount = Sum of individual splits.
 */
static int expense_add(int actor_role, int actor_uid, const char *actor_name, const char *description, double total_amount, const char *payload, char *msg, size_t msg_len) {
    (void)actor_name;
    if (auth_require_role(actor_role, ROLE_USER) != OK) { 
        snprintf(msg, msg_len, "ERROR: Guest restricted."); 
        return ERR; 
    }

    User users[MAX_USERS]; int user_count = 0; load_users(users, &user_count);
    int split_ids[MAX_USERS]; double split_amts[MAX_USERS]; int split_count = 0;
    
    // Track the sum of individual splits for validation
    double running_total = 0.0;

    char csv[512]; strncpy(csv, payload, sizeof(csv)-1);
    char *entry = strtok(csv, ",");//breaks the string wherever it sees a ,
    while (entry && split_count < MAX_USERS) {
        double amt; char uname[MAX_NAME];
        if (sscanf(entry, "%lf:%63s", &amt, uname) == 2) {//extract values from token and put it in vars
            for (int i = 0; i < user_count; i++) {
                if (users[i].active && strcmp(users[i].username, uname) == 0) {
                    split_ids[split_count] = users[i].id;
                    split_amts[split_count] = amt;
                    
                    // Accumulate split sum
                    running_total += amt; 
                    
                    split_count++;
                    break;
                }
            }
        }
        entry = strtok(NULL, ",");//start from the , you ended before
    }

    if (split_count == 0) { 
        snprintf(msg, msg_len, "ERROR: No valid splits."); 
        return ERR; 
    }

    // Validation Check: Sum of splits must equal the total expense amount
    if (total_amount > 0 && (running_total < total_amount - 0.01 || running_total > total_amount + 0.01)) {
        snprintf(msg, msg_len, "ERROR: Split sum ($%.2f) does not match Total ($%.2f).", 
                 running_total, total_amount);
        return ERR;
    }

    pthread_mutex_lock(&g_expense_mutex);//prevents simultaneous expense modification-lock
    FileLock fl; filelock_open(&fl); filelock_acquire(&fl, FL_WRITE);
    Expense exps[MAX_EXPENSES]; int exp_cnt = 0; expenses_load(exps, &exp_cnt);

    if (exp_cnt >= MAX_EXPENSES) {
        filelock_release(&fl); filelock_close(&fl);
        pthread_mutex_unlock(&g_expense_mutex);
        return ERR;
    }

    Expense *e = &exps[exp_cnt++];
    e->id = exp_cnt; 
    e->amount = total_amount; 
    e->paid_by = actor_uid;
    strncpy(e->description, description, MAX_DESC - 1);
    e->split_count = split_count; 
    e->settled = 0;
    e->timestamp = time(NULL);
    memcpy(e->split_among, split_ids, split_count * sizeof(int));//copy the array
    memcpy(e->split_amounts, split_amts, split_count * sizeof(double));

    expenses_save(exps, exp_cnt);
    filelock_release(&fl); filelock_close(&fl);
    pthread_mutex_unlock(&g_expense_mutex);

    // Update the P2P Debt Matrix in Shared Memory
    int idxs[MAX_USERS];
    for (int i = 0; i < split_count; i++) idxs[i] = get_user_idx(split_ids[i]);
    ipc_shm_update_custom(get_user_idx(actor_uid), idxs, split_amts, split_count, 0);

    // Signal update via Semaphore and Message Queue
    ipc_sem_post();
    snprintf(msg, msg_len, "OK: Custom expense #%d added.", exp_cnt);
    return OK;
}

/**
 * expense_settle: Marks an expense as settled and reverses the debt in the matrix.
 */
static int expense_settle(int actor_role, int actor_uid, const char *actor_name, int expense_id, char *msg, size_t msg_len) {
    (void)actor_name; (void)actor_uid;
    if (auth_require_role(actor_role, ROLE_USER) != OK) return ERR;

    pthread_mutex_lock(&g_expense_mutex);//lock
    FileLock fl; filelock_open(&fl); filelock_acquire(&fl, FL_WRITE);
    Expense exps[MAX_EXPENSES]; int exp_count = 0; expenses_load(exps, &exp_count);

    int idx = -1;
    for (int i = 0; i < exp_count; i++) if (exps[i].id == expense_id) { idx = i; break; }
    if (idx == -1 || exps[idx].settled) { 
        filelock_release(&fl); filelock_close(&fl); 
        pthread_mutex_unlock(&g_expense_mutex); 
        return ERR; 
    }

    exps[idx].settled = 1;
    expenses_save(exps, exp_count);
    filelock_release(&fl); filelock_close(&fl);
    pthread_mutex_unlock(&g_expense_mutex);

    // Reverse the debt in the SHM matrix
    int idxs[MAX_USERS];
    for (int i = 0; i < exps[idx].split_count; i++) idxs[i] = get_user_idx(exps[idx].split_among[i]);
    ipc_shm_update_custom(get_user_idx(exps[idx].paid_by), idxs, exps[idx].split_amounts, exps[idx].split_count, 1);

    ipc_sem_post();//generate notification
    snprintf(msg, msg_len, "OK: Expense #%d settled.", expense_id);
    return OK;
}

/**
 * balance_view: Implements debt transitivity to simplify the display.
 * Calculates net positions to bypass middlemen (A->B->C becomes A->C).
 */
static int balance_view(int actor_role, int actor_uid, char *out, size_t out_len) {
    User users[MAX_USERS]; int user_count = 0; load_users(users, &user_count);
    double net_balances[MAX_USERS] = {0};
    size_t pos = 0;

    pos += snprintf(out + pos, out_len - pos, "--- SIMPLIFIED SETTLEMENTS ---\n");
    pos += snprintf(out + pos, out_len - pos, "%-16s %-16s %-10s\n", "Debtor", "Creditor", "Amount");
    pos += snprintf(out + pos, out_len - pos, "--------------------------------------------\n");

    // 1. Calculate net balance for each user from the SHM matrix
    pthread_mutex_lock(&g_shm->lock);
    for (int i = 0; i < user_count; i++) {
        for (int j = 0; j < user_count; j++) {
            double amt = g_shm->debt_matrix[i][j];
            if (amt > 0.01) {
                net_balances[i] -= amt; // i owes money
                net_balances[j] += amt; // j is owed money
            }
        }
    }
    pthread_mutex_unlock(&g_shm->lock);

    // 2. Greedy Simplification Algorithm
    for (int i = 0; i < user_count; i++) {
        if (net_balances[i] >= -0.01) continue;//don't owe money then skip

        for (int j = 0; j < user_count; j++) {
            if (net_balances[j] <= 0.01) continue; 

            //select the smaller one between debtor need and creditors
            double settlement = (-net_balances[i] < net_balances[j]) ? -net_balances[i] : net_balances[j];
            
            // Authorization Check: Guests only see their own simplified debts
            if (actor_role == ROLE_GUEST && users[i].id != actor_uid && users[j].id != actor_uid) {
                continue;
            } else {
                pos += snprintf(out + pos, out_len - pos, "%-16s owes %-16s: $%.2f\n", 
                                users[i].username, users[j].username, settlement);
            }

            net_balances[i] += settlement;
            net_balances[j] -= settlement;

            if (net_balances[i] >= -0.01) break;
        }
    }
    return OK;
}

/**
 * expense_view: Reads the persistent ledger to show all transactions.
 */
static int expense_view(int actor_role, int actor_uid, char *out, size_t out_len) {
    (void)actor_role; (void)actor_uid;
    FileLock fl; filelock_open(&fl); filelock_acquire(&fl, FL_READ);
    Expense exps[MAX_EXPENSES]; int exp_count = 0; expenses_load(exps, &exp_count);
    filelock_release(&fl); filelock_close(&fl);
    
    size_t pos = 0;//current write position inside output buffer
    for (int i = 0; i < exp_count; i++) {
        pos += snprintf(out + pos, out_len - pos, "ID: %d | %-12s | %6.2f | %s\n", 
                        exps[i].id, exps[i].description, exps[i].amount, 
                        exps[i].settled ? "Settled" : "Pending");
    }
    return OK;
}

#endif