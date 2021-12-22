 /* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
// void serve_static(int fd, char *filename, int filesize);
void serve_static(int fd, char *filename, int filesize, char *method);
void get_filetype(char *filename, char *filetype);
// void serve_dynamic(int fd, char *filename, char *cgiargs);
void serve_dynamic(int fd, char *filename, char *cgiargs, char *method);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg);
void echo(int connfd); // 11.6 a
// void sigchild_handler(int sig); // 11.8

int main(int argc, char **argv) {
  int listenfd, connfd; // 듣기 식별자, 연결 식별자, fd: 파일 또는 소켓을 지칭하기 위해 부여한 숫자
  char hostname[MAXLINE], port[MAXLINE]; // 호스트 이름과 포트를 저장할 "캐릭터" 배열
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;

  /* Check command line args */
  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  // if (Signal(SIGCHLD, sigchild_handler) == SIG_ERR) // 11.8
  //   unix_error("signal child handler error"); //11.8
  listenfd = Open_listenfd(argv[1]);
  while (1) {
    clientlen = sizeof(clientaddr);
    connfd = Accept(listenfd, (SA *)&clientaddr,
                    &clientlen);  // line:netp:tiny:accept  // 반복적으로 연결 요청 접수
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE,
                0);
    printf("Accepted connection from (%s, %s)\n", hostname, port);
    doit(connfd);   // line:netp:tiny:doit // transaction 수행
    // echo(connfd); // 11.6 a
    Close(connfd);  // line:netp:tiny:close // server 쪽 연결 close
  }
}


// void sigchild_handler(int sig) { // 11.8
//   int old_errno = errno;
//   int status;
//   pid_t pid;
//   while ((pid = waitpid(-1, &status, WNOHANG)) >0) {
//   }
//   errno = old_errno;
// }



void doit(int fd) // 한번에 하나의 HTTP request/response transaction
{
  int is_static;
  struct stat sbuf;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char filename[MAXLINE], cgiargs[MAXLINE];
  rio_t rio; //rio 구조체 선언

  /* Read request line and headers */
  Rio_readinitb(&rio, fd); // &rio 주소를 가지는 읽기 버퍼를 만들고 초기화 
  Rio_readlineb(&rio, buf, MAXLINE); // 버퍼에서 읽은 것 담겨있음 (rio_readlineb함수를 사용해 요청라인 읽음)
  printf("Request headers:\n");
  printf("%s", buf); // "GET / HTTP/1.1"
  sscanf(buf, "%s %s %s", method, uri, version); // 버퍼에서 자료형 읽음, 요청라인 분석
  // if (strcasecmp(method, "GET")){ // comment for 11.11  // TINY는 get요청만 지원 - 다른 method 요청시 에러메세지 보낸 후 메인 루틴으로 돌아와서 연결을 닫음
  if (!(strcasecmp(method, "GET") == 0 || strcasecmp(method, "HEAD") == 0)){ 
    clienterror(fd, method, "501", "Not implemented",
              "Tiny does not implement this method");
      return; // method == "GET": 두 문자열의 길이와 내용 같을 때 0 반환
  }
  read_requesthdrs(&rio);

  /* parse URI from GET request */
  // uri 분석, 파일이 없는 경우 에러, parse_uri들어가기 전엔 filename, cgiargs 는 없음
  // 이 uri를 cgi 인자 스트링으로 분석하고 요청이 정적 또는 동적 컨텐츠인지 flag를 설정
  is_static = parse_uri(uri, filename, cgiargs);

  // 이 파일이 디스크 상에 있지 않으면(??) 에러메세지 클라이언트에 보내고 종료
  if(stat(filename, &sbuf) < 0) { // stat=> 파일 정보를 불러오고 sbuf 에 내용을 적어줌 ok 0, error -1
    clienterror(fd, filename, "404", "Not found",
                "Tiny couldn't find this file");
    return;
  }

// 정적 컨텐츠일경우 
  if(is_static){ /* serve static content */
    // s_isreg => 일반파일인지?, s_irusr => 읽기 권한이 있는지?, s_ixusr => 실행권한이 있는지?
    // 일반파일이고 읽기권한을 가지고 있는지 확인
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
      // 일반파일이 아니거나 권한이 없는경우 클라이언트에 403 에러 반환
      clienterror(fd, filename, "403", "Forbidden",
                  "Tiny couldn't read the file");
      return;
    }
    // satisfied if conditions => 클라이언트에 파일 전달
    // serve_static(fd, filename, sbuf.st_size);  // comment for 11.11
    serve_static(fd, filename, sbuf.st_size, method);
  } // 정적 컨텐츠가 아닌경우
  else { /* Serve dynamic content */
    // 일반파일이고 실행권한이 있는지 검증
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) {
      // 일반파일이 아니거나 실행 불가능한 경우 403 error
      clienterror(fd, filename, "403", "Forbidden",
                  "Tiby couldn't run the CGI program");
      return;
    }
    // 조건에 맞는경우 클라이언트에 동적파일 제공
    // serve_dynamic(fd, filename, cgiargs);  // comment for 11.11
    serve_dynamic(fd, filename, cgiargs, method);
  }
}


