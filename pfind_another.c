#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <limits.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

// Tamir Yaari - 304842990 //

// Structs for 2 FIFO queues - one to store threads and another to store paths

typedef struct pathNode {
    char *path;
    struct pathNode *next;
} pathNode;

typedef struct pathQueue {
    pathNode *head, *tail;
    unsigned int size;
} pathQueue;

typedef struct threadNode {
    long tid;
    pthread_cond_t *cv;
    struct threadNode *next;
} threadNode;

typedef struct threadQueue {
    threadNode *head, *tail;
    unsigned int size;
} threadQueue;

// Queue functions for the 2 queues - create queue, create node, enqueue, dequeue, print queue (for debugging)

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
    q->size = 0;
    return q;
}

void threadNode_enQueue(threadQueue *q, threadNode *node) {
    q->size++;
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
    q->size--;
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
    q->size = 0;
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
    q->size++;
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
    q->size--;
    if (q->head == NULL)
        q->tail = NULL;
    temp->next = NULL;
    return temp;
}

long *thread_ids; // will store thread IDs
const char *st; // will store the search term
long num_of_threads;
pthread_mutex_t thread_mutex; // lock used in most critical sections
pthread_mutex_t path_queue_mutex; // lock used for enqueue / dequeue from paths queue
pthread_cond_t *cv_arr; // array of condition variables - one for each thread
pthread_cond_t all_threads_created;
atomic_long num_of_threads_created = 0;
threadQueue *sleeping_threads_queue;
pathQueue *path_queue;
atomic_int numFilesFound = 0; // to be printed eventually

int isDirectory(const char *path) {
    struct stat stat_buf;
    if (stat(path, &stat_buf) != 0)
        return 0;
    return S_ISDIR(stat_buf.st_mode);
}


void searchDirectory(long my_id) { // my_id previously used for debugging
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
    pathNode *p;
    threadNode *next_sleeping_thread;
    threadNode *my_node;

    if (path_queue->size == 0) {
        pthread_mutex_lock(&path_queue_mutex);
        my_node = newThreadNode(my_id, &cv_arr[my_id]);
        threadNode_enQueue(sleeping_threads_queue, my_node);
        if (num_of_threads == sleeping_threads_queue->size) {
//            printf("no paths left to search and all threads are sleeping - exit program!\n");
            printf("Done searching, found %d files\n", numFilesFound);
            exit(1);
        }
        printf("Thread %ld went to sleep. 3\n",my_id);
        pthread_cond_wait(&cv_arr[my_id], &path_queue_mutex);
        printf("Thread %ld went to woke up! 3\n",my_id);
        pthread_mutex_unlock(&path_queue_mutex);
    }
    pthread_mutex_lock(&path_queue_mutex);
    if (path_queue->size == 0) {
//        sleep(5);
//        for (long i = 0; i < num_of_threads; i++) {
//            pthread_cond_signal(&cv_arr[i]);
//        }
        return;
    }
    p = pathNode_deQueue(path_queue);
    pthread_mutex_unlock(&path_queue_mutex);
    if (p == NULL) {
        printf("WOULD GET SIGFAULT!\n");
        return;
    }
    dir = opendir(p->path);
    if (!dir) {
        printf("Directory %s: Permission denied.\n", p->path);
        return;
    }
//    printf("Thread %ld starting to search %s\n", my_id, p->path);
    while ((dp = readdir(dir)) != NULL) { // each dp is an entry in the directory
        entry_name = dp->d_name;
        if (strcmp(entry_name, ".") != 0 && strcmp(entry_name, "..") != 0) {
            strcpy(path, p->path);
            strcat(path, "/");
            strcat(path, entry_name);

            if (isDirectory(path)) {
                pthread_mutex_lock(&path_queue_mutex);
                pathNode_enQueue(path_queue, path, 0); // enqueuing to path queue
                pthread_mutex_unlock(&path_queue_mutex);
                pthread_mutex_lock(&thread_mutex);
                next_sleeping_thread = threadNode_deQueue(sleeping_threads_queue);
//                printf("Thread %ld found a new directory in path %s\n", my_id, path);
                if (next_sleeping_thread != NULL) {
//                    printf("Thread %ld waking up thread %ld \n", my_id, next_sleeping_thread->tid);
                    pthread_cond_signal(next_sleeping_thread->cv);
                } else {
//                    printf("There is a live thread that will take care of it!\n");
                }
                pthread_mutex_unlock(&thread_mutex);
            } else {
                found = strstr(entry_name, st); // entry found!
                if (found) {
                    strcpy(display_path, p->path);
                    strcat(display_path, "/");
                    strcat(display_path, entry_name);
                    printf("Thread %ld found ---- ", my_id);
                    printf("%s\n", display_path);
                    numFilesFound++;
                }
            }
        }
    }
//    printf("Thread %ld finished searching %s\n",my_id,p->path);
    closedir(dir);
    free(p);
}

