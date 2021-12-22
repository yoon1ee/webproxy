#include <stdio.h>
#include "csapp.h"
/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define LRU_MAGIC_NUMBER 9999 // 캐쉬 블록의 사용 순서를 알게 하기 위한 상수.
#define CACHE_OBJS_COUNT 10 // 캐쉬 블록 10개로 설정.

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *prox_hdr = "Proxy-Connection: close\r\n";
static const char *host_hdr_format = "Host: %s\r\n";
static const char *requestlint_hdr_format = "GET %s HTTP/1.0\r\n";
static const char *endof_hdr = "\r\n";

static const char *connection_key = "Connection";
static const char *user_agent_key= "User-Agent";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *host_key = "Host";

void doit(int connfd);
void parse_uri(char *uri,char *hostname,char *path,int *port);
void build_http_header(char *http_header,char *hostname,char *path,int port,rio_t *client_rio);
int connect_endServer(char *hostname,int port,char *http_header);
void *thread(void *vargsp);
/* cache function */
void cache_init();
int cache_find(char *url);
int cache_eviction();
void cache_LRU(int index);
void cache_uri(char *uri,char *buf);
void readerPre(int i);
void readerAfter(int i);

/* Cache  structure */
typedef struct {
    char cache_obj[MAX_OBJECT_SIZE]; // 최대로 담을 수 있는 캐쉬 블록의 사이즈
    char cache_url[MAXLINE]; // 캐쉬에 해당 url 정보가 있는지 확인하기 위한 변수.
    int LRU; // least recently used 사용 순서 비교를 위한 값.
    int isEmpty; // 캐시 블록 사용 여부 0과 1로 판단.

    int readCnt;            /*count of readers*/
    sem_t wmutex;           /*protects accesses to cache  세마포어 타입의 뮤텍스.한 쓰레드가 자원을 쓰고 있을때 다른 쓰레드가 못쓰게 막는 뮤텍스 선언*/
    sem_t rdcntmutex;       /*protects accesses to readcnt*/

    int writeCnt;   //readCnt 와 반대 쓰임새. 구조는 같다.
    sem_t wtcntMutex; 
    sem_t queue;

} cache_block;

typedef struct { 
    cache_block cacheobjs[CACHE_OBJS_COUNT];  /*cache_block 구조체를 가지는 cacheobjs[10] 배열 을 만들었다.*/
    int cache_num; // 캐쉬 블록 인덱스 사용하기 위한 변수.
} Cache;

Cache cache; // static 도 된다. 

int main(int argc,char **argv)
{
    int listenfd,connfd;
    socklen_t  clientlen;
    char hostname[MAXLINE],port[MAXLINE];
    struct sockaddr_storage clientaddr;/*generic sockaddr struct which is 28 Bytes.The same use as sockaddr*/
    pthread_t tid;

    cache_init(); // 여기서 초기화?

    if(argc != 2){
        fprintf(stderr,"usage :%s <port> \n",argv[0]);
        exit(1);
    }
    Signal(SIGPIPE, SIG_IGN); /* 예외적인 상황일때 날리는 신호, 트랩. 잘못됐다는 시그널 받으면 프로세스 종료해야 하는데, 전체 클라이언트를 닫으면
     안되므로 신호를 무시해라는 뜻. */
    listenfd = Open_listenfd(argv[1]);
    while(1){
        clientlen = sizeof(clientaddr);
        connfd = Malloc(sizeof(int));
        connfd = Accept(listenfd,(SA *)&clientaddr,&clientlen);

        /*print accepted message*/
        Getnameinfo((SA*)&clientaddr,clientlen,hostname,MAXLINE,port,MAXLINE,0);
        printf("Accepted connection from (%s %s).\n",hostname,port);

        Pthread_create(&tid, NULL, thread, (void *)connfd);
    }
    return 0;
}

void *thread(void *vargs) {
    int connfd = (int)vargs;
    Pthread_detach(pthread_self()); // 이쓰레드를 프로세스에서 분리
    doit(connfd); // 각각 클라이언트와 연결하고 싶어서
    Close(connfd);
}

