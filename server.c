//Eugene Li - Multithreaded chat server
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>

#define MAX_BUFFER 1024

void chatloop(int socketFd);
void buildMessage(char *result, char *name, char *msg);
void bindSocket(struct sockaddr_in *serverAddr, int socketFd, long port);

void *newClientHandler(void *data);
void *clientHandler(void *chv);
void *messageHandler(void *data);

typedef struct {
    char *buffer[MAX_BUFFER];
    int head, tail;
    int full, empty;
    pthread_mutex_t *mutex;
    pthread_cond_t *notFull, *notEmpty;
} queue;

typedef struct {
    fd_set serverReadFds;
    int socketFd;
    int clientSockets[MAX_BUFFER];
    int numClients;
    pthread_mutex_t *clientListMutex;
    queue *queue;
} chatDataVars;

typedef struct {
    chatDataVars *data;
    int clientSocketFd;
} clientHandlerVars;

void queueDestroy(queue *q);
queue* queueInit(void);
void queuePush(queue *q, char* msg);
char* queuePop(queue *q);

int main(int argc, char *argv[])
{
    struct sockaddr_in serverAddr;
    long port = 9999;
    int socketFd;

    if(argc == 2) port = strtol(argv[1], NULL, 0);

    if((socketFd = socket(AF_INET, SOCK_STREAM, 0))== -1)
    {
        fprintf(stderr, "Couldn't create socket\n");
        exit(1);
    }

    bindSocket(&serverAddr, socketFd, port);
    if(listen(socketFd, 1) == -1) perror("listen failed: ");

    chatloop(socketFd);
    
    close(socketFd);
}

//Main loop to take in chat input and display output
void chatloop(int socketFd)
{
    queue *q = queueInit();
    chatDataVars data;
    data.numClients = 0;
    data.socketFd = socketFd;
    data.queue = q;
    data.clientListMutex = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(data.clientListMutex, NULL);

    //Start thread to handle new client connections
    pthread_t connectionThread;
    if((pthread_create(&connectionThread, NULL, (void *)&newClientHandler, (void *)&data)) == 0)
    {
        fprintf(stderr, "Connection handler started\n");
    }

    FD_ZERO(&(data.serverReadFds));
    FD_SET(socketFd, &(data.serverReadFds));

    //Start thread to handle messages received
    pthread_t messagesThread;
    if((pthread_create(&messagesThread, NULL, (void *)&messageHandler, (void *)&data)) == 0)
    {
        fprintf(stderr, "Message handler started\n");
    }

    pthread_join(connectionThread, NULL);
    pthread_join(messagesThread, NULL);

    queueDestroy(q);
    pthread_mutex_destroy(data.clientListMutex);
    free(data.clientListMutex);
}

//Initializes queue
queue* queueInit(void)
{
    queue *q = (queue *)malloc(sizeof(queue));
    if(q == NULL)
    {
        fprintf(stderr, "Couldn't allocate anymore memory!\n");
        exit(EXIT_FAILURE);
    }

    q->empty = 1;
    q->full = q->head = q->tail = 0;
    q->mutex = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
    if(q->mutex == NULL)
    {
        fprintf(stderr, "Couldn't allocate anymore memory!\n");
        exit(EXIT_FAILURE);
    }
    pthread_mutex_init(q->mutex, NULL);

    q->notFull = (pthread_cond_t *) malloc(sizeof(pthread_cond_t));
    if(q->notFull == NULL)
    {
        fprintf(stderr, "Couldn't allocate anymore memory!\n");
        exit(EXIT_FAILURE);   
    }
    pthread_cond_init(q->notFull, NULL);

    q->notEmpty = (pthread_cond_t *) malloc(sizeof(pthread_cond_t));
    if(q->notEmpty == NULL)
    {
        fprintf(stderr, "Couldn't allocate anymore memory!\n");
        exit(EXIT_FAILURE);
    }
    pthread_cond_init(q->notEmpty, NULL);

    return q;
}

//Frees a queue
void queueDestroy(queue *q)
{
    pthread_mutex_destroy(q->mutex);
    pthread_cond_destroy(q->notFull);
    pthread_cond_destroy(q->notEmpty);
    free(q->mutex);
    free(q->notFull);
    free(q->notEmpty);
    free(q);
}

