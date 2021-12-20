#define _BSD_SOURCE // for usleep to get compiled on nova

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <limits.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Tamir Yaari - 304842990 //

// Structs for 2 FIFO queues - one to store threads and another to store paths

typedef struct pathNode {
    char *path;
    struct pathNode *next;
} pathNode;

typedef struct pathQueue {
    pathNode *head, *tail;
} pathQueue;

typedef struct threadNode {
    long tid;
    pthread_cond_t *cv;
    struct threadNode *next;
} threadNode;

typedef struct threadQueue {
    threadNode *head, *tail;
} threadQueue;

// Queue functions for the 2 queues - create queue, create node, enqueue, dequeue, get size, print queue (for debugging)

threadNode *newThreadNode(long tid, pthread_cond_t *cv) {
    threadNode *temp = malloc(sizeof(struct threadNode));
    if (temp == NULL) {
        perror("Memory allocation failed.");
        exit(1);
    }
    temp->tid = tid;
    temp->cv = cv;
    temp->next = NULL;
    return temp;
}

threadQueue *createThreadQueue() {
    threadQueue *q = malloc(sizeof(struct threadQueue));
    if (q == NULL) {
        perror("Memory allocation failed.");
        exit(1);
    }
    q->head = q->tail = NULL;
    return q;
}

void threadNode_enQueue(threadQueue *q, threadNode *node) {
    if (q->tail == NULL) {
        q->head = q->tail = node;
        return;
    }
    q->tail->next = node;
    q->tail = node;
    node->next = NULL;
}

threadNode *threadNode_deQueue(threadQueue *q) {
    if (q->head == NULL)
        return NULL;
    threadNode *temp = q->head;
    q->head = q->head->next;
    if (q->head == NULL)
        q->tail = NULL;
    temp->next = NULL;
    return temp;
}

void printThreadQueue(threadQueue *q) {
    threadNode *node = q->head;
    printf("sleeping_threads_queue:\n");
    while (node != NULL) {
        printf("%ld\n", node->tid);
        node = node->next;
    }
    printf("---\n");
}

pathNode *newPathNode(char *path, int is_main) {
    pathNode *temp = malloc(sizeof(struct pathNode));
    if (temp == NULL) {
        perror("Memory allocation failed.");
        if (is_main) {
            exit(1);
        }
        pthread_exit(NULL);
    }
    temp->path = path;
    temp->next = NULL;
    return temp;
}

pathQueue *createPathQueue() {
    pathQueue *q = malloc(sizeof(struct pathQueue));
    if (q == NULL) {
        perror("Memory allocation failed.");
        exit(1);
    }
    q->head = q->tail = NULL;
    return q;
}

void printPathQueue(pathQueue *q) {
    pathNode *node = q->head;
    printf("path_queue:\n");
    while (node != NULL) {
        printf("%s\n", node->path);
        node = node->next;
    }
    printf("---\n");
}

void pathNode_enQueue(pathQueue *q, char *path, int is_main) {
    char *node_path = malloc(sizeof(char) * PATH_MAX);
    if (node_path == NULL) {
        if (is_main) {
            exit(1);
        }
        pthread_exit(NULL);
    }
    strcpy(node_path, path);
    pathNode *temp = newPathNode(node_path, is_main);
    if (q->tail == NULL) {
        q->head = q->tail = temp;
        return;
    }
    q->tail->next = temp;
    q->tail = temp;
    temp->next = NULL;
}

pathNode *pathNode_deQueue(pathQueue *q) {
    if (q->head == NULL)
        return NULL;
    pathNode *temp = q->head;
    q->head = q->head->next;
    if (q->head == NULL)
        q->tail = NULL;
    temp->next = NULL;
    return temp;
}


int getPathQueueSize(pathQueue *q) {
    int cnt = 0;
    pathNode *head = q->head;
    while (head != NULL) {
        cnt++;
        head = head->next;
    }
    return cnt;
}

int getThreadQueueSize(threadQueue *q) {
    int cnt = 0;
    threadNode *head = q->head;
    while (head != NULL) {
        cnt++;
        head = head->next;
    }
    return cnt;
}

void freePathQueue(pathQueue *q) {
    pathNode* head = q->head;
    pathNode* temp = head;
    while (head != NULL) {
        head = head->next;
        free(temp->path);
        free(temp);
        temp = head;
    }
}

void freeThreadQueue(threadQueue *q) {
    threadNode* head = q->head;
    threadNode* temp = head;
    while (head != NULL) {
        head = head->next;
        free(temp);
        temp = head;
    }
}