void *search_thread_func(void *t) {
    long my_id = (long) t;
    int i;
    int pathQueueNotEmpty = 0;
    threadNode *next_thread;
    threadNode *my_node;

    pthread_mutex_lock(&thread_mutex);
    num_of_threads_created++;
    if (num_of_threads_created == num_of_threads) {
        pthread_cond_signal(&all_threads_created);
    }
    pthread_mutex_unlock(&thread_mutex);

    pthread_mutex_lock(&thread_mutex);
    printf("Thread %ld went to sleep. 1\n",my_id);
    pthread_cond_wait(&cv_arr[my_id], &thread_mutex);
    printf("Thread %ld woke up! 1\n",my_id);
    pthread_mutex_unlock(&thread_mutex);

    pthread_mutex_lock(&thread_mutex);
    pathQueueNotEmpty = path_queue->size > 0;
    pthread_mutex_unlock(&thread_mutex);

    while (1) {
        pthread_mutex_lock(&thread_mutex);
        pathQueueNotEmpty = path_queue->size > 0;
        pthread_mutex_unlock(&thread_mutex);
        while (pathQueueNotEmpty) {
            searchDirectory(my_id); // main search function
            pthread_mutex_lock(&thread_mutex);
            pathQueueNotEmpty = path_queue->size > 0;
            pthread_mutex_unlock(&thread_mutex);
        }
        pthread_mutex_lock(&thread_mutex);
        if (num_of_threads == sleeping_threads_queue->size) {
            printf("Done searching, found %d files\n", numFilesFound);
            pthread_mutex_unlock(&thread_mutex);
            exit(2); //TODO - change
        }
        my_node = newThreadNode(my_id, &cv_arr[my_id]);
        threadNode_enQueue(sleeping_threads_queue, my_node);
        printf("Thread %ld went to sleep. 2\n",my_id);
        pthread_cond_wait(&cv_arr[my_id], &thread_mutex);
        printf("Thread %ld woke up! 2\n",my_id);
        pthread_mutex_unlock(&thread_mutex);

//        pthread_mutex_lock(&thread_mutex);


//        pthread_mutex_unlock(&thread_mutex);
    }
}

void *driver_thread_func() { // Driver thread - this thread will initiate the search
    pthread_mutex_lock(&thread_mutex);
    threadNode *first_thread = threadNode_deQueue(sleeping_threads_queue);
    pthread_cond_wait(&all_threads_created, &thread_mutex);
//    printf("driver thread is up! waking up thread %ld\n",first_thread->tid);
    pthread_cond_signal(first_thread->cv);
    pthread_mutex_unlock(&thread_mutex);

    int i = 0;
    while (1) {
        sleep(1);
        i++;
        if (i == 3) {
            printThreadQueue(sleeping_threads_queue);
            break;
        }
    }

    pthread_exit(NULL);
}


//----------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    int i;
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

    pthread_t threads[num_of_threads + 1]; // one extra thread for the driver thread
    cv_arr = malloc(sizeof(pthread_cond_t) * num_of_threads);
    if (cv_arr == NULL) {
        perror("Memory allocation failed.");
        exit(1);
    }
    thread_ids = malloc(sizeof(long) * (num_of_threads + 1));
    if (thread_ids == NULL) {
        perror("Memory allocation failed.");
        exit(1);
    }
    sleeping_threads_queue = createThreadQueue();
    path_queue = createPathQueue();
    pathNode_enQueue(path_queue, startPath, 1);

    // Initialize mutex and condition variable objects
    pthread_mutex_init(&thread_mutex, NULL);
    pthread_mutex_init(&path_queue_mutex, NULL);

    for (i = 0; i < num_of_threads + 1; i++) {
        pthread_cond_init(&cv_arr[i], NULL);
        thread_ids[i] = i;
    }
    for (i = 0; i < num_of_threads; i++) {
        rc = pthread_create(&threads[i], NULL, search_thread_func, (void *) thread_ids[i]);
        if (rc) {
            printf("ERROR in pthread_create(): ""%s\n", strerror(rc));
            exit(1);
        }
    }
    for (i = 0; i < num_of_threads; i++) {
        threadNode *node = newThreadNode(thread_ids[i], &cv_arr[i]);
        threadNode_enQueue(sleeping_threads_queue, node);
    }
    rc = pthread_create(&threads[num_of_threads], NULL, driver_thread_func, NULL); // driver
    if (rc) {
        printf("ERROR in pthread_create(): ""%s\n", strerror(rc));
        exit(1);
    }
    // Wait for all threads to complete
    for (i = 0; i < num_of_threads + 1; i++) {
        rc = pthread_join(threads[i], NULL);
        if (rc) {
            printf("ERROR in pthread_join(): ""%s\n", strerror(rc));
            exit(1);
        }
    }
    // Clean up and exit
    pthread_mutex_destroy(&thread_mutex);
    pthread_mutex_destroy(&path_queue_mutex);
    for (i = 0; i < num_of_threads + 1; i++) {
        pthread_cond_destroy(&cv_arr[i]);
    }
    pthread_cond_destroy(&all_threads_created);
    printf("Done searching, found %d files\n", numFilesFound);
    pthread_exit(NULL);
}