#ifndef MIMUW_FORK_EXECUTOR_H
#define MIMUW_FORK_EXECUTOR_H

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <limits.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>

#include "utils.h"
#include "err.h"

#define MAX_N_TASKS 4096
#define MAX_LINE_LEN 1023

struct EndedMessage {
    bool flag1;
    int exit_code;
    uint16_t task_id;
    pthread_t direction;
};

struct DirectMessage {
    uint16_t task_id;
    pthread_t direction;
};

struct Task {
    uint16_t task_id;
    pid_t pid;
    pthread_t direction;
    pthread_t out_reader;
    pthread_t err_reader;
    char out_buffer[MAX_LINE_LEN];
    char err_buffer[MAX_LINE_LEN];
    sem_t out_mutex;
    sem_t err_mutex;
    char **private_args;
    bool transfer;
};

struct ReaderArg {
    int fd[2];
    char *buf;
    sem_t *mutex;
};

int created_tasks = 0;
struct Task tasks[MAX_N_TASKS];
struct EndedMessage end_message[MAX_N_TASKS];
int end_message_counter = 0;
sem_t end_mutex;
int finished_process = 0;
sem_t process_mutex;
sem_t print_mutex;
sem_t thread_mutex;
bool processing_command = false;
pthread_barrier_t barrier;
bool flag = false;

struct DirectMessage prev_direction = {.direction = -1, .task_id = -1};

/* Methods to help main look prettier */
void init() {
    ASSERT_SYS_OK(sem_init(&process_mutex, 0, 1));
    ASSERT_SYS_OK(pthread_barrier_init(&barrier, NULL, 2));
    ASSERT_SYS_OK(sem_init(&thread_mutex, 0, 1));
    ASSERT_SYS_OK(sem_init(&print_mutex, 0, 1));
    ASSERT_SYS_OK(sem_init(&end_mutex, 0, 1));

    for (int i = 0; i < MAX_N_TASKS; ++i) {
        struct Task *task = &tasks[i];
        task->transfer = false;
        task->task_id = i;
        memset(task->out_buffer, '\0', MAX_LINE_LEN);
        memset(task->err_buffer, '\0', MAX_LINE_LEN);
        ASSERT_SYS_OK(sem_init(&task->out_mutex, 0, 1));
        ASSERT_SYS_OK(sem_init(&task->err_mutex, 0, 1));
    }
}

void safety(bool status) {
    ASSERT_SYS_OK(sem_wait(&thread_mutex));
    if (status && prev_direction.direction != -1) {
        ASSERT_SYS_OK(pthread_join(prev_direction.direction, NULL));
        tasks[prev_direction.task_id].transfer = true;
        prev_direction.direction = -1;
        prev_direction.task_id = -1;
    }
    processing_command = status;
    ASSERT_SYS_OK(sem_post(&thread_mutex));
}

/* Methods to run programs */
void *output_manager(void *arg) {
    struct ReaderArg* priv_arg = arg;
    char local_buf[MAX_LINE_LEN];
    FILE *in_stream = fdopen(priv_arg->fd[0], "r");

    while (read_line(local_buf, MAX_LINE_LEN, in_stream)) {
        ASSERT_SYS_OK(sem_wait(priv_arg->mutex));
        memcpy(priv_arg->buf, local_buf, MAX_LINE_LEN);
        ASSERT_SYS_OK(sem_post(priv_arg->mutex));
    }
    ASSERT_SYS_OK(fclose(in_stream));
    return 0;
}

