
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <sys/sendfile.h>
#include <netdb.h>

#define PORT       "4080"
#define DOC_ROOT   "/DATA/etc/oson/img/"
#define MAX_EVENTS 10240

#define STATIC_ASSERT(COND,MSG) typedef char static_assertion_##MSG[(COND)?1:-1]

struct client_t
{
    int32_t fid_socket ; // socket 
    int32_t fid_file   ;
};

int print_ts(const char* msg);
int set_nonblock(int fd);
int create_and_bind(const char* port);

int hex2bin(const char* hex, int hex_size,   char* bin, int bin_size);
int is_hex(const char* s, size_t n);
int open_file_from_http_get(const char* buf, size_t sz , const char* doc_root );



int main(int argc, char* argv[] ) 
{
    int ret           ;
    int client_sock   ;
    int server_sock   ;
    int evpoll_fd     ;
    struct sockaddr_in servaddr;
    struct sockaddr_in  cli ;
    struct epoll_event ev   ;
    struct epoll_event * events ;
    char buf[ 512 ], buf2[4096] ;
    char path[ 1024 ];
    
    const char* port = PORT ;
    const char* doc_root = DOC_ROOT;
    
    if(argc > 1 )
    {
        port = argv[1];
    }
    
    if(argc > 2 )
    {
        doc_root = argv[2];
    }
    
    server_sock = create_and_bind( port );
    
    
    if (server_sock < 0 )
    {
        return -1;
    }
    
    ret = set_nonblock(server_sock);
    if (ret < 0){
        return -1;
    }
    
    ret = listen(server_sock, SOMAXCONN  ) ;
    
    if (ret != 0){
        return -1;
    }
    
    
    evpoll_fd = epoll_create1( 0 );
    
    if ( evpoll_fd == -1 ) 
    {
        return -1;
    }
    
    events = (struct epoll_event*)  malloc( MAX_EVENTS * sizeof(struct epoll_event) ) ;
    
    if (events == NULL)
    {
        return -1;
    }
    struct client_t  c  ;
    
    c.fid_socket = server_sock;
    c.fid_file   = -1;
    STATIC_ASSERT( (sizeof (c) <= sizeof(epoll_data_t)), sizeof_c_is_large_than_sizeof_epoll_data_t ) ;
    
    ev.events = EPOLLIN | EPOLLET  ;
    memcpy(&ev.data, &c, sizeof(c));
    
    ret = epoll_ctl(evpoll_fd, EPOLL_CTL_ADD, server_sock, &ev ) ;
    if (ret < 0){
        return -1;
    }
    while(1)
    {
        int n,i;
        n = epoll_wait(evpoll_fd, events, 10240, -1);
        
        print_ts("epoll_wait finished");
        
        for(i = 0; i < n; ++i)
        {
            struct epoll_event* evi = & ( events[ i ] )   ;
            struct client_t c ;
            memcpy(&c, &evi->data, sizeof (c));
            
            //1. close if finished.
            if ( (evi->events & EPOLLERR) || (evi->events & EPOLLHUP) )
            {
                print_ts("close EPOLLHUP");
                close(c.fid_socket);
                
                if (c.fid_file != -1 )
                {
                    close(c.fid_file);
                }
                continue;
            }
            
            //2. if this is server_sock
            if ( c.fid_socket == server_sock)
            {
                while(1)
                {
                    //accept a new socket.
                    socklen_t len = 0;
                    client_sock = accept(server_sock, (struct sockaddr*)&cli, &len);

                    if (client_sock < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK){
                            break;
                        }
                        //can't accept, continue.
                        printf("error on accept");
                        break;
                    }
                    

                    printf("accepted: %d\n", client_sock);
                    print_ts("accepted socket");
                    set_nonblock(client_sock);

                    struct client_t new_c;
                    
                    new_c.fid_socket  = client_sock;
                    new_c.fid_file = -1;
                    
                    // put client-sock to event - loop for reading a request.
                    ev.events = EPOLLIN | EPOLLET ;
                    memcpy(&ev.data, &new_c, sizeof(new_c));
                    
                    epoll_ctl(evpoll_fd, EPOLL_CTL_ADD, client_sock, &ev ) ;
                }
                continue;
            }
            
            //3. ready  to read a request.
            if (evi->events & EPOLLIN)
            {
                ssize_t sz;
                int done = 0;
                int first = 1;
                size_t total_sz = 0;
                client_sock = c.fid_socket ;
                c.fid_file = -1;
                
                
                //printf("ready to read: %d\n", client_sock);
                print_ts("start read socked");
                while(1)
                {
                    sz = read(client_sock, buf, sizeof buf);
                    
                    if (sz < 0 )
                    {
                        if(errno != EAGAIN)
                        {
                            printf("some another error: %d \n", errno ) ;
                             done = 1;
                             break;
                        }
                        
                        /*errno == EAGAIN. add socket to wait write a response. */
                        
                        c.fid_file = open_file_from_http_get(buf2, total_sz, doc_root );
                        
                        memcpy(&ev.data, &c, sizeof (c));
                        ev.events  = EPOLLOUT | EPOLLET ;
                        epoll_ctl(evpoll_fd, EPOLL_CTL_MOD, client_sock, &ev);
                        break;
                    }
                    
                    if (sz == 0)
                    {
                        done = 1;
                        break;
                    }
                    
                    if (total_sz + sz < sizeof buf2)
                    {
                        memcpy(  buf2 + total_sz, buf, sz);
                        total_sz += sz;
                    }
                                        
                }
                
                if (done == 1)
                {
                    printf("closed with done: %d\n", client_sock);
                    close(client_sock);
                   
                }
                print_ts("finish read request\n" );
                continue;
            }
            
            //4. ready to write a response.
            if (evi->events & EPOLLOUT ) 
            {
                int file_fd;
                ssize_t sz;
                int done = 0;
                
                client_sock = c.fid_socket ;
                
                struct stat st;
                file_fd = c.fid_file;
                if ( file_fd < 0  || fstat( file_fd, &st) != 0) 
                {
                    file_fd = -1;
                }
                
                
                print_ts("ready to write");
                
                
                if (file_fd < 0)
                {
                    printf("file not found!\n");
                    
                    //send a not found response.
                    char out[] = "HTTP/1.1 404 NOT FOUND\r\nContent-Type: text/html;charset=UTF-8\r\n\r\nNot found";
                    
                    sz = write(client_sock, out, sizeof out);
                    if (sz < 0)
                    {
                        if (errno != EAGAIN)
                        {
                            done = 1;
                        } else {
                            done = 0;
                        }
                    } else {
                        done = 1;
                    }
                    
                    if (done == 1)
                    {
                        close(client_sock);
                    } 
                    continue;
                }
                
                off_t of = lseek(file_fd, 0, SEEK_CUR) ;
                
                if ( of == (off_t)0 /*beginning*/)
                {
                    char head[512];
                    int sz_head = snprintf(head, 512,  "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %ld \r\n\r\n", (long int)st.st_size) ;
                    write(client_sock, head, sz_head);
                }
                
                while(1)
                {
                    sz = sendfile(client_sock, file_fd, 0, 8192 );
                
                    if( sz < 0 )
                    {
                        printf("errno: %d \n", errno );
                        if  (errno != EAGAIN){
                            done = 1;
                            break;
                        }
                       
                        break;
                    }
                    printf("sz: %d\n", (int)sz);
                    if (sz == 0)
                    {
                        done = 1;
                        break;
                    }
                }
                
                if (done)
                {
                    close(client_sock);
                    close(file_fd);
                    print_ts("finished ts done=1");
                } else {
                    print_ts("finished ts done=0");
                }
                
                continue;
            }
            
            
            //some other situation. close file descriptor.
            //close(evi->data.fd);
            
        } // end for i = 0..n  loop.
        
    } // end main event-loop
    
    
    free(events);
    close(evpoll_fd);
    close(server_sock);
    return 0;
}


