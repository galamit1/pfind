#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


#define SUCCESS 0
#define ERROR 1

#define FALSE 0

pthread_mutex_t dir_queue_lock;
pthread_mutex_t threads_queue_lock;
pthread_mutex_t threads_counter_lock;
pthread_mutex_t cv_list_lock;

int thread_number;
int threads_counter;
pthread_t *threads;

atomic_int files_matched_counter = 0;
atomic_int working_threads_counter = 0;
atomic_int threads_failed_counter = 0;

pthread_cond_t *threads_cv;
pthread_cond_t all_threads_created;

char *search_term;


// --------------------------Queue functions--------------------------

typedef struct Node {
    int thread_id;
    char *path;
    struct Node *next;
} Node;

typedef struct queue {
    Node *head;
    Node *tail;
} Queue;


Node *create_node(char *path) {
    Node *new_node = (Node *) malloc(sizeof(Node));
    if (new_node == NULL) {
        return NULL;
    }
    new_node->path = path;
    new_node->thread_id = 0;
    new_node->next = NULL;
    return new_node;
}

Node *create_thread_node(int thread_id) {
    Node *new_node = (Node *) malloc(sizeof(Node));
    if (new_node == NULL) {
        return NULL;
    }
    new_node->thread_id = thread_id;
    new_node->path = NULL;
    new_node->next = NULL;
    return new_node;
}

Queue *create_queue() {
    Queue *new_queue = (Queue *) malloc(sizeof(Queue));
    if (new_queue == NULL) {
        return NULL;
    }
    new_queue->head = NULL;
    new_queue->tail = NULL;
    return new_queue;
}

int push(Queue *queue, char *path) {
    Node *new_node;
    new_node = create_node(path);
    if (new_node == NULL) {
        return ERROR;
    }

    pthread_mutex_lock(&dir_queue_lock);
    if (queue->head == NULL) {
        queue->head = new_node;
        queue->tail = new_node;
    } else {
        queue->tail->next = new_node;
        queue->tail = new_node;
    }
    pthread_mutex_unlock(&dir_queue_lock);
     return SUCCESS;
}

int push_thread(Queue *queue, int thread_id) {
    Node *new_node;
    new_node = create_thread_node(thread_id);

    if (new_node == NULL) {
        return ERROR;
    }

    pthread_mutex_lock(&threads_queue_lock);
    if (queue->head == NULL) {
        queue->head = new_node;
        queue->tail = new_node;
    } else {
        queue->tail->next = new_node;
        queue->tail = new_node;
    }
    pthread_mutex_unlock(&threads_queue_lock);
    return SUCCESS;
}

char *pop(Queue *queue) {
    Node *old_head_node;
    char *head_node_path;

    pthread_mutex_lock(&dir_queue_lock);
    if (queue->head == NULL) {
        pthread_mutex_unlock(&dir_queue_lock);
        return NULL;
    }
    old_head_node = queue->head;
    queue->head = queue->head->next;
    if (queue->head == NULL) {
        queue->tail = NULL;
    }
    pthread_mutex_unlock(&dir_queue_lock);

    head_node_path = old_head_node->path;
    free(old_head_node);
    return head_node_path;
}


int pop_thread_id(Queue *queue) {
    Node *old_head_node;
    int head_node_thread_id;

    pthread_mutex_lock(&threads_queue_lock);
    if (queue->head == NULL) {
        pthread_mutex_unlock(&threads_queue_lock);
        return -1;
    }
    old_head_node = queue->head;
    queue->head = queue->head->next;
    if (queue->head == NULL) {
        queue->tail = NULL;
    }
    pthread_mutex_unlock(&threads_queue_lock);

    head_node_thread_id = old_head_node->thread_id;
    free(old_head_node->path);
    free(old_head_node);
    return head_node_thread_id;
}

Queue *dirs_queue;
Queue *threads_queue;

//--------------------Dirent functions--------------------------------

int is_dir(char *path) {
    struct stat s;
    if (stat(path, &s) < 0) {
        return FALSE;
    }
    return (S_ISDIR(s.st_mode));
}

int can_be_searched(char *path) {
    struct stat s;
    if (stat(path, &s) < 0) {
        return FALSE;
    }
    return (s.st_mode & S_IRUSR) && (s.st_mode & S_IXUSR); // has read and execute permissions
}

char *join_path(char *dir, char *entry){
    int length = strlen(dir) + strlen(entry) + 2;
    char *path = malloc(length * sizeof(char));
    if (path == NULL) {
        return NULL;
    }
    snprintf(path, length, "%s/%s", dir, entry);
    path[length - 1] = '\0';
    return path;
}

//----------------------thread functions---------------------

void destroy_mutexes() {
    pthread_mutex_destroy(&dir_queue_lock);
    pthread_mutex_destroy(&threads_queue_lock);
    pthread_mutex_destroy(&threads_counter_lock);
    pthread_mutex_destroy(&cv_list_lock);

    pthread_cond_destroy(&all_threads_created);
}

void finish_run() {
    destroy_mutexes();
    printf("Done searching, found %d files\n", files_matched_counter);

    if (threads_failed_counter > 0) {
        exit(ERROR);
    }
    exit(SUCCESS);
}

void finish_running_if_needed() {
    if (working_threads_counter == 0) {
        finish_run();
    }
}

void thread_can_run_check() {
    if (threads_counter < thread_number) {
        pthread_mutex_lock(&cv_list_lock);
        pthread_cond_wait(&all_threads_created, &cv_list_lock);
        pthread_mutex_unlock(&cv_list_lock);
    }
}