//Push to end of queue
void queuePush(queue *q, char* msg)
{
    q->buffer[q->tail] = msg;
    q->tail++;
    if(q->tail == MAX_BUFFER)
        q->tail = 0;
    if(q->tail == q->head)
        q->full = 1;
    q->empty = 0;
}

//Get front of queue
char* queuePop(queue *q)
{
    char* msg = q->buffer[q->head];
    q->head++;
    if(q->head == MAX_BUFFER)
        q->head = 0;
    if(q->head == q->tail)
        q->empty = 1;
    q->full = 0;

    return msg;
}

//Concatenates the name with the message and puts it into result
void buildMessage(char *result, char *name, char *msg)
{
    memset(result, 0, MAX_BUFFER);
    strcpy(result, name);
    strcat(result, ": ");
    strcat(result, msg);
}

//Sets up and binds the socket
void bindSocket(struct sockaddr_in *serverAddr, int socketFd, long port)
{
    memset(serverAddr, 0, sizeof(*serverAddr));
    serverAddr->sin_family = AF_INET;
    serverAddr->sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr->sin_port = htons(port);

    if(bind(socketFd, (struct sockaddr *)serverAddr, sizeof(struct sockaddr_in)) == -1)
        fprintf(stderr, "Bind socket failed\n");
}

//Handles new connections
void *newClientHandler(void *data)
{
    chatDataVars *chatData = (chatDataVars *) data;
    while(1)
    {
        int clientSocketFd = accept(chatData->socketFd, NULL, NULL);
        if(clientSocketFd > 0)
        {
            fprintf(stderr, "Server accepted new client. Socket: %d\n", clientSocketFd);

            pthread_mutex_lock(chatData->clientListMutex);
            if(chatData->numClients < MAX_BUFFER)
            {
                //Add new client to list
                chatData->clientSockets[chatData->numClients] = clientSocketFd;

                FD_SET(clientSocketFd, &(chatData->serverReadFds));

                //Spawn new thread to handle client's messages
                clientHandlerVars chv;
                chv.clientSocketFd = clientSocketFd;
                chv.data = chatData;

                pthread_t clientThread;
                if((pthread_create(&clientThread, NULL, (void *)&clientHandler, (void *)&chv)) == 0)
                {
                    chatData->numClients++;
                    fprintf(stderr, "Client has joined chat. Socket: %d\n", clientSocketFd);
                }
                else
                    close(clientSocketFd);
            }
            pthread_mutex_unlock(chatData->clientListMutex);
        }
    }
}

//The "producer" -- Listens for messages from client to add to message queue
void *clientHandler(void *chv)
{
    clientHandlerVars *vars = (clientHandlerVars *)chv;
    chatDataVars *data = (chatDataVars *)vars->data;

    queue *q = data->queue;
    int clientSocketFd = vars->clientSocketFd;

    char msgBuffer[MAX_BUFFER];

    while(1)
    {
        int numBytesRead = read(clientSocketFd, msgBuffer, MAX_BUFFER - 1);
        msgBuffer[numBytesRead] = '\0';

        pthread_mutex_lock(q->mutex);
        while(q->full)
        {
            //printf("Message queue full");
            pthread_cond_wait(q->notFull, q->mutex);
        }
        fprintf(stderr, "Pushing message to queue: %s\n", msgBuffer);
        queuePush(q, msgBuffer);
        pthread_mutex_unlock(q->mutex);
        pthread_cond_signal(q->notEmpty);

        memset(&msgBuffer, 0, sizeof(msgBuffer));
    }
}

//The "consumer" -- waits for the queue to have messages then takes them out and broadcasts to clients
void *messageHandler(void *data)
{
    chatDataVars *chatData = (chatDataVars *)data;
    queue *q = chatData->queue;
    int *clientSockets = chatData->clientSockets;

    while(1)
    {
        pthread_mutex_lock(q->mutex);
        while(q->empty)
        {
            //printf("Message queue empty");
            pthread_cond_wait(q->notEmpty, q->mutex);
        }
        char* msg = queuePop(q);

        fprintf(stderr, "Broadcasting message: %s\n", msg);
        for(int i = 0; i < chatData->numClients; i++)
        {
            if(write(clientSockets[i], msg, MAX_BUFFER - 1) == -1)
                fprintf(stderr, "Write failed\n");
        }

        pthread_mutex_unlock(q->mutex);
        pthread_cond_signal(q->notFull);
    }
}