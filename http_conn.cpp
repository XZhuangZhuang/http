#include "http_conn.h"

/*定义HTTP相应的一些状态信息*/
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";
/*网站根目录*/
const char* doc_root = "/var/www/html";

//设置文件描述符为非阻塞
int setnonblocking( int fd )
{
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}
//在epoll事件表中添加新的文件描述符，同时采用ET模式，并且设置EPOLLONESHOT 属性：任一时刻，socket连接只能被一个线程所处理
void addfd( int epollfd, int fd, bool one_shot )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if( one_shot )
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}
//在epoll事件表中删除新的文件描述符，并且关闭文件描述符
void removefd( int epollfd, int fd )
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close( fd );
}

//EPOLLONESHOT需要重置事件后事件才能进行下次侦听
void modfd( int epollfd, int fd, int ev )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

int http_conn::m_user_count = 0;//连接数
int http_conn::m_epollfd = -1;//事件表，注意是static故所有http_con类对象共享一个事件表

//关闭连接，从事件表中移除描述符
void http_conn::close_conn( bool real_close )
{
    if( real_close && ( m_sockfd != -1 ) )
    {
        //modfd( m_epollfd, m_sockfd, EPOLLIN );
        removefd( m_epollfd, m_sockfd );
        m_sockfd = -1;
        m_user_count--;
    }
}

void http_conn::init( int sockfd, const sockaddr_in& addr )
{
    m_sockfd = sockfd;
    m_address = addr;

    int error = 0;
    socklen_t len = sizeof( error );
    getsockopt( m_sockfd, SOL_SOCKET, SO_ERROR, &error, &len );//获取与某个套接字关联的选项
	/*以下两行是为了避免TIME_WAIT状态，仅用于调试，实际使用时应该去掉*/
    int reuse = 1;
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );//设置与某个套接字关联的选项
     //一个端口释放后会等待两分钟之后才能再被使用，SO_REUSEADDR是让端口释放后立即就可以被再次使用。
    addfd( m_epollfd, sockfd, true );//在事件表中添加连接套接字
    m_user_count++;//用户数+1

    init();
}

void http_conn::init()//初始化其他shttp_conn类的成员变量
{
    m_check_state = CHECK_STATE_REQUESTLINE;//主状态机当前所处的状态初始为正在解析请求行
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset( m_read_buf, '\0', READ_BUFFER_SIZE );
    memset( m_write_buf, '\0', WRITE_BUFFER_SIZE );
    memset( m_real_file, '\0', FILENAME_LEN );
}