const char *st; // will store the search term
long num_of_threads;
pthread_mutex_t thread_mutex; // lock used in most critical sections
pthread_mutex_t path_queue_mutex; // lock used for enqueue / dequeue from paths queue
pthread_mutex_t search_over_mutex; // lock used for when the search is over
pthread_cond_t *cv_arr; // array of condition variables - one for each thread
pthread_cond_t all_threads_created; // to be used to indicate all threads were created and search can begin
pthread_cond_t search_over_cv;
atomic_long num_of_threads_created = 0;
threadQueue *sleeping_threads_queue;
pathQueue *path_queue;
atomic_int numFilesFound = 0; // to be printed eventually
atomic_int search_over = 0;

int isDirectory(const char *path) {
    struct stat stat_buf;
    if (stat(path, &stat_buf) != 0)
        return 0;
    return S_ISDIR(stat_buf.st_mode);
}


void searchDirectory(long my_id,
                     char *search_path) { // Main searching function for the searching threads (my_id previously used for debugging)
    char *path = malloc(sizeof(char) * PATH_MAX);
    if (path == NULL) {
        perror("Memory allocation failed.");
        pthread_exit(NULL);
    }
    char *display_path = malloc(sizeof(char) * PATH_MAX);
    if (display_path == NULL) {
        perror("Memory allocation failed.");
        pthread_exit(NULL);
    }
    struct dirent *dp;
    DIR *dir;
    char *entry_name;
    char *found;
    threadNode *next_sleeping_thread;

    dir = opendir(search_path);
    if (!dir) {
        printf("Directory %s: Permission denied.\n", search_path);
        return;
    }

    while ((dp = readdir(dir)) != NULL) { // each dp is an entry in the directory
        entry_name = dp->d_name;
        if (strcmp(entry_name, ".") != 0 && strcmp(entry_name, "..") != 0) {
            strcpy(path, search_path);
            strcat(path, "/");
            strcat(path, entry_name);

            if (isDirectory(path)) {
                pthread_mutex_lock(&path_queue_mutex);
                pathNode_enQueue(path_queue, path, 0); // enqueuing to path queue
                pthread_mutex_unlock(&path_queue_mutex);
                pthread_mutex_lock(&thread_mutex);
                next_sleeping_thread = threadNode_deQueue(sleeping_threads_queue);
                pthread_mutex_unlock(&thread_mutex);
                if (next_sleeping_thread !=
                    NULL) { // if a directory was added and there is a sleeping thread waiting for work, wakes it up
                    pthread_cond_signal(next_sleeping_thread->cv);
                }
            } else {
                found = strstr(entry_name, st);
                if (found) { // found an entry containing the provided search term
                    strcpy(display_path, search_path);
                    strcat(display_path, "/");
                    strcat(display_path, entry_name);
                    printf("%s\n", display_path);
                    numFilesFound++;
                }
            }
        }
    }
    free(path);
    free(display_path);
    closedir(dir);
}

void *search_thread_func(void *t) { // Main function for searching threads
    long my_id = (long) t;
    int i;
    int pathQueueSize;
    int threadQueueSize;
    threadNode *my_node;
    pathNode *search_path;

    pthread_mutex_lock(&thread_mutex);
    num_of_threads_created++;
    pthread_cond_signal(&all_threads_created); // signals main thread that the thread was created;
    pthread_mutex_unlock(&thread_mutex);

    pthread_mutex_lock(&thread_mutex);
    pthread_cond_wait(&cv_arr[my_id], &thread_mutex); // waits for first awakening
    pthread_mutex_unlock(&thread_mutex);
    pthread_mutex_lock(&search_over_mutex);
    if (search_over) {
        pthread_mutex_unlock(&search_over_mutex);
        pthread_exit(NULL);
    }
    pthread_mutex_unlock(&search_over_mutex);

    while (1) {
        pthread_mutex_lock(&thread_mutex);
        pathQueueSize = getPathQueueSize(path_queue);
        pthread_mutex_unlock(&thread_mutex);
        while (pathQueueSize > 0) { // if the paths queue is not empty - dequeues a directory and searches it
            pthread_mutex_lock(&thread_mutex);
            search_path = pathNode_deQueue(path_queue);
            pthread_mutex_unlock(&thread_mutex);
            if (search_path != NULL) {
                searchDirectory(my_id, search_path->path); // main search function
                usleep(1); // allows other threads waiting for directories to participate - NOT busy waiting
            } else {
                break; // error in deque - if there are still directories, try again
            }
            pthread_mutex_lock(&thread_mutex);
            pathQueueSize = getPathQueueSize(path_queue);
            pthread_mutex_unlock(&thread_mutex);
        }
        pthread_mutex_lock(&thread_mutex);
        my_node = newThreadNode(my_id, &cv_arr[my_id]);
        threadNode_enQueue(sleeping_threads_queue,
                           my_node); // paths queue is empty - thread will either go to sleep or exit program (if needed)
        threadQueueSize = getThreadQueueSize(sleeping_threads_queue);
        if (num_of_threads == threadQueueSize) {
            pathQueueSize = getPathQueueSize(path_queue);
            if (pathQueueSize == 0) { // if all threads are sleeping and no more paths to search -> exit program
                search_over = 1; // signals all other threads that the search is over with this flag
                pthread_mutex_unlock(&thread_mutex);
                for (i = 0; i < num_of_threads; i++) {
                    pthread_cond_signal(&cv_arr[i]); // awakes other threads so that they notice flag change and exit
                }
                pthread_cond_signal(&search_over_cv); // signals main thread that search is over
                pthread_exit(NULL);
            }
        }
        pthread_cond_wait(&cv_arr[my_id],&thread_mutex); // path queue was empty when checked - will go to sleep and check again once awakened
        if (search_over) {
            pthread_mutex_unlock(&thread_mutex);
            pthread_exit(NULL);
        }
        pthread_mutex_unlock(&thread_mutex);
    }
}

