/*
 * Lab 4 - Scheduler
 *
 * IMPORTANT:
 * - Do NOT change print_log() format (autograder / TA diff expects exact output).
 * - Do NOT change the order of operations in the main tick loop.
 * - compile: $make
 *   run testcase: $./groupX_scheduler < test_input.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "queue.h"

/* ----------------------------
 * Global "hardware resources"
 * ---------------------------- */
int printers  = 2;
int scanners  = 1;
int modems    = 1;
int cd_drives = 2;

int memory           = 960;
int memory_real_time = 64;

/* ----------------------------
 * Ready queues
 * ---------------------------- */
queue_t rt_queue;
queue_t sub_queue;
queue_t user_queue[3];

#define MAX_PROCESSES 128

/* ----------------------------
 * Process state
 * ---------------------------- */
typedef enum {
    NEW, SUBMITTED, READY, RUNNING, TERMINATED
} proc_state_t;

typedef struct process {
    int pid;
    int arrival_time;
    int init_prio;
    int cpu_total;
    int mem_req;
    int printers;
    int scanners;
    int modems;
    int cds;
    int cpu_remain;
    int current_prio;
    proc_state_t state;
    int mem_start;
} process_t;

/* ----------------------------
 * Free block linked list (sorted by start address)
 * Used for First Fit memory allocation in user region [64, 1023].
 * ---------------------------- */
typedef struct free_block {
    int start;
    int size;
    struct free_block *next;
} free_block_t;

free_block_t *freelist;

/* =========================================================
 * Function prototypes
 * ========================================================= */
void memory_init(void);
void admit_process(void);
process_t *dispatch(process_t **cur_running_rt);
void run_process(process_t *p);
void post_run(process_t *p, process_t **cur_running_rt);
int  termination_check(int processNo, int process_count, process_t *cur_running_rt);

int  memory_can_allocate(int req_size);
int  memory_allocate(process_t *p);
int  memory_free(process_t *p);

int  resource_available(process_t *p);
void resource_occupy(process_t *p);
void resource_free(process_t *p);

void arrival(process_t *p);

/* =========================================================
 * LOG OUTPUT (DO NOT MODIFY)
 * ========================================================= */
void print_log(process_t *ready_process, int time) {
    if (ready_process == NULL) {
        printf("[t=%d] IDLE\n", time);
    } else {
        printf(
            "[t=%d] RUN PID=%d PR=%d CPU=%d MEM_ST=%d MEM=%d P=%d S=%d M=%d C=%d\n",
            time,
            ready_process->pid,
            ready_process->current_prio,
            ready_process->cpu_remain,
            ready_process->mem_start,
            ready_process->mem_req,
            ready_process->printers,
            ready_process->scanners,
            ready_process->modems,
            ready_process->cds
        );
    }
}

/* =========================================================
 * MEMORY MANAGER
 *
 * User memory region: [64, 1023] (960 MB).
 * Real-time processes always occupy [0, 63] (not managed by freelist).
 *
 * First Fit strategy:
 *   - Scan free blocks from lowest address.
 *   - Allocate from the LOW-ADDRESS end of the first fitting block.
 *   - On free, insert block in sorted order and coalesce adjacent blocks.
 *   - The RT region [0,63] is never merged with the user region.
 * ========================================================= */

void memory_init(void) {
    freelist        = (free_block_t *)malloc(sizeof(free_block_t));
    freelist->start = 64;   /* user region starts at 64 MB */
    freelist->size  = 960;  /* 960 MB available */
    freelist->next  = NULL;
}

int memory_can_allocate(int req_size) {
    free_block_t *blk = freelist;
    while (blk) {
        if (blk->size >= req_size) return 1;
        blk = blk->next;
    }
    return 0;
}

int memory_allocate(process_t *p) {
    free_block_t *blk  = freelist;
    free_block_t *prev = NULL;

    while (blk) {
        if (blk->size >= p->mem_req) {
            p->mem_start = blk->start;          /* allocate from low end */

            if (blk->size == p->mem_req) {
                /* Exact fit: remove block */
                if (prev) prev->next = blk->next;
                else       freelist  = blk->next;
                free(blk);
            } else {
                /* Split: shrink block from low end */
                blk->start += p->mem_req;
                blk->size  -= p->mem_req;
            }
            return 1;
        }
        prev = blk;
        blk  = blk->next;
    }
    return -1;
}