/*handle the client HTTP transaction*/
void doit(int connfd)
{
    int end_serverfd;/*the end server file descriptor*/

    char buf[MAXLINE],method[MAXLINE],uri[MAXLINE],version[MAXLINE];
    char endserver_http_header [MAXLINE];
    /*store the request line arguments*/
    char hostname[MAXLINE],path[MAXLINE];
    int port;

    rio_t rio,server_rio;/*rio is client's rio,server_rio is endserver's rio*/

    Rio_readinitb(&rio,connfd);
    Rio_readlineb(&rio,buf,MAXLINE);
    sscanf(buf,"%s %s %s",method,uri,version); /*read the client request line*/

    char url_store[100]; // 길이 100을 가지는 url_store.
    strcpy(url_store,uri);  /*store the original url , connfd가 들고 있는 uri 넣기*/
    if(strcasecmp(method,"GET")){
        printf("Proxy does not implement the method");
        return;
    }

     /*the uri is cached ? */
    int cache_index;
    if((cache_index=cache_find(url_store))!=-1){/*url_store 에 해당하는 캐시 블록 있는지 찾는다. 찾으면 인덱스 반환. 찾으면 if 실행*/
         readerPre(cache_index); // locking cache 다른 쓰레드에서 접근하지 못하도록 막는다.
         Rio_writen(connfd,cache.cacheobjs[cache_index].cache_obj,strlen(cache.cacheobjs[cache_index].cache_obj)); // 프록시 서버에서 클라이언트로 바로 보내주는 부분.
         readerAfter(cache_index); // unlocking cache 다른 쓰레드에서 접근 가능.
         cache_LRU(cache_index); // 가장 최근에 사용된 캐시 블럭 갱신.
         return;
    }

    /*parse the uri to get hostname,file path ,port*/
    parse_uri(uri,hostname,path,&port);

    /*build the http header which will send to the end server*/
    build_http_header(endserver_http_header,hostname,path,port,&rio);

    /*connect to the end server*/
    end_serverfd = connect_endServer(hostname,port,endserver_http_header);
    if(end_serverfd < 0){
        printf("connection failed\n");
        return;
    }

    Rio_readinitb(&server_rio,end_serverfd);
    /*write the http header to endserver*/
    Rio_writen(end_serverfd,endserver_http_header,strlen(endserver_http_header));

    /*receive message from end server and send to the client*/
    char cachebuf[MAX_OBJECT_SIZE]; //cachebuf의 최대 크기 선언.
    int sizebuf = 0;
    size_t n;
    while((n = Rio_readlineb(&server_rio,buf,MAXLINE)) != 0) // line별로 가져온다.
    {   
        sizebuf+=n;
        if(sizebuf < MAX_OBJECT_SIZE)  
            strcat(cachebuf,buf); //buf를 한줄씩 cachebuf로 복사. 한줄씩 가져오기 때문에 strcat함수로 이어 붙인다.
        printf("proxy received %d bytes,then send\n",n);
        Rio_writen(connfd,buf,n); // 프록시서버가 end서버에 받아온 거 confdclient에 보냄.
    }
    Close(end_serverfd);

     /*store it*/
    if(sizebuf < MAX_OBJECT_SIZE){ // 한번 더 체크 , cachebuf 쓰면 안됨.
        cache_uri(url_store,cachebuf); //
    }
}