int main(int argc, char *argv[]) {
    long i;
    int rc;
    char *startPath;
    char *ptr;
    DIR *dir;

    if (argc != 4) {
        perror("Invalid number of arguments.");
        exit(1);
    }
    startPath = argv[1];
    st = argv[2];
    num_of_threads = strtol(argv[3], &ptr, 10);

    dir = opendir(startPath);
    if (!dir) { // validate that given startPath can be searched
        perror("Search root directory given cannot be searched!");
        exit(1);
    }

    pthread_t threads[num_of_threads];
    cv_arr = malloc(sizeof(pthread_cond_t) * num_of_threads);
    if (cv_arr == NULL) {
        perror("Memory allocation failed.");
        exit(1);
    }
    sleeping_threads_queue = createThreadQueue();
    path_queue = createPathQueue();
    pathNode_enQueue(path_queue, startPath, 1);

    // Initialize mutex and condition variable objects
    pthread_mutex_init(&thread_mutex, NULL);
    pthread_mutex_init(&path_queue_mutex, NULL);
    pthread_mutex_init(&search_over_mutex, NULL);
    pthread_cond_init(&all_threads_created, NULL);
    pthread_cond_init(&search_over_cv, NULL);

    for (i = 0; i < num_of_threads; i++) {
        pthread_cond_init(&cv_arr[i], NULL);
    }
    for (i = 0; i < num_of_threads; i++) {
        rc = pthread_create(&threads[i], NULL, search_thread_func, (void *) i);
        if (rc) {
            printf("ERROR in pthread_create(): ""%s\n", strerror(rc));
            exit(1);
        }
    }
    for (i = 0; i < num_of_threads; i++) {
        threadNode *node = newThreadNode(i, &cv_arr[i]);
        threadNode_enQueue(sleeping_threads_queue, node);
    }

    pthread_mutex_lock(&thread_mutex);
    threadNode *first_thread = threadNode_deQueue(sleeping_threads_queue);
    pthread_mutex_unlock(&thread_mutex);
    while (num_of_threads_created < num_of_threads) {
        pthread_mutex_lock(&thread_mutex);
        pthread_cond_wait(&all_threads_created, &thread_mutex); // waits for threads to be created
        pthread_mutex_unlock(&thread_mutex);
    }
    // out of loop - all threads created - search can begin
    pthread_mutex_lock(&thread_mutex);
    pthread_cond_signal(first_thread->cv); // first thread starts searching (there is only one directory for now)
    pthread_mutex_unlock(&thread_mutex);

    pthread_mutex_lock(&search_over_mutex);
    pthread_cond_wait(&search_over_cv, &search_over_mutex); // main awaits for search to be over
    pthread_mutex_unlock(&search_over_mutex);

    // Wait for all threads to complete
    for (i = 0; i < num_of_threads; i++) {
        rc = pthread_join(threads[i], NULL);
        if (rc) {
            printf("ERROR in pthread_join(): ""%s\n", strerror(rc));
            exit(1);
        }
    }
    // Clean up and exit

    pthread_mutex_destroy(&thread_mutex);
    pthread_mutex_destroy(&path_queue_mutex);
    pthread_mutex_destroy(&search_over_mutex);

    for (i = 0; i < num_of_threads; i++) {
        pthread_cond_destroy(&cv_arr[i]);
    }
    pthread_cond_destroy(&all_threads_created);
    pthread_cond_destroy(&search_over_cv);

    freePathQueue(path_queue);
    freeThreadQueue(sleeping_threads_queue);
    free(path_queue);
    free(sleeping_threads_queue);
    free(cv_arr);
    
    printf("Done searching, found %d files\n", numFilesFound);
    exit(0);
}