void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
/* 
        description: 클라이언트에 오류 전달. HTTP 응답을 응답 라인에 상태코드, 상태 메세지와 함께 클라이언트에 보내고, 응답 본체에 HTML파일도 함께 보냄.
                      HTML응답은 본체에서 컨텐츠의 크기와 타입을 나타내야 하기 때문에 HTML 컨텐츠를 한개의 스트링으로 만듦 => 크기를 쉽게 결정 가능.
                      이 함수에서는 robust한 rio_writen 함수를 모든 출력에 대해 사용하고 있음 (???)
        Args:
            int fd
            char *cause
            char *errnum
            char *shortmsg
            char *longmsg
        Returns:
            void
*/
{
  char buf[MAXLINE], body[MAXBUF];

  /* Build the HTTP response body */
  sprintf(body, "<html><title>Tiny Error</title>");
  sprintf(body, "%s<vody bgcolor = ""ffffff"">\r\n", body);
  sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
  sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
  sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

  /* Print the HTTP response */
  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-type: text/html\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
  Rio_writen(fd, buf, strlen(buf));
  Rio_writen(fd, body, strlen(body));
}


void read_requesthdrs(rio_t *rp)
/* 
        description: 요청 헤더를 읽음. TINY는 요청 헤더 내 어떤 정보도 사용하지 않음.
                      단순히 read_requesthdrs 함수를 호출해서 이들을 읽고 무시. 
        Args:
            rio_t *rp
        Returns:
            void
*/
{
  char buf[MAXLINE];
  
  Rio_readlineb(rp, buf, MAXLINE); // buf에서 maxline까지 읽기
  while (strcmp(buf, "\r\n")) {
    // 끝줄 나올 때까지 읽음. 요청 헤더를 종료하는 빈 텍스트 줄이 위 line에서 체크하고 있는 carriage return과 line feed쌍으로 구성.
    Rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);
  }
  return;
}



int parse_uri(char *uri, char *filename, char *cgiargs)
/* 
        description: TINY는 정적 컨텐츠를 위한 홈 디렉토리가 자신의 현재 디렉토리고, 
                      실행파일의 홈 디렉토리는 /cgi-bin이라고 가정. 스트링 cgi-bin을 포함하는 모든 uri는
                      동적 컨텐츠를 요청하는 것을 나타낸다고 가정한다. 기본 파일 이름은 ./home.html 
        Args:
            char *uri
            char *filename
            char *cgiargs
        Returns:
            int
*/
{
  char *ptr;
  // uri 를 파일 이름과 옵션(?)으로 CGI인자 스트링을 분석
  // cig-bin이 없다면 (== 정적 컨텐츠를 위한 것이라면)
  if (!strstr(uri, "cgi-bin")) { /*static content */
    strcpy(cgiargs, ""); // cgi 인자 스트링을 지운다
    strcpy(filename, ".");
    strcat(filename, uri); // ./home.html이 된다 (상대 리눅스 경로 이름으로 변환)
    if (uri[strlen(uri)-1] == '/') // uri가  '/'문자로 끝난다면
      strcat(filename, "home2.html"); // 기본 파일 이름을 추가
    return 1;
  }
  else { /* Dynamic content */ // 동적 컨텐츠를 위한 것이면
    ptr = index(uri, '?');
    // 모든 cgi인자 추출
    if (ptr) {
        strcpy(cgiargs, ptr+1);
        // 물음표 뒤에 있는 인자 다 붙임
        *ptr = '\0';
        // 포인터는 문자열 마지막으로 바꿈
        // uri 물음표 뒤 다 없애기
    }
    else
        strcpy(cgiargs, ""); // 물음표 뒤 인자 전부 넣기
    strcpy(filename, "."); // 나머지 부분 상대 리눅스 uri 로 바꿈
    strcat(filename, uri); // ./uri 가 된다
    return 0;
  }
}