void build_http_header(char *http_header,char *hostname,char *path,int port,rio_t *client_rio)
{
    char buf[MAXLINE],request_hdr[MAXLINE],other_hdr[MAXLINE],host_hdr[MAXLINE];
    /*request line*/
    sprintf(request_hdr,requestlint_hdr_format,path);
    /*get other request header for client rio and change it */
    while(Rio_readlineb(client_rio,buf,MAXLINE)>0)
    {
        if(strcmp(buf,endof_hdr)==0) break;/*EOF*/

        if(!strncasecmp(buf,host_key,strlen(host_key)))/*Host:*/
        {
            strcpy(host_hdr,buf);
            continue;
        }

        if(!strncasecmp(buf,connection_key,strlen(connection_key))
                &&!strncasecmp(buf,proxy_connection_key,strlen(proxy_connection_key))
                &&!strncasecmp(buf,user_agent_key,strlen(user_agent_key)))
        {
            strcat(other_hdr,buf);
        }
    }
    if(strlen(host_hdr)==0)
    {
        sprintf(host_hdr,host_hdr_format,hostname);
    }
    sprintf(http_header,"%s%s%s%s%s%s%s",
            request_hdr,
            host_hdr,
            conn_hdr,
            prox_hdr,
            user_agent_hdr,
            other_hdr,
            endof_hdr);

    return ;
}
/*Connect to the end server*/
inline int connect_endServer(char *hostname,int port,char *http_header){
    char portStr[100];
    sprintf(portStr,"%d",port);
    return Open_clientfd(hostname,portStr);
}

// http://localhost\0 8000/home.html
// http://localhost/home.html
//http://localhost
/*parse the uri to get hostname,file path ,port*/
void parse_uri(char *uri,char *hostname,char *path,int *port)
{
    *port = 80;
    char* pos = strstr(uri,"//");

    pos = pos!=NULL? pos+2:uri;

    char*pos2 = strstr(pos,":");
    if(pos2!=NULL)
    {
        *pos2 = '\0';
        sscanf(pos,"%s",hostname);
        sscanf(pos2+1,"%d%s",port,path);
    }
    else
    {
        pos2 = strstr(pos,"/");
        if(pos2!=NULL)
        {
            *pos2 = '\0';
            sscanf(pos,"%s",hostname);
            *pos2 = '/';
            sscanf(pos2,"%s",path);
        }
        else
        {
            sscanf(pos,"%s",hostname);
        }
    }
    return;
}

void cache_init(){
    cache.cache_num = 0;
    int i;
    for(i=0;i<CACHE_OBJS_COUNT;i++){
        cache.cacheobjs[i].LRU = 0; // 시작할 때 LRU
        cache.cacheobjs[i].isEmpty = 1; //1이 비어있음.
        // 캐시에 접근 하는 프로텍트
        Sem_init(&cache.cacheobjs[i].wmutex,0,1); // 캐시오브젝트 포인터, 세마포어 뮤텍스로 쓸려면 0, 그 값을 1로 넣겠다. 
        // readcount에 접근 하는 프로텍트.
        Sem_init(&cache.cacheobjs[i].rdcntmutex,0,1);
        cache.cacheobjs[i].readCnt = 0;

        cache.cacheobjs[i].writeCnt = 0;
        Sem_init(&cache.cacheobjs[i].wtcntMutex,0,1);
        Sem_init(&cache.cacheobjs[i].queue,0,1);
    }
}

void readerPre(int i){ // P 연산과 V연산을 이용해서 안전 궤적을 가지는 변수를 가져온다.
    /*해당 인덱스를  */
    P(&cache.cacheobjs[i].queue); 
    P(&cache.cacheobjs[i].rdcntmutex);
    cache.cacheobjs[i].readCnt++; //readCnt 1일때만 접근할 수 있다.
    if(cache.cacheobjs[i].readCnt==1) P(&cache.cacheobjs[i].wmutex); // readCnt가 2가 되면 접근할 수 없다.
    V(&cache.cacheobjs[i].rdcntmutex);
    V(&cache.cacheobjs[i].queue);
}

void readerAfter(int i){
    P(&cache.cacheobjs[i].rdcntmutex);
    cache.cacheobjs[i].readCnt--;
    if(cache.cacheobjs[i].readCnt==0) V(&cache.cacheobjs[i].wmutex);
    V(&cache.cacheobjs[i].rdcntmutex);

}

void writePre(int i){
    P(&cache.cacheobjs[i].wtcntMutex);
    cache.cacheobjs[i].writeCnt++;
    if(cache.cacheobjs[i].writeCnt==1) P(&cache.cacheobjs[i].queue);
    V(&cache.cacheobjs[i].wtcntMutex);
    P(&cache.cacheobjs[i].wmutex);
}