int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0 ) ;
    if (flags == -1)return -1;
    flags |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
}

int create_and_bind(const char* port)
{
    struct addrinfo hints, *result, *rp;
    int ret, fd;
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC ;
    hints.ai_socktype = SOCK_STREAM ;
    hints.ai_flags = AI_PASSIVE;
    
    ret = getaddrinfo(NULL, port, &hints, /*OUT*/ &result ) ;
    
    
    if (ret != 0 ){
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }
    
    for( rp = result; rp != NULL; rp = rp->ai_next ) 
    {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0 ){
            continue;
        }
        
        ret = bind(fd, rp->ai_addr, rp->ai_addrlen ) ;
        if (ret == 0){
            break;
        }
        
        close(fd);
    }
    
    freeaddrinfo(result);
    if (rp == NULL ) {
        perror("bind");
        return -1;
    }
    
    return fd;
    
}

int hex2bin(const char* hex, int hex_size,   char* bin, int bin_size)
{
    int i,n, u1,u2;
    if ( hex == NULL || hex_size <= 0   )
        return 0 ;
    
    if (bin == NULL )
        return -1;
    
    if (  bin_size * 2 < hex_size )
        return -1;
    
    n = hex_size / 2;
    
    for( i = 0; i< n; i ++ )
    {
        u1 = ( unsigned char ) hex[ i * 2 + 0 ] ;
        u2 = ( unsigned char ) hex[ i * 2 + 1 ] ;
        
        if (u1 >= '0' && u1 <= '9')
            u1 = u1 - '0';
        else if (u1 >='a' && u1 <='f')
            u1 = u1 - 'a' + 10;
        else 
            u1 = u1 - 'A' + 10;
        
        if ( u2 >= '0' && u2 <= '9' )
            u2 = u2 -'0';
        else if (u2 >='a' && u2 <='f')
            u2 = u2 - 'a' + 10;
        else
            u2 = u2 - 'A' + 10;
        
        bin[i] = (char)( ( u1 << 4) | u2 ) ;
    }
    
    return n;
        
}