// void serve_static(int fd, char *filename, int filesize)  //comment for 11.11
void serve_static(int fd, char *filename, int filesize, char *method)
{
  int srcfd;
  char *srcp, filetype[MAXLINE], buf[MAXBUF];

  /* Send response headers to client */
  get_filetype(filename, filetype);
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
  sprintf(buf, "%sContent_length: %d\r\n", buf, filesize);
  sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
  Rio_writen(fd, buf, strlen(buf));
  printf("Response headers: \n");
  printf("%s", buf);

  if (strcasecmp(method, "HEAD") == 0)  // 11.11
    return;

  /* Send response body to client */
  srcfd = Open(filename, O_RDONLY, 0);
  // srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0); // comment for 11.9
  srcp = (char*)Malloc(filesize); // 11.9
  Rio_readn(srcfd, srcp, filesize); // 11.9
  Close(srcfd);
  Rio_writen(fd, srcp, filesize);
  // Munmap(srcp, filesize); // comment for 11.9
  free(srcp); // 11.9
}


/* get_filetype - Derive file type from filename */
void get_filetype(char *filename, char *filetype)
{
  if (strstr(filename, ".html"))
    strcpy(filetype, "text/html");
  else if (strstr(filename, ".gif"))
    strcpy(filetype, "image/gif");
  else if (strstr(filename, ".png"))
    strcpy(filetype, "image/png");
  else if (strstr(filename, ".jpg"))
    strcpy(filetype, "image/jpeg");
  else if (strstr(filename, ".mp4")) // 11.7
    strcpy(filetype, "video/mp4");
  else if (strstr(filename, ".mpg")) // 11.7  chrome does not support .mpg - open in safari
    strcpy(filetype, "video/mpg");
  else
    strcpy(filetype, "text/plain");
}


// void serve_dynamic(int fd, char *filename, char *cgiargs)   // comment for 11.11
void serve_dynamic(int fd, char *filename, char *cgiargs, char *method)
{
  char buf[MAXLINE], *emptylist[] = { NULL };

  /* Return first part of HTTp reponse */
  sprintf(buf, "HTTP/1.0/200 OK\r\n");    // \r carriage return -  to the beginnint of the row, \n 다음줄 ; 요즘은 \n하나로 \r\n역할 함
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Server; Tiny Web Server\r\n");
  Rio_writen(fd, buf, strlen(buf));

  if (Fork() == 0) { /* Child */
    /* Real server would set all CGI vars here */
    setenv("QUERY_STRING", cgiargs, 1);
    setenv("REQUEST_METHOD", method, 1);  // 11.11
    Dup2(fd, STDOUT_FILENO);      /* Redirect stdout to client */
    Execve(filename, emptylist, environ); /* Run CGI program */
  }
  Wait(NULL); /* Parent waits for and reaps child */
}


void echo(int connfd)
{
  size_t n;
  char buf[MAXLINE];
  rio_t rio;

  Rio_readinitb(&rio, connfd);
  while((n = Rio_readlineb(&rio, buf, MAXLINE)) != 0){
      printf("server received %d bytes\n", (int)n);
      
      // printf("\tserver received : ");
      // for(int i = 0; i < MAXLINE; i++){
      //   printf("%c", buf[i]);
      // }
      // printf("\n");
      
      Rio_writen(connfd, buf, n);
  }
}