void writeAfter(int i){
    V(&cache.cacheobjs[i].wmutex);
    P(&cache.cacheobjs[i].wtcntMutex);
    cache.cacheobjs[i].writeCnt--;
    if(cache.cacheobjs[i].writeCnt==0) V(&cache.cacheobjs[i].queue);
    V(&cache.cacheobjs[i].wtcntMutex);
}

/*find url is in the cache or not */
int cache_find(char *url){
    int i;
    for(i=0;i<CACHE_OBJS_COUNT;i++){
        readerPre(i);
        if((cache.cacheobjs[i].isEmpty==0) && (strcmp(url,cache.cacheobjs[i].cache_url)==0)) break;
        readerAfter(i);
    }
    if(i>=CACHE_OBJS_COUNT) return -1; /*can not find url in the cache '=' 만 해도 되지 않을까?*/
    return i;
}

/*find the empty cacheObj or which cacheObj should be evictioned*/
int cache_eviction(){ // 캐시 블럭 10개를 처음부터 보면서 가장 첫번째로 나오는 빈 캐시 블럭을 리턴.
    int min = LRU_MAGIC_NUMBER;
    int minindex = 0;
    int i;
    for(i=0; i<CACHE_OBJS_COUNT; i++)
    {
        readerPre(i);
        if(cache.cacheobjs[i].isEmpty == 1){/*비어있는 캐시 블록의 i 인덱스 */
            minindex = i;
            readerAfter(i);
            break; 
        }
        if(cache.cacheobjs[i].LRU < min){    /*비어 있는 캐시 블록을 찾지 못함.*/
            min = cache.cacheobjs[i].LRU; // 이거 우리가 추가함. min값 갱신.
            minindex = i;
            readerAfter(i);
            continue;
        }
        readerAfter(i);
    }

    return minindex;
}
/*update the LRU number except the new cache one*/
void cache_LRU(int index){ // 방금 채운 cache 블럭 인덱스 or 방금 사용한 cache 블럭 인덱스

    writePre(index); // locking해서 다른 쓰레드 접근 불가.
    cache.cacheobjs[index].LRU = LRU_MAGIC_NUMBER; // 해당 인덱스의 LRU를 9999로 바꿔준다.가장 최근에 사용됨을 알려줌
    writeAfter(index); // unlocking

    int i;
    for(i=0; i<index; i++)    { //지금 사용된 인덱스 전 캐시블록들 까지 for문
        writePre(i); // locking
        if(cache.cacheobjs[i].isEmpty==0 && i!=index){ // 캐시 블록이 채워져있고, 자기 자신이 아니면
            cache.cacheobjs[i].LRU--; // cache.cacheobjs[i].lru를 하나 내림.
        }
        writeAfter(i); // unlocking
    }
    i++; // i는 index + 1
    for(i; i<CACHE_OBJS_COUNT; i++)    { //자기 자신 이후 부터 마지막 까지 for문
        writePre(i);
        if(cache.cacheobjs[i].isEmpty==0 && i!=index){
            cache.cacheobjs[i].LRU--;
        }
        writeAfter(i);
    }
}
/*cache the uri and content in cache*/
void cache_uri(char *uri,char *buf){
    int i = cache_eviction(); // 캐쉬 저장할 블록 인덱스.

    writePre(i);/* locking, writer P*/

    strcpy(cache.cacheobjs[i].cache_obj,buf); // buf를 cache_obj에 복사.
    strcpy(cache.cacheobjs[i].cache_url,uri); // uri를 cache_url에 복사.
    cache.cacheobjs[i].isEmpty = 0; // 캐시 블록 사용 처리.

    writeAfter(i);/*unlocking, writer V*/

    cache_LRU(i); // 9999 업데이트.
}
/*캐시를 했다가 일정 시간이 지나서 풀어주는 함수가 있는지? 지윤누가 만들기로 함.*/