int memory_free(process_t *p) {
    int rel_start = p->mem_start;
    int rel_size  = p->mem_req;

    /* Find sorted insertion position */
    free_block_t *prev = NULL;
    free_block_t *cur  = freelist;
    while (cur && cur->start < rel_start) {
        prev = cur;
        cur  = cur->next;
    }

    /* Insert new block */
    free_block_t *newblk = (free_block_t *)malloc(sizeof(free_block_t));
    newblk->start = rel_start;
    newblk->size  = rel_size;
    newblk->next  = cur;
    if (prev) prev->next = newblk;
    else       freelist  = newblk;

    /* Coalesce with next block if adjacent (both in user region [64,..]) */
    if (newblk->next &&
        newblk->start + newblk->size == newblk->next->start &&
        newblk->start >= 64 && newblk->next->start >= 64) {
        free_block_t *nxt = newblk->next;
        newblk->size += nxt->size;
        newblk->next  = nxt->next;
        free(nxt);
    }

    /* Coalesce with previous block if adjacent (both in user region) */
    if (prev &&
        prev->start + prev->size == newblk->start &&
        prev->start >= 64 && newblk->start >= 64) {
        prev->size += newblk->size;
        prev->next  = newblk->next;
        free(newblk);
    }

    return 1;
}

/* =========================================================
 * RESOURCE MANAGER
 * Tracks available printers, scanners, modems, cd_drives.
 * ========================================================= */

int resource_available(process_t *p) {
    return (p->printers <= printers)  &&
           (p->scanners <= scanners)  &&
           (p->modems   <= modems)    &&
           (p->cds      <= cd_drives);
}

void resource_occupy(process_t *p) {
    printers  -= p->printers;
    scanners  -= p->scanners;
    modems    -= p->modems;
    cd_drives -= p->cds;
}

void resource_free(process_t *p) {
    printers  += p->printers;
    scanners  += p->scanners;
    modems    += p->modems;
    cd_drives += p->cds;
}

/* =========================================================
 * ARRIVAL
 * Real-time processes go to rt_queue with mem_start = 0.
 * User processes go to sub_queue to await admission.
 * ========================================================= */

void arrival(process_t *p) {
    p->state = SUBMITTED;
    if (p->init_prio == 0) {
        p->mem_start = 0;       /* RT always uses [0,63] */
        queue_push(&rt_queue, p);
    } else {
        queue_push(&sub_queue, p);
    }
}

/* =========================================================
 * ADMIT PROCESS
 * Scans sub_queue in FIFO order.
 * Admits head process only if BOTH memory AND all I/O resources
 * are simultaneously available.
 * Stops immediately if head cannot be admitted.
 * ========================================================= */

void admit_process(void) {
    while (!queue_empty(&sub_queue)) {
        process_t *head = queue_peek(&sub_queue);

        if (!memory_can_allocate(head->mem_req) || !resource_available(head)) {
            break; /* head blocked; no further admission this tick */
        }

        queue_pop(&sub_queue);
        memory_allocate(head);      /* sets head->mem_start */
        resource_occupy(head);

        head->state        = READY;
        head->current_prio = head->init_prio;
        queue_push(&user_queue[head->init_prio - 1], head);
    }
}

/* =========================================================
 * DISPATCH
 * 1. If an RT job is already running, continue with it.
 * 2. Else if rt_queue non-empty, start next RT job.
 * 3. Else pick head of highest-priority non-empty user queue.
 * 4. Return NULL if nothing to run (IDLE).
 * ========================================================= */

process_t *dispatch(process_t **cur_running_rt) {
    /* Continue current RT job */
    if (*cur_running_rt != NULL) {
        return *cur_running_rt;
    }

    /* Start next RT job */
    if (!queue_empty(&rt_queue)) {
        *cur_running_rt = queue_pop(&rt_queue);
        (*cur_running_rt)->state = RUNNING;
        return *cur_running_rt;
    }

    /* Highest-priority user job */
    for (int i = 0; i < 3; i++) {
        if (!queue_empty(&user_queue[i])) {
            process_t *p = queue_pop(&user_queue[i]);
            p->state = RUNNING;
            return p;
        }
    }

    return NULL; /* IDLE */
}