int is_hex(const char* s, size_t n)
{
    size_t i;
    for(i = 0; i < n; ++i){
        if (s[i] >='0' && s[i] <='9' )
            ;
        else if (s[i] >='a' && s[i] <='f')
            ;
        else if (s[i] >='A' && s[i] <='F' )
            ;
        else
            return 0;
    }
    return 1;
}

int open_file_from_http_get(const char* buf, size_t sz , const char* doc_root )
{
    int is_http_get  ;
    int pos, end_pos, splash;                
    const char* target;
     
    
    is_http_get = sz > 3 && buf[0] =='G' && buf[1] == 'E' && buf[2] == 'T' && buf[3] == '\x20';
    if ( ! is_http_get )
        return -1;
    
    
    
    pos = 4;
    end_pos = 0;
    while(pos < sz && buf[pos] <= '\x20' &&  ! ( pos + 1 < sz && buf[pos] == '\r' && buf[pos + 1] == '\n' ) ) 
        ++pos;
    end_pos = pos;
    while(end_pos < sz && buf[end_pos] > '\x20' ) 
        ++end_pos;

    target = buf + pos;

    // MUST start with /geticon/....png
    if ( ! ( end_pos - pos >= 9 && strncmp(target, "/geticon/", 9)  == 0 ) )
        return -1;
        
        
    //find last '/' from end

    splash = end_pos ;

    while(splash > pos && buf[splash-1] != '/' )
        --splash;

    target = buf + splash  ;
    
    //find last dot
    int dot = end_pos;
    int i;
    for( i = splash; i < end_pos-1; ++i)
    {
        //  '..'  -- invalid path.
        if (buf[i] == '.' && buf[i+1] == '.' )
        {
            return -1;
        }
        
        if(buf[i] == '.')
            dot = i;
    }
    
    //dot not found 
    if(dot == end_pos)
        return -1;
    
    char path[512]  = {0};
    
    strcpy(path, doc_root ) ;
    
    //find last  dost.
    if (is_hex(target, dot - splash))
    {
        int len = strlen(path);
        
        // 3137.png
        
        int n = hex2bin(target, dot - splash, path + len, 512 - len ) ;
       // printf("hex2bin result: n = %d\n", n ) ;
        
        len += n;
        strncpy(path + len, buf + dot, end_pos - dot );
    }
    else
    {
        strncat(path, target, end_pos - splash );
    }
    
    printf("path: %s\n", path);
    {
        int fid;
        fid = open(path, O_RDONLY | O_NONBLOCK);
        if (fid < 0)
            return -1;
        return fid;
    }
    
}

int print_ts(const char* msg)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("ts(%d: %09ld) msg: %s \n", (int)ts.tv_sec, (long int)ts.tv_nsec, msg);
}