/*从状态机*/  //用于解析出一行内容
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
	/*m_checked_index指向m_read_buf中当前正在分析的字节，m_read_idx指向m_read_buffer中客户数据的
	尾部的下一字节。m_read_buffer中第0~m_checked_index字节都已经分析完毕，第m_checked_index~（m_read_index-1）
	字节由下面的循环挨个分析*/
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx )
    {
		/*获得当前要分析的字节*/
        temp = m_read_buf[ m_checked_idx ];
		/*如果当前的字节是"\r"，即回车符，则说明可能读取到一个完整的行*/
        if ( temp == '\r' )
        {
			/*如果“\r”字符碰巧是目前m_read_buffer中最后一个已经被读入的客户数据，那么这次分析
			没有读取到一个完整的行，返回LINE_OPEN以表示还需要继续去客户数据才能进一步分析*/
            if ( ( m_checked_idx + 1 ) == m_read_idx )
            {
                return LINE_OPEN;
            }
			/*如果下一个字符时"\n"，则说明我们成功读取一个完整的行*/
            else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' )
            {
                m_read_buf[ m_checked_idx++ ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
			/*否则的话，说明客户发送的HTTP请求存在语法问题*/
            return LINE_BAD;
        }
		/*如果当前的字节是"\n"，即换行符，则也说明可能读取到一个完整的行*/
        else if( temp == '\n' )
        {
            if( ( m_checked_idx > 1 ) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) )
            {
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
	/*如果所有内容都分析完毕也没有遇到"\r"字符，则返回LINE_OPEN，表示还需要继续读取客户数据才能进一步分析*/
    return LINE_OPEN;
}
/*循环读取客户数据，直到无数据可读或者对方关闭连接*/
//读取客户端发送过来的数据，将其存放到m_read_buf缓冲区中
bool http_conn::read()
{
    if( m_read_idx >= READ_BUFFER_SIZE )
    {
        return false;
    }

    int bytes_read = 0;
    while( true )
    {
        bytes_read = recv( m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0 );//循环读取套接字描述符发送来的数据，缓存到m_read_buf中
        if ( bytes_read == -1 )
        {
            //以非阻塞O_NONBLOCK的标志打开文件/socket/FIFO，如果你连续做read操作而没有数据可读。
            /*	此时程序不会阻塞起来等待数据准备就绪返回，read函数会返回一个错误EAGAIN，提示你的应用程序现在没有数据可读请稍后再试。*/
            //对非阻塞socket而言，EAGAIN不是一种错误。在VxWorks和Windows上，EAGAIN的名字叫做EWOULDBLOCK。
	    if( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                break;
            }
            return false;
        }
        else if ( bytes_read == 0 )
        {
            return false;
        }

        m_read_idx += bytes_read;
    }
    return true;
}
/*解析HTTP请求行，获得请求方法、目标URL，以及HTTP版本号*/
http_conn::HTTP_CODE http_conn::parse_request_line( char* text )
{
    //strpbrk(s1，s2)从s1的第一个字符向后检索，直到'\0'，如果当前字符存在于s2中，那么返回当前字符的地址，并停止检索。
	m_url = strpbrk( text, " \t" );//\t是水平制表符，相当于TAB按键
	/*如果请求行中没有空白字符或“\t”，则HTTP请求必有问题*/
    if ( ! m_url )
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';

    char* method = text;
     //strcasecmp(s1,s2)用来比较参数s1 和s2 字符串，比较时会自动忽略大小写的差异。
    //返回值：若参数s1 和s2 字符串相同则返回0。s1 长度大于s2 长度则返回大于0 的值，s1 长度若小于s2 长度则返回小于0 的值。
    if ( strcasecmp( method, "GET" ) == 0 )
    {
        m_method = GET;
    }
    else
    {
        return BAD_REQUEST;
    }

    m_url += strspn( m_url, " \t" );
    m_version = strpbrk( m_url, " \t" );
    if ( ! m_version )
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn( m_version, " \t" );
	/*仅支持HTTP/1.1*/
    if ( strcasecmp( m_version, "HTTP/1.1" ) != 0 )
    {
        return BAD_REQUEST;
    }
	/*检查URL是否合法*/
    if ( strncasecmp( m_url, "http://", 7 ) == 0 )
    {
        m_url += 7;
        m_url = strchr( m_url, '/' );
    }

    if ( ! m_url || m_url[ 0 ] != '/' )
    {
        return BAD_REQUEST;
    }
	/*HTTP请求行处理完毕，状态转移到头部字段的分析*/
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}
/*解析HTTP请求的一个头部信息*/
http_conn::HTTP_CODE http_conn::parse_headers( char* text )
{
	/*遇到空行，表示头部字段解析完毕*/
    if( text[ 0 ] == '\0' )
    {
        if ( m_method == HEAD )
        {
            return GET_REQUEST;
        }
		/*如果HTTP请求有消息体，则还需要读取m_content_length字节的小西天，
		状态机转移到CHECK_STATE_CONTENT状态*/
        if ( m_content_length != 0 )
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
		/*否则说明我们已经得到了一个完整的HTTP请求*/
        return GET_REQUEST;
    }
	/*处理Connection头部字段*/
    else if ( strncasecmp( text, "Connection:", 11 ) == 0 )
    {
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 )
        {
            m_linger = true;
        }
    }
	/*处理Content-Length头部字段*/
    else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 )
    {
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol( text );
    }
	/*处理Host头部字段*/
    else if ( strncasecmp( text, "Host:", 5 ) == 0 )
    {
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    }
    else
    {
        printf( "oop! unknow header %s\n", text );
    }

    return NO_REQUEST;

}
/*我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了*/
http_conn::HTTP_CODE http_conn::parse_content( char* text )
{
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }

    return NO_REQUEST;
}
/*主状态机出*/
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;	/*记录当前的读取状态*/
    HTTP_CODE ret = NO_REQUEST;	/*记录HTTP请求的处理结果*/
    char* text = 0;
	/*用于从m_read_buffer中取出完整的行*/
    while ( ( ( m_check_state == CHECK_STATE_CONTENT ) && ( line_status == LINE_OK  ) )
                || ( ( line_status = parse_line() ) == LINE_OK ) )
    {
        text = get_line();
        m_start_line = m_checked_idx;		/*记录下一行的其实位置*/
        printf( "got 1 http line: %s\n", text );

        switch ( m_check_state )
        {
            case CHECK_STATE_REQUESTLINE:	/*第一个状态，分析请求*/
            {
                ret = parse_request_line( text );
                if ( ret == BAD_REQUEST )
                {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:	/*第二个状态，分析头部字段*/
            {
                ret = parse_headers( text );
                if ( ret == BAD_REQUEST )
                {
                    return BAD_REQUEST;
                }
                else if ( ret == GET_REQUEST )
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:	/*第三个状态，分析请求包体*/
            {
                ret = parse_content( text );
                if ( ret == GET_REQUEST )
                {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }

    return NO_REQUEST;
}
/*当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性。如果目标文件存在、
对所有用户可读、且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉
调用者获取文件成功*/
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy( m_real_file, doc_root );
    int len = strlen( doc_root );
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    if ( stat( m_real_file, &m_file_stat ) < 0 )//stat函数通过文件名filename获取文件信息，并保存在buf所指的结构体stat中
    {
        return NO_RESOURCE;
    }

    if ( ! ( m_file_stat.st_mode & S_IROTH ) )//若文件没有其他用户读权限
    {
        return FORBIDDEN_REQUEST;
    }

    if ( S_ISDIR( m_file_stat.st_mode ) )//若文件是目录
    {
        return BAD_REQUEST;
    }

    int fd = open( m_real_file, O_RDONLY );//以只读的方式open
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );//将文件mmap映射，PROT_READ 映射区域可被读取，对映射区域的写入操作会产生一个映射文件的复制，即私人的“写入时复制”（copy on write）对此区域作的任何修改都不会写回原来的文件内容。
    close( fd );
    return FILE_REQUEST;
}
/*对内存映射去执行munmap操作*/
void http_conn::unmap()
{
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}
/*写HTTP响应*/
bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = m_write_idx;
    if ( bytes_to_send == 0 )
    {
        modfd( m_epollfd, m_sockfd, EPOLLIN );//EPOLLONESHOT事件每次需要重置事件
        init();
        return true;
    }

    while( 1 )
    {    ////集中写，m_sockfd是http连接对应的描述符，m_iv是iovec结构体数组表示内存块地址，m_iv_count是数组的长度即多少个内存块将一次集中写到m_sockfd
        temp = writev( m_sockfd, m_iv, m_iv_count );
        if ( temp <= -1 )
        {
			/*如果TCP写缓冲区没有空间，则等待下一轮EPOLLOUT事件。虽然在此期间，服务器无法立即接收到
			同一客户的下一个请求，但这可以保证连接的完整性*/
            if( errno == EAGAIN )
            {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );////重置EPOLLONESHOT事件,注册可写事件表示若m_sockfd没有写失败则关闭连接
                return true;
            }
			/*发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接*/
            unmap();
            return false;
        }

        bytes_to_send -= temp;
        bytes_have_send += temp;
        if ( bytes_to_send <= bytes_have_send )
        {
            unmap();
            if( m_linger )
            {
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            }
            else
            {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
    }
}
/*往写缓冲区写入待发送的数据*/
bool http_conn::add_response( const char* format, ... )////HTTP应答主要是将应答数据添加到写缓冲区m_write_buf
{
    if( m_write_idx >= WRITE_BUFFER_SIZE )
    {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) )
    {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

bool http_conn::add_status_line( int status, const char* title )
{
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers( int content_len )
{
    add_content_length( content_len );
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length( int content_len )
{
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}
/*根据服务器处理HTTP请求的结果，决定返回给客户端的内容*/
bool http_conn::process_write( HTTP_CODE ret )
{
    switch ( ret )
    {
        case INTERNAL_ERROR:
        {
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) )
            {
                return false;
            }
            break;
        }
        case BAD_REQUEST:
        {
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) )
            {
                return false;
            }
            break;
        }
        case NO_RESOURCE:
        {
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) )
            {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line( 403, error_403_title );
            add_headers( strlen( error_403_form ) );
            if ( ! add_content( error_403_form ) )
            {
                return false;
            }
            break;
        }
        case FILE_REQUEST:
        {
            add_status_line( 200, ok_200_title );
            if ( m_file_stat.st_size != 0 )
            {
                add_headers( m_file_stat.st_size );
                m_iv[ 0 ].iov_base = m_write_buf;
                m_iv[ 0 ].iov_len = m_write_idx;
                m_iv[ 1 ].iov_base = m_file_address;
                m_iv[ 1 ].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                return true;
            }
            else
            {
                const char* ok_string = "<html><body></body></html>";
                add_headers( strlen( ok_string ) );
                if ( ! add_content( ok_string ) )
                {
                    return false;
                }
            }
        }
        default:
        {
            return false;
        }
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}
/*由线程中的工作线程调用，这是处理HTTP请求的入口函数*/
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_REQUEST )
    {
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        return;
    }

    bool write_ret = process_write( read_ret );
    if ( ! write_ret )
    {
        close_conn();
    }

    modfd( m_epollfd, m_sockfd, EPOLLOUT );
}