void *run(void *arg) {
    uint16_t task_id = *(uint16_t *)arg;
    struct Task *task = &tasks[task_id];
    char **task_args = task->private_args + 1;

    int fderr[2];
    int fdout[2];
    ASSERT_SYS_OK(pipe(fderr));
    ASSERT_SYS_OK(pipe(fdout));
    set_close_on_exec(fderr[0], true);
    set_close_on_exec(fderr[1], true);
    set_close_on_exec(fdout[0], true);
    set_close_on_exec(fdout[1], true);

    struct ReaderArg out_reader_arg = {.mutex = &task->out_mutex, .buf = task->out_buffer, .fd = {fdout[0], fdout[1]}};
    ASSERT_SYS_OK(pthread_create(&task->out_reader, NULL, output_manager, &out_reader_arg));
    struct ReaderArg err_reader_arg = {.mutex = &task->err_mutex, .buf = task->err_buffer, .fd = {fderr[0], fderr[1]}};
    ASSERT_SYS_OK(pthread_create(&task->err_reader, NULL, output_manager, &err_reader_arg));

    task->pid = fork();
    ASSERT_SYS_OK(task->pid);
    if (!task->pid) {
        ASSERT_SYS_OK(dup2(fdout[1], STDOUT_FILENO));
        ASSERT_SYS_OK(dup2(fderr[1], STDERR_FILENO));
        ASSERT_SYS_OK(execvp(task_args[0], task_args));
        exit(1);
    }
    free_split_string(task->private_args);
    ASSERT_SYS_OK(close(fdout[1]));
    ASSERT_SYS_OK(close(fderr[1]));
    printf("Task %d started: pid %d.\n", task_id, task->pid);

    int s = pthread_barrier_wait(&barrier);
    if (s != 0 && s != PTHREAD_BARRIER_SERIAL_THREAD) fatal("pthread barrier");
    int status;
    ASSERT_SYS_OK(waitpid(task->pid, &status, 0));
    ASSERT_SYS_OK(sem_wait(&process_mutex));
    finished_process++;
    if (finished_process == 1) ASSERT_SYS_OK(sem_wait(&thread_mutex));
    ASSERT_SYS_OK(sem_post(&process_mutex));

    ASSERT_SYS_OK(pthread_join(task->out_reader, NULL));
    ASSERT_SYS_OK(pthread_join(task->err_reader, NULL));
    bool signalled = false;
    int exit_code = -1;
    if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
    else signalled = true;

    if (processing_command) {
        ASSERT_SYS_OK(sem_wait(&end_mutex));
        assert(end_message_counter < MAX_N_TASKS);
        struct EndedMessage *end_msg = &end_message[end_message_counter++];
        end_msg->task_id = task_id;
        end_msg->flag1 = signalled;
        end_msg->exit_code = exit_code;
        end_msg->direction = task->direction;
        ASSERT_SYS_OK(sem_post(&end_mutex));
    } else {
        ASSERT_SYS_OK(sem_wait(&print_mutex));
        if (prev_direction.direction != -1 && !tasks[prev_direction.task_id].transfer) {
            ASSERT_SYS_OK(pthread_join(prev_direction.direction, NULL));
            tasks[prev_direction.task_id].transfer = true;
        }
        if (signalled) printf("Task %d ended: flag1.\n", task_id);
        else printf("Task %d ended: status %d.\n", task_id, exit_code);

        prev_direction.direction = task->direction;
        prev_direction.task_id = task_id;
        ASSERT_SYS_OK(sem_post(&print_mutex));
    }
    ASSERT_SYS_OK(sem_wait(&process_mutex));
    finished_process--;
    if (finished_process == 0) {
        if (processing_command) {
            prev_direction.direction = -1;
            prev_direction.task_id = -1;
        }
        ASSERT_SYS_OK(sem_post(&thread_mutex));
    }
    ASSERT_SYS_OK(sem_post(&process_mutex));
    return NULL;
}

void runner(char ** args) {
    struct Task* task = &tasks[created_tasks++];
    task->private_args = args;
    ASSERT_SYS_OK(pthread_create(&task->direction, NULL, run, &task->task_id));
    int s = pthread_barrier_wait(&barrier);
    if (s != 0 && s != PTHREAD_BARRIER_SERIAL_THREAD) fatal("pthread barrier");
}

/* Methods for (out or err) */
void print_line(sem_t *mutex_, uint16_t id, const char * line, const char *source) {
    ASSERT_SYS_OK(sem_wait(mutex_));
    printf("Task %d %s: '%s'.\n", id, source, line);
    ASSERT_SYS_OK(sem_post(mutex_));
}

void printer(char ** args) {
    uint16_t task_id = (uint16_t) strtol(args[1], NULL, 10);
    struct Task *task = &tasks[task_id];
    if (!strcmp(*args, "out"))
        print_line(&task->out_mutex, task->task_id, task->out_buffer, "stdout");
    else if (!strcmp(*args, "err"))
        print_line(&task->err_mutex, task->task_id, task->err_buffer, "stderr");
}
/* Method to take a nap */
void nap(char ** args) {
    assert(args[1]);
    long long nap_time = strtoll(args[1], NULL, 10);
    usleep(nap_time * 1000);
}

/* Method for killing single task */
void killer(char ** args) {
    assert(args[1]);
    uint16_t task_id = (uint16_t) strtol(args[1], NULL, 10);
    kill(tasks[task_id].pid, SIGINT);
}

/* Methods for quiting */
void kill_tasks() {
    for (int i = 0; i < created_tasks; i++) {
        struct Task *task = &tasks[i];
        kill(task->pid, SIGKILL);
        if (!task->transfer) {
            ASSERT_SYS_OK(pthread_join(task->direction, NULL));
            task->transfer = true;
        }
    }
}

void destroy() {
    for (int i = 0; i < MAX_N_TASKS; ++i) {
        struct Task *task = &tasks[i];
        ASSERT_SYS_OK(sem_destroy(&task->out_mutex));
        ASSERT_SYS_OK(sem_destroy(&task->err_mutex));
    }
    ASSERT_SYS_OK(sem_destroy(&process_mutex));
    ASSERT_SYS_OK(pthread_barrier_destroy(&barrier));
    ASSERT_SYS_OK(sem_destroy(&thread_mutex));
    ASSERT_SYS_OK(sem_destroy(&print_mutex));
    ASSERT_SYS_OK(sem_destroy(&end_mutex));
}

void quit(char ** args) {
    flag = true;
    free_split_string(args);
}

#endif //MIMUW_FORK_EXECUTOR_H