void  iterate_dir(char *dir) {
    struct dirent *cur_dirent;
    DIR *opened_dir;
    char * cur_path;
    int status;

    opened_dir = opendir(dir);
    if (opened_dir == NULL){
        fprintf(stderr, "Error open dir %s\n", dir);
        threads_failed_counter++;
        working_threads_counter--;
        finish_running_if_needed();
        pthread_exit(NULL);
    }
    cur_dirent = readdir(opened_dir);
    while (cur_dirent != NULL) {
        if (strcmp(cur_dirent->d_name, ".") != 0 && strcmp(cur_dirent->d_name, "..") != 0) {
            cur_path = join_path(dir, cur_dirent->d_name);
            if (cur_path == NULL) { //error
                fprintf(stderr, "memory allocation error %s\n", dir);
                threads_failed_counter++;
                closedir(dir);
                working_threads_counter--;
                finish_running_if_needed();
                pthread_exit(NULL);
            }
            if (is_dir(cur_path)) {
                if (can_be_searched(cur_path)) {
                    //push dir to the dirs queue
                    status = push(dirs_queue, cur_path);
                    if (status == ERROR) {
                        fprintf(stderr, "error push to queue\n");
                        threads_failed_counter++;
                        closedir(dir);
                        working_threads_counter--;
                        finish_running_if_needed();
                        pthread_exit(NULL);
                    }
                } else {
                    printf("Directory %s: Permission denied.\n", cur_path);
                }
            } else {
                // check the search term
                if (strstr(cur_dirent->d_name, search_term) != NULL) {
                    files_matched_counter++;
                    printf("%s\n", cur_path);
                }
                free(cur_path);
            }
        }
        cur_dirent = readdir(opened_dir);
    }
    closedir(opened_dir);
}

void *run_thread(void *thread_data) {
    char *dir;
    int next_thread_id;
    int thread_id = (int) thread_data;
    working_threads_counter++;
    thread_can_run_check();

    while(1) {
        dir = pop(dirs_queue);
        if (dir == NULL) { //the queue is empty
            //add to the queue and go to sleep
            if (push_thread(threads_queue, thread_id) == ERROR) { //error in thread
                threads_failed_counter++;
                pthread_exit(NULL);
            }
            pthread_mutex_lock(&cv_list_lock);
            working_threads_counter--;
            finish_running_if_needed();
            pthread_cond_wait(threads_cv + thread_id, &cv_list_lock); // waiting for another thread for signal
            pthread_mutex_unlock(&cv_list_lock);

        } else {
            iterate_dir(dir);
            //calling the next thread in the queue
            next_thread_id = pop_thread_id(threads_queue);
            if (next_thread_id != -1) {
                pthread_mutex_lock(&cv_list_lock);
                working_threads_counter++;
                pthread_cond_broadcast(threads_cv + next_thread_id);
                pthread_mutex_unlock(&cv_list_lock);
            }
        }
    }
    pthread_exit(NULL);
}



//----------------------init functions----------------------
void init_mutexes() {
    pthread_mutex_init(&dir_queue_lock, NULL);
    pthread_mutex_init(&threads_queue_lock, NULL);
    pthread_mutex_init(&threads_counter_lock, NULL);
    pthread_mutex_init(&cv_list_lock, NULL);

    pthread_cond_init(&all_threads_created, NULL);
}


void init_threads() {
    threads = (pthread_t *) malloc(thread_number * sizeof(pthread_t));
    if (threads == NULL) {
        fprintf(stderr, "Error creating memory for threads\n");
        exit(ERROR);
    }
    threads_cv = (pthread_cond_t *) malloc(thread_number * sizeof(pthread_cond_t));

    for (int i = 0; i < thread_number; i++) {
        //init cond_variable
        pthread_mutex_lock(&cv_list_lock);
        pthread_cond_init(threads_cv + i, &cv_list_lock);
        pthread_mutex_unlock(&cv_list_lock);
        //init thread
        if (0 != pthread_create(threads + i, NULL, run_thread, (void *) i)) {
            fprintf(stderr, "Error creating thread\n");
            exit(ERROR);
        } else {
            pthread_mutex_lock(&threads_counter_lock);
            threads_counter++;
            pthread_mutex_unlock(&threads_counter_lock);
            if (threads_counter == thread_number) { //if all all threads created - signal the threads to start working
                pthread_cond_broadcast(&all_threads_created);
            }
        }
    }

    for (int i = 0; i < thread_number; ++i) {
        pthread_join(threads[i], NULL);
    }
}



//-----------------------main----------------------------------

int main(int argc, char *argv[]) {
    char *root_path;
    int status;

    if (argc != 4) {
        fprintf(stderr, "Wrong arguments\n");
        exit(ERROR);
    }
    root_path = argv[1];

    if (!is_dir(root_path)) {
        fprintf(stderr, "D is not a dir\n");
        exit(ERROR);
    }

    if (!can_be_searched(root_path)) {
        printf("Directory %s: Permission denied.\n", root_path);
        exit(ERROR);
    }

    search_term = argv[2];
    thread_number = atoi(argv[3]);
    init_mutexes();

    dirs_queue = create_queue();
    if (dirs_queue == NULL) {
        printf("Error creating dirs queue\n");
        exit(ERROR);
    }
    threads_queue = create_queue();
    if (threads_queue == NULL) {
        printf("Error creating threads queue\n");
        exit(ERROR);
    }
    status = push(dirs_queue, root_path);
    if (status == ERROR) {
        printf("Error push root dir to the dirs queue\n");
        exit(ERROR);
    }

    init_threads();
}