/* =========================================================
 * RUN PROCESS
 * Decrement cpu_remain by 1 for one tick.
 * ========================================================= */

void run_process(process_t *p) {
    if (p) p->cpu_remain--;
}

/* =========================================================
 * POST-RUN
 * If completed: terminate, release resources.
 * If RT and not done: stays in cur_running_rt for next tick.
 * If user and not done: demote priority (max 3), re-queue.
 *
 * Demotion: priority 1→2, 2→3, 3→3 (round-robin at bottom).
 * ========================================================= */

void post_run(process_t *p, process_t **cur_running_rt) {
    if (!p) return;

    if (p->cpu_remain == 0) {
        /* Completed */
        p->state = TERMINATED;
        if (p->init_prio == 0) {
            *cur_running_rt = NULL;  /* RT job done */
        } else {
            memory_free(p);
            resource_free(p);
        }
    } else {
        /* Not done */
        if (p->init_prio == 0) {
            /* RT: keep running next tick; cur_running_rt already set */
            p->state = READY;
        } else {
            /* User: demote and re-queue */
            if (p->current_prio < 3) p->current_prio++;
            p->state = READY;
            queue_push(&user_queue[p->current_prio - 1], p);
        }
    }
}

/* =========================================================
 * TERMINATION CHECK
 * Simulation ends when all processes have arrived and all
 * queues are empty and no RT job is running.
 * ========================================================= */

int termination_check(int processNo, int process_count, process_t *cur_running_rt) {
    return  processNo == process_count  &&
            cur_running_rt == NULL      &&
            queue_empty(&rt_queue)      &&
            queue_empty(&sub_queue)     &&
            queue_empty(&user_queue[0]) &&
            queue_empty(&user_queue[1]) &&
            queue_empty(&user_queue[2]);
}

/* =========================================================
 * MAIN (DO NOT CHANGE LOOP ORDER)
 * ========================================================= */

int main(void) {
    queue_init(&rt_queue);
    queue_init(&sub_queue);
    for (int i = 0; i < 3; i++) queue_init(&user_queue[i]);

    memory_init();

    process_t processes[MAX_PROCESSES];
    int process_count = 0;

    while (process_count < MAX_PROCESSES) {
        int a, p, cpu, mem, pr, sc, mo, cd;
        if (scanf("%d %d %d %d %d %d %d %d",
                  &a, &p, &cpu, &mem, &pr, &sc, &mo, &cd) != 8) break;

        processes[process_count].arrival_time = a;
        processes[process_count].init_prio    = p;
        processes[process_count].cpu_total    = cpu;
        processes[process_count].mem_req      = mem;
        processes[process_count].printers     = pr;
        processes[process_count].scanners     = sc;
        processes[process_count].modems       = mo;
        processes[process_count].cds          = cd;
        processes[process_count].pid          = process_count;
        processes[process_count].cpu_remain   = cpu;
        processes[process_count].current_prio = p;
        processes[process_count].state        = NEW;
        processes[process_count].mem_start    = 0;

        process_count++;
    }

    int processNo        = 0;
    process_t *cur_running_rt = NULL;

    for (int time = 0; ; time++) {
        /* 1) ARRIVAL */
        for (; processNo < process_count; processNo++) {
            if (processes[processNo].arrival_time == time) {
                arrival(&processes[processNo]);
            } else {
                break;
            }
        }

        /* 2) ADMIT */
        admit_process();

        /* 3) DISPATCH */
        process_t *ready_process = dispatch(&cur_running_rt);

        /* 4) RUN */
        run_process(ready_process);

        /* 5) PRINT */
        print_log(ready_process, time);

        /* 6) POST-RUN */
        post_run(ready_process, &cur_running_rt);

        if (termination_check(processNo, process_count, cur_running_rt)) break;
    }

    return 0;
}
