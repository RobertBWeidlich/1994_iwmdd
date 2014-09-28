/************************************************************************
 *  file:	iwmdd.c
 *  date:	January 21, 1994
 *  author:	Bob Weidlich
 *  purpose:	IW Message Dispatcher Daemon: a socket server that allows
 *		multiple clients to connect, register their process
 *		names, and then pass messages transparently from one
 *		client to another.
 *  todo:
 *		1.  convert names to lower case
 *		2.  security
 ************************************************************************/
#ifndef lint
#ifndef SABER
static char sccsid[] = "@(#)iwmdd.c	1.36 1/21/94 GTE-TSC";
#endif
#endif
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/resource.h>

#ifdef sun
#define  MAX_FDS 	(NOFILE)
#else
#define  MAX_FDS	(NOFILES_MAX)
#endif

#define  MAX_CLIENTS ((MAX_FDS)-6)

#define  MAX_PROCNAME_LEN      128
#define  MAXMSGSIZE	      8000
#define  LOGFILESIZE	    250000
#define  CLR_WBUF_INTERVAL	 3	/*  interval in seconds to	*/
					/*  attempt to clear write	*/
					/*  buffers			*/

typedef  struct	_SysMib  {
	/*  static system MIB info.  This is the stuff that is known	*/
	/*  at start-up time.  All other data is dynamic and is		*/
	/*  determined when requested.					*/
	char	*sysProcPathName;
	char	*sysProcName;
	int	 sysProcId;
	char	*sysHostName;
	char	 sysIpAddr[20];
	int	 sysTcpPort;
	char	 sysVersionNum[40];
	char	 sysVersionDate[40];
	int	 sysMaxFds;
	int	 sysMaxClients;
	char	 sysStartTimeGmt[20];
	char	 sysStartTimeLocal[20];
	long	 sysStartTimeSecs;
	long	 sysOctetsReceived;
	long	 sysMsgsReceived;
	long	 sysOctetsSent;
	long	 sysMsgsSent;
	}  SysMib;

typedef  struct _ClientMib  {
	/*  client MIB info.  This maintains interesting data about	*/
	/*  each client.						*/
	char	 clHostName[64];
	char	 clHostIpAddr[20];
	int	 clHostPort;
	long	 clMsgsReceived;
	long	 clOctetsReceived;
	long	 clCompleteReads;
	long	 clPartialReads;
	long	 clReadBufOverflows;
	long	 clMsgsSent;
	long	 clOctetsSent;
	long	 clCompleteWrites;
	long	 clPartialWrites;
	long	 clFailedWrites;
	long	 clWhenConnected;
	long	 clLastHeardFrom;
	}  ClientMib;

typedef  struct  _RBuf  {
	char	*Buf;		/*  data				*/
	int	 SizeOfBuf;	/*  size allocated to Buf		*/
	int	 EndOfBuf;	/*  where in Buf to append 		*/
	int	 Ovfl;		/*  characters overflowed due to	*/
				/*  msg size exceeding MAXMSGSIZE	*/
	}  RBuf;

typedef  struct  _WBuf  {
	char	*Buf;		/*  data				*/
	int	 SizeOfBuf;	/*  size allocated to Buf		*/
	char	*Data;		/*  point to unread data in Buf		*/
	int	 Ovfl;		/*  messages missed due to blocked	*/
				/*  channel				*/
	long	 Mid;		/*  identifier of the message		*/
	}  WBuf;

typedef  struct _ClientRecord  {
	char	  Name[MAX_PROCNAME_LEN+2];
	int	  Active; /*  1 if this record is active; 0 if not	*/
	int	  Sock;   /*  socket file descriptor			*/
	RBuf	  IncMsg; /*  buffer for incomplete incoming messages	*/
	WBuf	  OutMsg; /*  buffer for blocked outgoing messages	*/
	ClientMib ClMib;  /*  MIB info					*/
	}  ClientRecord;

ClientRecord	client_table[MAX_CLIENTS];
SysMib		sysMib;

#define VERSION_NUM	"1.0.3a"
#define VERSION_DATE	"Fri Oct  8 18:05:02 EDT 1993"

char	 vbuf[1000];

int	 local_sock;
int	 width;
int	 l_flag;
char	*pname;
extern	 char	*index(), *rindex();
extern	 int	 errno;
extern	 int	 sys_nerr;
extern	 char	*sys_errlist[];
long	 glmid;
int	 max_clients;

void	 usage();
int	 init_local_sock();
void	 service_msg_from_client();
void	 service_msg_to_me();
void	 send_msg_to_clients();
void	 write_client();
int	 clr_wbuffer();
void	 clr_all_wbuffers();
char	*read_client();
int	 disconnect_channel();
int	 disconnect_process();
char	*get_gmt_timestamp();
char	*get_local_timestamp();
void	 catcher();


/*  info on would-be clients for security  */
struct	  sockaddr_in  sockinfo;
int	  infosize;

main(argc, argv)
int	 argc;
char	*argv[];
{
    int		 ac;
    char	*ar;
    int		 excess_fd;
    fd_set	 read_ready;
    int		 i;
    char	*portstr;
    int		 fd;
    char	*logfname;
    char	 hnbuf[MAXHOSTNAMELEN+1];
    int		 why;
    struct	 timeval	t;
    char	*rmsg;
    long	 mid;
    struct	 rlimit rlp;
    int		 old_max_fds, max_fds;
    char	*s;
    char	 mbuf[2000];
    struct hostent
		*hostinfo;
    time_t	 now;


    pname = strrchr(argv[0], '/');
    if (pname && pname[1])
	++pname;
    else
	pname = argv[0];

    l_flag = 0;
    portstr = (char *)NULL;
    for (ac = 1; ac < argc; ac++)  {
	ar = argv[ac];
	if (*ar != '-')  {
	    /*  must be IP port number - ignore subsequent args, if any	*/
	    portstr = ar;
	    break;
	    }
	++ar;
	switch (*ar)  {
	case 'd':
	case 'l':
	    l_flag = 1;
	    ++ac;
	    if (ac >= argc)  {
		usage(pname);
		exit(1);
		}
	    ar = argv[ac];	/*  make sure filename is absolute	*/
	    if (*ar != '/')  {	/*  pathname				*/
		fprintf(stderr, 
		  "%s: ERROR - filename \"%s\" must be absolute pathname\n",
		  pname, ar);
		exit(1);
		}
	    logfname = ar;
	    break;
	default:
	    usage(pname);
	    exit(1);
	    }
	}
    if (!portstr || atoi(portstr) < 1)  {
	usage(pname);
	exit(1);
	}

    printf("iwmdd Version %s (%s) starting on IP port %d\n",
		VERSION_NUM, VERSION_DATE, atoi(portstr));


     /*	call setrlimit() to set no. open file descriptors to MAX_FDS	*/
 
     if( getrlimit( RLIMIT_NOFILE, &rlp ) != 0 ) {
   	fprintf(stderr, "%s: getrlimit(RLIMIT_NOFILE, ...): ",pname);
 	perror("");
 	exit(1);
	}
    old_max_fds = rlp.rlim_cur;

    rlp.rlim_cur = MAX_FDS;
    if( setrlimit( RLIMIT_NOFILE, &rlp ) != 0 ) {
	fprintf(stderr,
	  "\n%s: setrlimit(RLIMIT_NOFILE, %d): ",pname,rlp.rlim_cur);
 	perror("");
 	exit(1);
	}

     if( getrlimit( RLIMIT_NOFILE, &rlp ) != 0 ) {
   	fprintf(stderr, "%s: getrlimit(RLIMIT_NOFILE, ...): ",pname);
 	perror("");
 	exit(1);
	}
    max_fds = rlp.rlim_cur;

    printf("max_fds originally %d, reset to %d\n",
		old_max_fds, max_fds);

    if (max_fds > MAX_CLIENTS)
	max_clients = MAX_CLIENTS;
    else
	max_clients = max_fds - 6;
    printf("max_clients = %d\n", max_clients);

    glmid = 0;
    signal(SIGPIPE, catcher);

    /*  set static system mib info  */

    now = (long)time((long *)NULL);

    sysMib.sysProcName = pname;

    if (argv[0][0] == '/')
	strcpy(mbuf, argv[0]);
    else  {
	s = getenv("PWD");
	if (s)  {
	    strcpy(mbuf, s);
	    strcat(mbuf, "/");
	    }
	else
	    mbuf[0] = '\0';
	strcat(mbuf, pname);
	}
    s = strdup(mbuf);
    if (s)
	sysMib.sysProcPathName = s;
    else
	sysMib.sysProcPathName = pname;

    sysMib.sysProcId = (int)getpid();

    gethostname(mbuf, sizeof(mbuf));
    mbuf[sizeof(mbuf)-1] = '\0';
    s = strdup(mbuf);
    if (s)
	sysMib.sysHostName = s;
    else
	sysMib.sysHostName = "";

    sysMib.sysIpAddr[0] = '\0';
    hostinfo = gethostbyname(sysMib.sysHostName);
    if (hostinfo)  {
	s = inet_ntoa(*(hostinfo->h_addr_list));
	    if (s)  {
                strcpy(sysMib.sysIpAddr, s);
	    }
	}
    sysMib.sysTcpPort = atoi(portstr);

    strncpy(sysMib.sysVersionNum, VERSION_NUM, sizeof(sysMib.sysVersionNum));
    sysMib.sysVersionNum[sizeof(sysMib.sysVersionNum)-1] = '\0';
    strncpy(sysMib.sysVersionDate, VERSION_DATE, sizeof(sysMib.sysVersionDate));
    sysMib.sysVersionDate[sizeof(sysMib.sysVersionDate)-1] = '\0';

    sysMib.sysStartTimeSecs = (long) now;

    s = get_gmt_timestamp(now);
    if (s)
	strcpy(sysMib.sysStartTimeGmt, s);
    else
	sysMib.sysStartTimeGmt[0] = '\0';

    s = get_local_timestamp(now);
    if (s)
	strcpy(sysMib.sysStartTimeLocal, s);
    else
	sysMib.sysStartTimeLocal[0] = '\0';

    sysMib.sysMaxFds = max_fds;
    sysMib.sysMaxClients = max_clients;

#ifdef DEBUG
    /*  don't run in background as a daemon...  */
    printf("running in DEBUG mode...\n");
    goto skipover;
#endif

    if (getuid())  {	/*  should run only as root  */
	/* Don't kludge this out, Eric!  */
	printf("warning: %s should be run as root...", pname);
	fflush(stdout);
	for (i = 10; i > 0; i--)  {
	    printf("."); fflush(stdout);
	    sleep(1);
	    }
	printf("\n");
	printf("%s running in background... goodbye!\n", pname);
	printf("\n");
	}

    /*  this stuff done because iwmdd is running as a daemon.  Works	*/
    /*  only for BSD, needs some work to work for SysV as well.  This	*/
    /*  scheme based on Unix Network Programming by Richard Stevens, 	*/
    /*  section 2.6.							*/

    /*  ignore terminal I/O signals  */
#ifdef SIGTTOU
    signal(SIGTTOU, SIG_IGN);
#endif
#ifdef SIGTTIN
    signal(SIGTTIN, SIG_IGN);
#endif
#ifdef SIGTSTP
    signal(SIGTSTP, SIG_IGN);
#endif

    /*  if process not started in background, forking, killing the	*/
    /*  parent, and letting the child resume execution ensures that	*/
    /*  process will run in the background.				*/
    if (fork())
	exit(0);

    setpgrp(0, getpid());

    if ((fd = open("/dev/tty", O_RDWR)) >= 0)  {
	ioctl(fd, TIOCNOTTY, (char *)NULL);
	close(fd);
	}

    errno = 0;
    if (chdir("/var/tmp") < 0)  {
	/*  move someplace in root partition  */
	if (errno < sys_nerr)
	    fprintf(stderr, "%s: /var/tmp: %s\n", pname, sys_errlist[errno]);
	else
	    fprintf(stderr, "chdir(\"/var/tmp\") failed\n");
	exit(1);
	}

    for (i = 1; i < NOFILE; i++)
	close(i);
    /*  NOTE:  no more read/writes to/from stdin, stdout, or stderr	*/
    /*	       beyond this point.					*/
    (void)umask(0);

skipover:

    if (l_flag)  {
	if (!open_mdlog(logfname, LOGFILESIZE))
	    l_flag = 0;
	}
    if (gethostname(hnbuf, sizeof(hnbuf)) != 0)
	strcpy(hnbuf, "(unknown)");
    else
	hnbuf[sizeof(hnbuf)-1] = '\0';
    if (l_flag)  {
	sprintf(vbuf, "iwmdd daemon starting on host \"%s\", pid=%d",
		hnbuf, getpid());
	mdlog(vbuf);
	}

    if (!init_local_sock(atoi(portstr)))
	exit(1);

    for (i = 0; i < max_clients; i++)  {  /*  clear out client_table	*/
	client_table[i].Active = 0;
	client_table[i].Name[0] = '\0';
	client_table[i].Sock = 0;
	client_table[i].IncMsg.Buf = (char *)NULL;
	client_table[i].IncMsg.SizeOfBuf = 0;
	client_table[i].IncMsg.EndOfBuf = 0;
	client_table[i].IncMsg.Ovfl = 0;
	client_table[i].OutMsg.Buf = (char *)NULL;
	client_table[i].OutMsg.Data = (char *)NULL;
	client_table[i].OutMsg.SizeOfBuf = 0;
	client_table[i].OutMsg.Ovfl = 0;
	(void)memset((void *)&(client_table[i].ClMib), 0, sizeof(ClientMib));
	}

    t.tv_sec = CLR_WBUF_INTERVAL;
    t.tv_usec = 500000;

    for (;;)  {
	FD_ZERO(&read_ready);
	FD_SET(local_sock, &read_ready);  /*  check incoming connections  */
	for(i = 0; i < max_clients; i++)  {
	    if (client_table[i].Active != 0)  /*  check active clients	*/
		FD_SET(client_table[i].Sock, &read_ready);
	    }

	/*  block until remote_sock or active client has data to be	*/
	/*  read.							*/
	why = select(width+1, &read_ready, (fd_set *)NULL, (fd_set *)NULL, &t);

	if (FD_ISSET(local_sock, &read_ready))  {
	    /*  new connection requested		   		*/
	    /*  get first free socket descriptor in client table	*/
	    for (i = 0; i < max_clients; i++)  {
		if (client_table[i].Active == 0)
		    break;
		}
	    if (i >= max_clients)  {  /*  no free descriptors left	*/
		infosize = sizeof(sockinfo);
		excess_fd = accept(local_sock, (struct sockaddr *)&sockinfo,
					&infosize);
		if (l_flag && (excess_fd == -1))  {
		    if (errno < sys_nerr)
			sprintf(vbuf, "excess_fd accept() failed: %s",
				sys_errlist[errno]);
		    else  {
			sprintf(vbuf, "excess_fd accept() failed\n");
			}
		    mdlog(vbuf);
		    usleep(50000);
		    }
		else if (excess_fd > 0)  {
		    close(excess_fd);
		    if (l_flag)  {
			sprintf(vbuf, "connection failed - client_table full");
			mdlog(vbuf);
			}
		    }
		}
	    else  {
		infosize = sizeof(sockinfo);
		client_table[i].Sock = accept(local_sock,
				(struct sockaddr *)&sockinfo, &infosize);
		if (client_table[i].Sock == -1)  {
		    if (l_flag)  {
			if (errno < sys_nerr)
			    sprintf(vbuf, "regular accept() failed: %s",
				    sys_errlist[errno]);
			else  
			    sprintf(vbuf, "regular accept() failed\n");
			mdlog(vbuf);
			}
		    client_table[i].Sock = 0;
		    usleep(50000);
		    }
		else  {
		    fcntl(client_table[i].Sock, F_SETFL, O_NDELAY);
		    client_table[i].Active = 1;
		    client_table[i].Name[0] = '\0';
		    if (width < client_table[i].Sock)
			width = client_table[i].Sock;

		    /*  fill in static entries in client table.  Assume	*/
		    /*  that everything initialized to zero.		*/

		    now = (long)time((long *)NULL);
		    client_table[i].ClMib.clWhenConnected = now;
		    client_table[i].ClMib.clLastHeardFrom = now;

		    s = inet_ntoa(sockinfo.sin_addr);
		    if (s)
			strncpy(client_table[i].ClMib.clHostIpAddr, s,
				sizeof(client_table[i].ClMib.clHostIpAddr)-1);

		    hostinfo = gethostbyaddr(sockinfo.sin_addr,
				sizeof(sockinfo.sin_addr), AF_INET);
		    if (hostinfo)
			strncpy(client_table[i].ClMib.clHostName,
				hostinfo->h_name,
				sizeof(client_table[i].ClMib.clHostName)-1);

		    client_table[i].ClMib.clHostPort = (int)sockinfo.sin_port;

		    if (l_flag)  {
			sprintf(vbuf, "new client; ch=%d, fd=%d\n", i,
				client_table[i].Sock);
			mdlog(vbuf);
			}
		    }
		}
	    }
	clr_all_wbuffers();
	for (i = 0; i < max_clients; i++)  {
	    if (client_table[i].Active == 0)
		continue;
	    if (FD_ISSET(client_table[i].Sock, &read_ready))  {
		/*  socket ready to read  */
		rmsg = read_client(i, &mid);
		if (rmsg)  {
		    now = (long)time((long *)NULL);
		    client_table[i].ClMib.clLastHeardFrom = now;
		    service_msg_from_client(i, rmsg, mid);
		    }
		}
	    }
	}
}

void	 usage(p)
char	*p;
{
    fprintf(stderr, "usage: %s [-l logfile] ip-port\n", p);
    fprintf(stderr, "  -l sets log file name - MUST be absolute path\n");
    fprintf(stderr, "  ip-port is IP port number\n");
}


int	init_local_sock(port)
int	port;
{

    struct sockaddr_in	 server;
    int			 sockadd_in_size;
    int			 i;

    sockadd_in_size = sizeof(struct sockaddr_in);
    local_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (local_sock < 0)  {
	if (l_flag)  {
	    if (errno < sys_nerr)
		sprintf(vbuf, "opening stream socket failed: %s",
			sys_errlist[errno]);
		else
		    sprintf(vbuf, "opening stream socket failed");
	    mdlog(vbuf);
	    }
	return(0);
	}
    width = local_sock;

    bzero(&server, sockadd_in_size);
    server.sin_family = AF_INET;
    server.sin_port = (u_short) port;

    setsockopt(local_sock, SOL_SOCKET, SO_REUSEADDR,
	       (char *)&i, sizeof(i));


    if (bind(local_sock, (struct sockaddr *) &server, sockadd_in_size) < 0)  {
	if (l_flag)  {
	    if (errno < sys_nerr)
		sprintf(vbuf, "binding stream socket failed: %s",
			sys_errlist[errno]);
	    else
		sprintf(vbuf, "binding stream socket failed");
	    mdlog(vbuf);
	    }
	return(0);
	}

    listen(local_sock, 5);
    return(1);
}


void	 service_msg_from_client(ind, msg, mid)
int	 ind;
char	*msg;
long	 mid;
/*
 *  ind is the client table index of the client that the message is from.
 *
 *  We expect the msg to be in the form:
 *
 *    <src> <dest-list> <message>
 *
 *  where src is the process name of the source, dest-list is a comma-
 *  separated list of process names, and the message is the body of the
 *  message.
 *
 *  if dest-list contains "all" deliver the message to all active clients.
 *
 *  if one of the destinations is "iwmd" then the message is intended for
 *  us.
 *
 *  If the name of the src process is not registered in the client
 *  table, then insert it in the client table (this is the method by which
 *  we "register" process names.)
 *
 *  mid is a unique identifier for the message.
 */
{
    char	 src[MAX_PROCNAME_LEN+3];
    char	 dest[MAX_PROCNAME_LEN+3];
    char	*destlist, *body;
    char	*s;
    int		 i;

    /*  copy first token to src buffer  */
    for (s = msg; *s; s++)  {  /*  first strip leading blanks  */
	if ((*s != '\t') && (*s != ' '))
	    break;
	}
    if (!*s)	/*  nothing in buffer but spaces and/or tabs, so ignore	*/
	return;
    memset(src, '\0', sizeof(src));
    for (i = 0; i < MAX_PROCNAME_LEN; i++)  {
	if ((*s == ' ') || (*s == '\t'))
	    break;
	src[i] = *s++;
	}
    src[i] = '\0';

    /*  second token is comma-separated lists of destinations  */
    for (; *s; s++)  {  /*  again strip leading blanks  */
	if ((*s != '\t') && (*s != ' '))
	    break;
	}
    if (!*s)	/*  no second token  */
	return;
    destlist = s;

    /*  now find third token, the body of the message  */
    for (; *s; s++)  {  /*  skip 2nd token  */
	if ((*s == '\t') || (*s == ' '))
	    break;
	}
    if (!*s)
	return;
    for (; *s; s++)  {  /*  skip leading blanks  */
	if ((*s != '\t') && (*s != ' '))
	    break;
	}
    if (!*s)	/*  no third token  */
	return;
    body = s;

    /*  register client if not yet registered  */
    if (strlen(client_table[ind].Name) < 1)  {
	strncpy(client_table[ind].Name, src, MAX_PROCNAME_LEN);
	client_table[ind].Name[MAX_PROCNAME_LEN] = '\0';
	if (l_flag)  {
	    sprintf(vbuf, "client channel=%d registering as \"%s\"",
		    ind, client_table[ind].Name);
	    mdlog(vbuf);
	    }
	}

    /*  parse each client in the comma-separated list of destination	*/
    /*  clients								*/
    for (;;)  {
	if ((!(*destlist)) || (*destlist == ' ') || (*destlist == '\t'))
	    break;
	for (i = 0; i < MAX_PROCNAME_LEN; i++)  {
	    if (*destlist == ',')  {
		++destlist;
		break;
		}
	    if ((*destlist == ' ') || (*destlist == '\t'))
		break;
	    dest[i] = *destlist++;
	    }
	if (i >= MAX_PROCNAME_LEN)
	    return;
	dest[i] = '\0';
	if (strncmp(dest, "iwmd", 4) == 0)	/*  message is to iwmd	*/
	    service_msg_to_me(src, dest, body, msg, mid);
	else  /*  dispatch message  */
	    send_msg_to_clients(ind, dest, msg, mid);
	}
}


/*ARGSUSED*/
void	 service_msg_to_me(src, dest, body, msg, mid)
/*
 *  deal with messages directed to iwmd.  See the file "iwmdd.mib-info"
 *  for details of the syntax of the messages.
 */
char	*src;
char	*dest;
char	*body;
char	*msg;
long	 mid;
{
    void	 show_client_table();
    void	 show_client_namelist();
    void	 dump_sys_mib();
    void	 dump_client_mib();
    char	*ms;
    char	*t3, *t4, *t5, *t6;

    if (!msg)
	return;

    if (l_flag)  {
	sprintf(vbuf, "message is for myself; mid=%ld", mid);
	mdlog(vbuf);
	}

    ms = strdup(body);	/* so we can strtok on our own copy		*/

    t3 = t4 = t5 = t6 = (char *)NULL;
    t3 = strtok(ms, " \t");
    if (t3)
	t4 = strtok((char *)NULL, " \t");
    if (t4)
	t5 = strtok((char *)NULL, " \t");
    if (t5)
	t6 = strtok((char *)NULL, " \t");

    if (!t3)  {
	cfree(ms);
	return;
	}

    if (strcmp(t3, "show") == 0)  {
	if (t4 && (strncmp(t4, "client", 6) == 0))  {
	    /* body = "show client" */
	    show_client_namelist(src);
	    }
	else  {
	    /*  body = "show"  */
	    show_client_table(src);
	    }
	}
    if ( (strcmp(t3, "dump") == 0) && (t4) )  {
	if (strncmp(t4, "sys", 3) == 0)  {
	    /*   body = "dump sys"  */
	    dump_sys_mib(src);
	    }
	else if ( (strncmp(t4, "cl", 2) == 0) && (t5) )  {
	    /*  body = "dump client <range-spec> [<reg-list>]" */
	    dump_client_mib(src, t5, t6);
	    }
	}


    cfree(ms);
}


void	 send_msg_to_clients(ind, destproc, msg, mid)
int	 ind;
char	*destproc;
char	*msg;
long	 mid;
/*
 *  send socket message to all registered clients whose first
 *  strlen(destproc) characters in their process names match those of
 *  destproc.
 *
 *  treat "all" as a special case which means to send msg to all
 *  registered clients.
 *
 *  the argument ind, if 0 or greater, is client_table index of process
 *  that originated the message.  If ind is set to -1, then the iwmdd
 *  itself is the source.
 */
{
    int		i, len, found;
    int		allflag;

    len = strlen(destproc);
    if (len < 1)
	return;

    if (strcmp(destproc, "all") == 0)  {
	allflag = 1;
	found = 1;
	}
    else  {
	allflag = 0;
	found = 0;
	}

    for (i = 0; i < max_clients; i++)  {
	if (!client_table[i].Active)
	    continue;
	if (client_table[i].Name[0] == '\0')
	    continue;	/*  unregistered clients are ignored  */
	if ( (allflag) ||
	     (strncmp(client_table[i].Name, destproc, len) == 0) )  {
	    found = 1;
	    write_client(ind, i, msg, mid);
	    }
	}
    if (!found)  {  /*  note that we don't complain if destproc is	*/
		    /*  "all" and nobody is registered.			*/
	if (l_flag)  {
	    sprintf(vbuf,
"cannot dispatch message; process \"%s\" not registered,\n\t\t\t  msg=\"",
		    destproc);
	    i = sizeof(vbuf) - strlen(vbuf);
	    i -= 10;
	    if (i > 10)  {
		strncat(vbuf, msg, i);	   /* truncate msg if necessary	*/
		if (strlen(msg) > i)
		    strcat(vbuf, "<...>"); /* indicates truncation	*/
		}
	    strcat(vbuf, "\"");
	    mdlog(vbuf);
	    }
	}
}


int	disconnect_channel(i)
/*
 *  disconnect channel i and return 1 if it is active, otherwise, return 0.
 */
{
    if ((i < 0) || (i >= max_clients))
	return(0);
    close(client_table[i].Sock);
    client_table[i].Sock = 0;
    client_table[i].Active = 0;
    client_table[i].Name[0] = '\0';

    /*  reset read buffer, but don't deallocate buffer memory  */
    if (client_table[i].IncMsg.Buf)
	memset(client_table[i].IncMsg.Buf, '\0',
	       client_table[i].IncMsg.SizeOfBuf);
    client_table[i].IncMsg.EndOfBuf = 0;
    client_table[i].IncMsg.Ovfl = 0;

    /*  reset write buffer, but don't deallocate buffer memory  */
    if (client_table[i].OutMsg.Buf != NULL)
	memset(client_table[i].OutMsg.Buf, '\0',
	       sizeof(client_table[i].OutMsg.Buf));
    client_table[i].OutMsg.Data = client_table[i].OutMsg.Buf;
    client_table[i].OutMsg.Ovfl = 0;

    /*  clean out Mib entry for this record in client table  */
    memset((void *)&(client_table[i].ClMib), 0, sizeof(ClientMib));

    return(1);
}


int	 disconnect_process(proc)
char	*proc;
/*
 *  disconnect all processes whose first strlen(proc) characters of their
 *  process names match proc.  Return 1 if at least one match if found,
 *  otherwise return 0.
 */
{
    int		i, len, found;

    found = 0;
    len = strlen(proc);
    for(i = 0; i < max_clients; i++)  {
	if (!client_table[i].Active)
	    continue;
	if (strncmp(proc, client_table[i].Name, len) == 0)  {
	    (void)disconnect_channel(i);
	    found = 1;
	    }
	}
    return(found);
}


void	 write_client(ifrom, ito, msg, mid)
int	 ifrom;
int	 ito;
char	*msg;
long	 mid;
/*
 *  write message msg to client.  ifrom is the index of the sending process
 *  in the client_table; if ifrom is -1 the source is the iwmdd itself,
 *  otherwise ifrom ranges from 0 to max_clients-1.  ito is the index of
 *  the destination process in the client_table.  msg is a null terminated
 *  message.  mid is the message identifier, used for logging.
 *
 *  If we are unable to write the complete msg, then buffer it.
 */
{
    int		 requested, actual;
    int		 bufsize;
    char	*s;


    /*  we cannot write if data already stuck in the queue.  See if we	*/
    /*  can flush out the queue.  If we fail, then exit.		*/
    if ( (client_table[ito].OutMsg.Data != NULL) &&
	 (client_table[ito].OutMsg.Data[0] != '\0') )  {
	if (!clr_wbuffer(ito, 1))  {
	    ++(client_table[ito].OutMsg.Ovfl);
	    return;
	    }
	}

    requested = strlen(msg)+1;
    errno = 0;

    actual = write(client_table[ito].Sock, msg, requested);

    if (actual == requested)  {	/*  complete write  */
	client_table[ito].ClMib.clOctetsSent += actual;
	sysMib.sysOctetsSent += actual;
	++(client_table[ito].ClMib.clMsgsSent);
	++(sysMib.sysMsgsSent);
	++(client_table[ito].ClMib.clCompleteWrites);
	if (l_flag)  {
	    memset(vbuf, '\0', sizeof(vbuf));
	    if (ifrom < 0)
		sprintf(vbuf, "dispatch: src=\"%s\", dest=\"%s\"\n\t\t\t  ",
		    "iwmd", client_table[ito].Name, actual, mid);
	    else
		sprintf(vbuf, "dispatch: src=\"%s\", dest=\"%s\"\n\t\t\t  ",
		    client_table[ifrom].Name, client_table[ito].Name,
		    actual, mid);
	    s = vbuf + strlen(vbuf);
	    sprintf(s, "mid=%ld,bytes-written=%d\n\t\t\t  msg=\"",
		    mid, actual);
	    strncat(vbuf, msg, 90); /*  truncate message if necessary  */
	    if (strlen(msg) > 90)
		strcat(vbuf, "<...>"); /*  indicates truncation	*/
	    strcat(vbuf, "\"");
            mdlog(vbuf);
	    }
	return;
	}

    else if (actual > 0)  {	/* partial write  */
	client_table[ito].ClMib.clOctetsSent += actual;
	sysMib.sysOctetsSent += actual;
	++(client_table[ito].ClMib.clPartialWrites);
	if (l_flag)  {
	    if ((errno < sys_nerr) && (errno > 0))
		sprintf(vbuf,
		  "partial write(); to channel=%d,name=\"%s\": %s\n\t\t\t  ",
			ito, client_table[ito].Name, sys_errlist[errno]);
	    else
		sprintf(vbuf,
		  "partial write(); to channel=%d,name=\"%s\"\n\t\t\t  ",
			ito, client_table[ito].Name);
	    s = vbuf + strlen(vbuf);
	    sprintf(s, "mid=%ld,bytes-written=%d", mid, actual);
	    mdlog(vbuf);
	    }
	}
    else  {	/*  probably blocked write  */
	++(client_table[ito].ClMib.clFailedWrites);
	if (l_flag)  {
	    if ((errno < sys_nerr) && (errno > 0))
		sprintf(vbuf,
		  "write() failed; to channel=%d,name=\"%s\": %s\n\t\t\t  ",
			ito, client_table[ito].Name, sys_errlist[errno]);
	    else
		sprintf(vbuf,
		  "write() failed; to channel=%d,name=\"%s\"\n\t\t\t  ",
			ito, client_table[ito].Name);
	    s = vbuf + strlen(vbuf);
	    sprintf(s, "mid=%ld,bytes-written=%d", mid, actual);
	    mdlog(vbuf);
	    }
	}

    if (actual > 0)
	bufsize = (requested - actual) + 1;
    else
	bufsize = requested + 1;

    /*  if buffer not yet allocated, or too small, allocate		*/
    /*  (reallocate) it							*/
    if (client_table[ito].OutMsg.Buf == NULL)  {
	s = (char *)calloc(bufsize, 1);
	if (!s)  {
	    if (errno < sys_nerr)
		sprintf(vbuf,
	"cannot create write buffer - calloc() error: %s", sys_errlist[errno]);
	    else
		sprintf(vbuf, "cannot create write buffer - calloc() error");
	    mdlog(vbuf);
	    return;
	    }
	else  {
	    sprintf(vbuf, 
	      "alloc write buf; client_table index=%d, size=%d", ito, bufsize);
	    mdlog(vbuf);
	    }
	client_table[ito].OutMsg.SizeOfBuf = bufsize;
	s[0] = '\0';
	client_table[ito].OutMsg.Buf = s;
	client_table[ito].OutMsg.Data = s;
	}
    else if (client_table[ito].OutMsg.SizeOfBuf < bufsize) {
	s = (char *)realloc((char *)client_table[ito].OutMsg.Buf, bufsize);
	if (!s)  {
	    if (errno < sys_nerr)
		sprintf(vbuf,
	"cannot create write buffer - realloc() error: %s", sys_errlist[errno]);
	    else
		sprintf(vbuf, "cannot create write buffer - realloc() error");
	    mdlog(vbuf);
	    return;
	    }
	else  {
	    sprintf(vbuf, 
	  "realloc write buf; client_table index=%d, size=%d", ito, bufsize);
	    mdlog(vbuf);
	    }
	client_table[ito].OutMsg.SizeOfBuf = bufsize;
	s[0] = '\0';
	client_table[ito].OutMsg.Buf = s;
	client_table[ito].OutMsg.Data = s;
	}
    memset(client_table[ito].OutMsg.Buf, '\0',
	   client_table[ito].OutMsg.SizeOfBuf);

    /*  buffer unread data  */
    if (actual < 1)
	strcpy(client_table[ito].OutMsg.Buf, msg);
    else
	strcpy(client_table[ito].OutMsg.Buf, (char *)(msg+actual));
    client_table[ito].OutMsg.Mid = mid;
}


char	*read_client(cti, mi)
int	 cti;
long	*mi;
/*
 *
 *  Read data, one byte at a time, from socket of client whose index
 *  in the client_table is is cti.  Buffer data until we read a null
 *  character, which delimits messages.  Return message when we receive
 *  the null character, otherwise return NULL.  For every message we
 *  receive, increment a message id counter (msg_id).  Set *mi to
 *  that value if a complete message is found, otherwise set *mi to -1.
 *
 *  There is a three-state FSM (Finite State Machine) associated with
 *  each socket.  Those states are:
 *
 *	1.  STATE_EMPTY		Buffer is empty
 *	2.  STATE_PARTIAL	Buffer is partially full
 *	3.  STATE_FULL		Buffer overflow
 *
 */
{
    RBuf	*rbuf;
    long	 rqueue_size;
    int		 i, n;
    char	 c;
    char	*s;
    int		 result;
    int		 c_count, bb_count;	/*  count characters read/char-	*/
					/*  acters dumped in bit bucket	*/

#define	 STATE_EMPTY	0
#define	 STATE_PARTIAL	1
#define	 STATE_FULL	2
    int		 state;

    static	char	buf[MAXMSGSIZE+3];

    *mi = -1;
    rbuf = &(client_table[cti].IncMsg);

    if (rbuf->Ovfl > 0)
	state = STATE_FULL;
    else if (rbuf->EndOfBuf == 0)
	state = STATE_EMPTY;
    else
	state = STATE_PARTIAL;

    if (ioctl(client_table[cti].Sock, FIONREAD, &rqueue_size) < 0)  {
	    if (errno < sys_nerr)
		sprintf(vbuf,
		  "read_client() ioctl() failed: %s", sys_errlist[errno]);
	    else
		sprintf(vbuf, "read_client() ioctl() failed");
	mdlog(vbuf);
	return((char *)NULL);
	}

    if (rqueue_size < 1)  {
	sprintf(vbuf, "client disconnected: channel=%d name=\"%s\"",
		cti, client_table[cti].Name);
	(void)disconnect_channel(cti);
	mdlog(vbuf);
	return((char *)NULL);
	}

    if (rbuf->Buf == NULL)  {
	rbuf->Buf = (char *)calloc(MAXMSGSIZE+3, 1);
	if (rbuf->Buf == NULL)  {
	    if (errno < sys_nerr)
		sprintf(vbuf,
	"cannot create read buffer - calloc() error: %s", sys_errlist[errno]);
	    else
		sprintf(vbuf, "cannot create read buffer - calloc() error");
	    mdlog(vbuf);
	    return((char *)NULL);
	    }
	rbuf->SizeOfBuf = MAXMSGSIZE+3;
	}

    c_count = bb_count = 0;
    for (i = 0; i < rqueue_size; i++)  {
	c = 0;
	result = read(client_table[cti].Sock, &c, 1);
	/*printf("c [%d]: \'%c\' (0x%x)\n", rbuf->EndOfBuf, (int)c);*/
	if (result < 0)  {	/*  bad read  */
	    if (l_flag)  {
		if (errno < sys_nerr)
		    sprintf(vbuf, "bad read(): channel=%d,name=\"%s\", %s",
			    cti, sys_errlist[errno]);
		else
		    sprintf(vbuf, "bad read(): channel=%d,name=\"%s\"",
			    cti, client_table[cti].Name);
		mdlog(vbuf);
		}
	    return((char *)NULL);
	    }
	if (result == 0)  {	/*  client disconnected		*/
	    sprintf(vbuf, "client disconnected: channel=%d name=\"%s\"",
		cti, client_table[cti].Name);
	    mdlog(vbuf);
	    (void)disconnect_channel(cti);
	    return((char *)NULL);
	    }
	/*  good read; what we do now is determined by state  */
	++(client_table[cti].ClMib.clOctetsReceived);
	++(sysMib.sysOctetsReceived);
	++c_count;

	if (state == STATE_EMPTY)  {
	    /*  Buffer is empty.  If read null character, ignore it.	*/
	    /*  Otherwise buffer character and enter STATE_PARTIAL.	*/
	    if (c == '\0')
		continue;
	    *(rbuf->Buf + rbuf->EndOfBuf) = c;
	    ++(rbuf->EndOfBuf);
	    state = STATE_PARTIAL;
	    continue;
	    }

	if (state == STATE_PARTIAL)  {
	    /*  Buffer is partially full.  If read null character, then	*/
	    /*  we have read complete message.  Empty buffer and return	*/
	    /*  message.  Otherwise, buffer character, checking for	*/
	    /*  buffer size overflow.					*/
	    if (c == '\0')  {
		++(client_table[cti].ClMib.clMsgsReceived);
		++(sysMib.sysMsgsReceived);
		*mi = glmid;
		++glmid;
		*(rbuf->Buf + rbuf->EndOfBuf) = c;
		++(rbuf->EndOfBuf);
		*(rbuf->Buf + rbuf->EndOfBuf) = '\0';
		memset(buf, '\0', sizeof(buf));
		(void)memcpy(buf, rbuf->Buf, rbuf->EndOfBuf);

		/*  EDP: if we finished read and got nothing, then	*/
		/*  client got disconnected				*/
		if (strlen(buf) == 0)  {
		    sprintf(vbuf,
			"client disconnected (edp): channel=%d name=\"%s\"",
			cti, client_table[cti].Name);
		    mdlog(vbuf);
		    (void)disconnect_channel(cti);
		    return((char *)NULL);
		    }

		if (l_flag)  {
		    sprintf(vbuf,
		"received complete message; channel=%d,name=\"%s\",\n\t\t\t  ",
		      cti, client_table[cti].Name);
		    s = vbuf + strlen(vbuf);
		    sprintf(s,
		"mid=%ld,received-size=%d,accumulated-size=%d,\n\t\t\t  msg=\"",
			    *mi, c_count, rbuf->EndOfBuf);
		    n = sizeof(vbuf) - strlen(vbuf);
		    n -= 10;
		    if (n > 10)  {
			/*  truncate message if unsufficient space	*/
			/*  remains on vbuf				*/
			strncat(vbuf, rbuf->Buf, n);
			if (strlen(rbuf->Buf) > n)
			    strcat(vbuf, "<...>");
			}
		    strcat(vbuf, "\"");
		    mdlog(vbuf);
		    }
		++(client_table[cti].ClMib.clCompleteReads);
		rbuf->EndOfBuf = 0;
		rbuf->Ovfl = 0;
		state = STATE_EMPTY;
		return(buf);
		}
	    else  {
		*(rbuf->Buf + rbuf->EndOfBuf) = c;
		++(rbuf->EndOfBuf);
		if (rbuf->EndOfBuf > MAXMSGSIZE)  {
		    bb_count = rbuf->EndOfBuf;
		    state = STATE_FULL;
		    ++(rbuf->Ovfl);
		    }
		}
	    continue;
	    }

	if (state == STATE_FULL)  {
	    /*  Buffer has overflowed.  Discard data until null		*/
	    /*  character received.					*/
	    ++bb_count;
	    if (c == '\0')  {
		if (l_flag)  {
		    memset(vbuf, '\0', sizeof(vbuf));
		    sprintf(vbuf,
"received too-large message; cannot dispatch due to read buffer overflow;\n\t\t\t  ");
		    s = vbuf + strlen(vbuf);
		    sprintf(s, "channel=%d,name=\"%s\",\n\t\t\t  ",
			cti, client_table[cti].Name);
		    s = vbuf + strlen(vbuf);
		    sprintf(s,
"received-size=%d,accumulated-size=%d (MAXMSGSIZE=%d),\n\t\t\t  ",
			    c_count, rbuf->EndOfBuf, MAXMSGSIZE);
		    s = vbuf + strlen(vbuf);
		    sprintf(s, "read-buffer=\"");
		    s = vbuf + strlen(vbuf);
		    strncat(s, rbuf->Buf, 60);
		    s += 60;
		    strcat(s, "<...>\"");
		    mdlog(vbuf);
		    }
		rbuf->EndOfBuf = 0;
		rbuf->Ovfl = 0;
		state = STATE_EMPTY;
		return((char *)NULL);
		}
	    ++(rbuf->Ovfl);
	    continue;
	    }
	}
    if (l_flag)  {
	if (state == STATE_EMPTY)  {
	    sprintf(vbuf, "received %d nulls; channel=%d,name=\"%s\"",
		    c_count, cti, client_table[cti].Name);
	    mdlog(vbuf);
	    }
	else if (state == STATE_PARTIAL)  {
	    sprintf(vbuf,
	      "received partial message; channel=%d,name=\"%s\",\n\t\t\t  ",
		    cti, client_table[cti].Name);
	    s = vbuf + strlen(vbuf);
	    sprintf(s, "received-size=%d,accumulated-size=%d",
			    c_count, rbuf->EndOfBuf);
	    mdlog(vbuf);
	    ++(client_table[cti].ClMib.clPartialReads);
	    }
	else if (state == STATE_FULL)  {
	    memset(vbuf, '\0', sizeof(vbuf));
	    sprintf(vbuf,
	      "received overflowed data; channel=%d,name=\"%s\",\n\t\t\t  ",
		    cti, client_table[cti].Name);
	    s = vbuf + strlen(vbuf);
	    sprintf(s,
	      "received-size=%d,accumulated-size=%d (MAXMSGSIZE=%d)\n\t\t\t  ",
			    c_count, rbuf->EndOfBuf, MAXMSGSIZE);
	    s = vbuf + strlen(vbuf);
	    sprintf(s, "read-buffer=\"");
	    s = vbuf + strlen(vbuf);
	    strncat(s, rbuf->Buf, 60);
	    s += 60;
	    strcat(s, "<...>\"");
	    mdlog(vbuf);
	    ++(client_table[cti].ClMib.clReadBufOverflows);
	    }
	}
    return((char *)NULL);
}


int	 clr_wbuffer(to, dolog)
int	 to;
int	 dolog;
/*
 *  attempt to clear write buffer by writing data to socket.  return 1
 *  if buffer emptied, 0 if not.  log failed attempt if log set to non-
 *  zero.
 */
{
    int		 requested, actual;
    char	*s;

    if ( (client_table[to].OutMsg.Data == NULL) ||
	 (client_table[to].OutMsg.Data[0] == '\0') )
	return(1);	/*  buffer already empty  */

    requested = strlen(client_table[to].OutMsg.Data)+1;
    errno = 0;

    actual = write(client_table[to].Sock, client_table[to].OutMsg.Data,
					  requested);

    if (actual < 1)  {
	if (dolog && l_flag)  {
	++(client_table[to].ClMib.clFailedWrites);
	    if (errno > 0 && errno < sys_nerr)  {
		sprintf(vbuf,
		"still can't clear write buffer: %s channel=%d,name=\"%s\",\n",
		        sys_errlist[errno], to, client_table[to].Name);
		}
	    else  {
		sprintf(vbuf,
		"still can't clear write buffer:  channel=%d,name=\"%s\"\n",
		    to, client_table[to].Name);
		}
	    s = vbuf + strlen(vbuf);
	    sprintf(s, "\t\t\t  mid=%ld\n", client_table[to].OutMsg.Mid);
	    mdlog(vbuf);
	    }
	if (errno == EPIPE)  {
	    sprintf(vbuf,
	    "forcing client disconnect due to write() EPIPE error:\n\t\t\t  ");
	    s = vbuf + strlen(vbuf);
	    sprintf(s, "channel=%d,name=\"%s\"",
		    to, client_table[to].Name);
	    mdlog(vbuf);
	    disconnect_channel(to);
	    }
	return(0);
	}
    if (actual == requested)  {	/*  complete write  */
	client_table[to].ClMib.clOctetsSent += actual;
	sysMib.sysOctetsSent += actual;
	++(client_table[to].ClMib.clCompleteWrites);
	++(client_table[to].ClMib.clMsgsSent);
	++(sysMib.sysMsgsSent);
	client_table[to].OutMsg.Data = client_table[to].OutMsg.Buf;
	memset(client_table[to].OutMsg.Data, '\0',
	       client_table[to].OutMsg.SizeOfBuf);
	if (dolog && l_flag)  {
	    sprintf(vbuf,
		"cleared write buffer; to channel=%d, name=\"%s\"\n\t\t\t  ",
		to, client_table[to].Name);
	    s = vbuf + strlen(vbuf);
	    sprintf(s,
		"mid=%ld,bytes-written=%d,messages-overflowed=%d",
	    client_table[to].OutMsg.Mid, actual, client_table[to].OutMsg.Ovfl);
	    mdlog(vbuf);
	    }
	client_table[to].OutMsg.Ovfl = 0;
	return(1);
	}
    else  {  /*  partial write  */
	client_table[to].ClMib.clOctetsSent += actual;
	sysMib.sysOctetsSent += actual;
	++(client_table[to].ClMib.clPartialWrites);
	client_table[to].OutMsg.Data += actual;
	if (dolog && l_flag)  {
	    sprintf(vbuf,
	"partially cleared write buffer; to channel=%d, name=\"%s\"\n\t\t\t  ",
		to, client_table[to].Name);
	    s = vbuf + strlen(vbuf);
	    sprintf(s,
		"mid=%ld,bytes-written=%d,messages-overflowed=%d",
	    client_table[to].OutMsg.Mid, actual, client_table[to].OutMsg.Ovfl);
	    mdlog(vbuf);
	    }
	return(1);
	}
}


void	clr_all_wbuffers()
/*
 *  search client_table for all clients with buffered data in the
 *  queue.  attempt to write buffered data to each client.
 *  run only every CLR_WBUF_INTERVAL seconds.
 */
{
    long	systime;	/*  system time in seconds  */
    static	long	last_time = 0;
    struct	timeval		timev;
    struct	timezone	timez;
    int		i;

    (void)gettimeofday(&timev, &timez);

    systime = timev.tv_sec;
    if (systime >= (last_time + CLR_WBUF_INTERVAL))
	last_time = systime;
    else
	return;

    for (i = 0; i < max_clients; i++)  {
	if (client_table[i].Active == 0)
	    continue;
	(void)clr_wbuffer(i, 1);
	}
}


void	 show_client_namelist(requestor)
char	*requestor;
/*
 *  send comma-separated list of clients
 */
{
    char		 lbuf[MAXMSGSIZE];
    int			 i;
    ClientRecord	*cr;
    int			 need_comma;
    long		 my_mid;

    memset(lbuf, '\0', sizeof(lbuf));
    sprintf(lbuf, "%s %s client-list ", pname, requestor);
    need_comma = 0;
    for (i = 0, cr = client_table; i < max_clients; i++, cr++)  {
        if (cr->Active < 1)
            continue;
	if (!need_comma)
	    need_comma = 1;
	else
	    strcat(lbuf, ",");
	strcat(lbuf, cr->Name);
	}
    my_mid = glmid;
    ++glmid;
    send_msg_to_clients(-1, requestor, lbuf, my_mid);
}


void	 show_client_table(requestor)
char	*requestor;
/*
 *  send client table info to requesting client
 */
{
    ClientRecord	*cr;
    int			 i;
    char		 shbuf[500];
    int			 wbuf_size;
    long		 my_mid;

    for (i = 0, cr = client_table; i < max_clients; i++, cr++)  {
	if (cr->Active < 1)
	    continue;
	if (cr->OutMsg.Buf == NULL || cr->OutMsg.Data == NULL)
	    wbuf_size = 0;
	else
	    wbuf_size = strlen(cr->OutMsg.Data)+1;

    sprintf(shbuf,
      "%s %s client_table[%d] Name=\"%s\",ReadBufSize=%d,ReadBufCharOvfl=%d,WriteBufSize=%d,WriteBufMsgOvfl=%d", 
	pname, requestor,
	i, cr->Name,
	cr->IncMsg.EndOfBuf, cr->IncMsg.Ovfl,
	wbuf_size, cr->OutMsg.Ovfl);
	my_mid = glmid;
	++glmid;
	send_msg_to_clients(-1, requestor, shbuf, my_mid);
	}
}


void	 dump_sys_mib(requestor)
char	*requestor;
{
    time_t		 now;
    struct rusage	 ru;
    static int		 page_size = -1;
    char		 lbuf[MAXMSGSIZE];
    char		 tbuf[5000];
    char		*s;
    int			 i;
    long		 my_mid;

    if (page_size < 0)
	page_size = getpagesize();

    memset((void *)&ru, 0, sizeof(struct rusage));

    now = (long)time((long *)NULL);
    (void)getrusage(RUSAGE_SELF, &ru);

    lbuf[0] = '\0';

    sprintf(tbuf, "sysProcPathName=%s\n  sysProcName=%s\n  sysProcId=%d\n",
	    sysMib.sysProcPathName, sysMib.sysProcName, sysMib.sysProcId);
    strcat(lbuf, tbuf);

    sprintf(tbuf, "sysHostName=%s\n  sysIpAddr=%s\n  sysTcpPort=%d\n",
	    sysMib.sysHostName, sysMib.sysIpAddr, sysMib.sysTcpPort);
    strcat(lbuf, tbuf);

    sprintf(tbuf, "sysVersionNum=%s\n  sysVersionDate=%s\n",
	    sysMib.sysVersionNum, sysMib.sysVersionDate);
    strcat(lbuf, tbuf);

    sprintf(tbuf, "sysMaxFds=%d\n  sysMaxClients=%d\n",
	    sysMib.sysMaxFds, sysMib.sysMaxClients);
    strcat(lbuf, tbuf);

    sprintf(tbuf,
      "sysStartTimeGmt=%s\n  sysStartTimeLocal=%s\n  sysStartTimeSecs=%ld\n",
	    sysMib.sysStartTimeGmt, sysMib.sysStartTimeLocal,
	    sysMib.sysStartTimeSecs);
    strcat(lbuf, tbuf);

    s = get_gmt_timestamp(now);
    if (!s)
	s = "";
    sprintf(tbuf, "sysTimeGmt=%s\n", s);
    strcat(lbuf, tbuf);

    s = get_local_timestamp(now);
    if (!s)
	s = "";
    sprintf(tbuf, "  sysTimeLocal=%s\n", s);
    strcat(lbuf, tbuf);
    sprintf(tbuf, "  sysTimeSecs=%ld\n", now);
    strcat(lbuf, tbuf);

    sprintf(tbuf, "sysUpTimeSecs=%ld\n", now - sysMib.sysStartTimeSecs);
    strcat(lbuf, tbuf);

    sprintf(tbuf, "sysOctetsReceived=%ld\n  sysMsgsReceived=%ld\n",
	    sysMib.sysOctetsReceived, sysMib.sysMsgsReceived);
    strcat(lbuf, tbuf);
    sprintf(tbuf, "  sysOctetsSent=%ld\n  sysMsgsSent=%ld\n",
	    sysMib.sysOctetsSent, sysMib.sysMsgsSent);
    strcat(lbuf, tbuf);

    sprintf(tbuf, "sysRuUsrTime=%ld.%06ld\n  sysRuSysTime=%ld.%06ld\n",
          ru.ru_utime.tv_sec, ru.ru_utime.tv_usec,
          ru.ru_stime.tv_sec, ru.ru_stime.tv_usec);
    strcat(lbuf, tbuf);

    i = (ru.ru_maxrss * page_size)/1024;
    sprintf(tbuf, "  sysRuRss=%d\n", i);
    strcat(lbuf, tbuf);
    i = (ru.ru_idrss * page_size)/1024;
    sprintf(tbuf, "  sysRuIntRss=%d\n", i);
    strcat(lbuf, tbuf);

    sprintf(tbuf, "  sysRuMinFlt=%d\n", ru.ru_minflt);
    strcat(lbuf, tbuf);
    sprintf(tbuf, "  sysRuMajFlt=%d\n", ru.ru_majflt);
    strcat(lbuf, tbuf);
    sprintf(tbuf, "  sysRuNSwap=%d\n", ru.ru_nswap);
    strcat(lbuf, tbuf);
    sprintf(tbuf, "  sysRuInBlock=%d\n", ru.ru_inblock);
    strcat(lbuf, tbuf);
    sprintf(tbuf, "  sysRuOutBlock=%d\n", ru.ru_oublock);
    strcat(lbuf, tbuf);
    sprintf(tbuf, "  sysRuMsgSnd=%d\n", ru.ru_msgsnd);
    strcat(lbuf, tbuf);
    sprintf(tbuf, "  sysRuMsgRcv=%d\n", ru.ru_msgrcv);
    strcat(lbuf, tbuf);
    sprintf(tbuf, "  sysRuNSignals=%d\n", ru.ru_nsignals);
    strcat(lbuf, tbuf);
    sprintf(tbuf, "  sysRuNVCSw=%d\n", ru.ru_nvcsw);
    strcat(lbuf, tbuf);
    sprintf(tbuf, "  sysRuNIVCSw=%d\n", ru.ru_nivcsw);
    strcat(lbuf, tbuf);

    my_mid = glmid;
    ++glmid;
    send_msg_to_clients(-1, requestor, lbuf, my_mid);
}


void	 dump_client_mib(requestor, irange, reglist)
char	*requestor;
char	*irange;
char	*reglist;
{
    int			 i_first, i_last;
    char		*s, *t;
    int			 all_reg; 
    int			 ci;
    ClientRecord	*cr;
    time_t		 now;
    char		 tbuf[5000];
    char		 lbuf[MAXMSGSIZE];
    long		 my_mid;

    i_first = atoi(irange);
    s = strchr(irange, '-');
    if (s && s[1])  {
	s = s+1;
	i_last = atoi(s);
	if (i_last < i_first)
	    i_last = i_first;
	}
    else
	i_last = i_first;


    if (reglist && reglist[0])  {
	/*  reglist points to a non-empty string.  convert it to lower-	*/
	/*  case for case-insensitive comparisons.			*/
	for (s = reglist; *s; s++)  {
	    if (isupper(*s))
		*s = tolower(*s);
	    }
	all_reg = 0;
	}
    else
	all_reg = 1;

    for (ci = i_first; ci <= i_last; ci++)  {
	cr = &(client_table[ci]);
	if (!(cr->Active))
	    continue;

	lbuf[0] = '\0';

	now = (long)time((long *)NULL);

	sprintf(tbuf, "clIndex.%d=%d\n", ci, ci);
	strcat(lbuf, tbuf);

	sprintf(tbuf, "clTimeOfPollSecs.%d=%ld\n", ci, now);
	strcat(lbuf, tbuf);

	if (all_reg || strstr(reglist, "name"))  {
	    sprintf(tbuf, "clName.%d=%s\n", ci, cr->Name);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "sock"))  {
	    sprintf(tbuf, "clSock.%d=%d\n", ci, cr->Sock);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "hostname"))  {
	    sprintf(tbuf, "clHostName.%d=%s\n", ci, cr->ClMib.clHostName);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "hostipaddr"))  {
	    sprintf(tbuf, "clHostIpAddr.%d=%s\n",
		    ci, cr->ClMib.clHostIpAddr);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "hostport"))  {
	    sprintf(tbuf, "clHostPort.%d=%d\n", ci, cr->ClMib.clHostPort);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "msgsr"))  {
	    sprintf(tbuf, "clMsgsReceived.%d=%ld\n",
		    ci, cr->ClMib.clMsgsReceived);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "octetsr"))  {
	    sprintf(tbuf, "clOctetsReceived.%d=%ld\n",
		    ci, cr->ClMib.clOctetsReceived);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "completer"))  {
	    sprintf(tbuf, "clCompleteReads.%d=%ld\n",
		    ci, cr->ClMib.clCompleteReads);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "partialr"))  {
	    sprintf(tbuf, "clPartialReads.%d=%ld\n",
		    ci, cr->ClMib.clPartialReads);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "readbufo"))  {
	    sprintf(tbuf, "clReadBufOverflows.%d=%ld\n",
		    ci, cr->ClMib.clReadBufOverflows);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "readbufs"))  {
	    sprintf(tbuf, "clReadBufSize.%d=%ld\n",
		    ci, cr->IncMsg.EndOfBuf);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "readbufc"))  {
	    sprintf(tbuf, "clReadBufCharOvfl.%d=%ld\n",
		    ci, cr->IncMsg.Ovfl);
	    strcat(lbuf, tbuf);
	    }

	if (!all_reg)  {
	    for (s = strstr(reglist, "readbuf"); s; s = strstr(s, "readbuf")) {
		/*  is there a "readbufx" where x is not [a-zA-Z]  */
		if (strlen(s) < 8)
		    break;
		if (!isalpha(s[7]))
		    break;
		s += 8;
		}
	    }
	if (all_reg || s)  {
	    t = cr->IncMsg.Buf;
	    if (!t)
		t = "";
	    sprintf(tbuf, "clReadBuf.%d,%d=%s\n", ci, strlen(t), t);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "msgss"))  {
	    sprintf(tbuf, "clMsgsSent.%d=%ld\n", ci, cr->ClMib.clMsgsSent);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "octetss"))  {
	    sprintf(tbuf, "clOctetsSent.%d=%ld\n", ci, cr->ClMib.clOctetsSent);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "completew"))  {
	    sprintf(tbuf, "clCompleteWrites.%d=%ld\n",
		    ci, cr->ClMib.clCompleteWrites);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "partialw"))  {
	    sprintf(tbuf, "clPartialWrites.%d=%ld\n",
		    ci, cr->ClMib.clPartialWrites);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "failedw"))  {
	    sprintf(tbuf, "clFailedWrites.%d=%ld\n",
		    ci, cr->ClMib.clFailedWrites);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "writebufs"))  {
	    sprintf(tbuf, "clWriteBufSize.%d=%d\n",
		    ci, (int)(cr->OutMsg.Data - cr->OutMsg.Buf));
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "writebufs"))  {
	    sprintf(tbuf, "clWriteBufMsgOvfl.%d=%d\n", ci, cr->OutMsg.Ovfl);
	    strcat(lbuf, tbuf);
	    }

	if (!all_reg)  {
	    for (s = strstr(reglist, "writebuf");
		 s;
		 s = strstr(s, "writebuf")) {
		/*  is there a "writebufx" where x is not [a-zA-Z]  */
		if (strlen(s) < 9)
		    break;
		if (!isalpha(s[8]))
		    break;
		s += 8;
		}
	    }
	if (all_reg || s)  {
	    t = cr->OutMsg.Buf;
	    if (!t)
		t = "";
	    sprintf(tbuf, "clWriteBuf.%d,%d=%s\n", ci, strlen(t), t);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "whencon"))  {
	    sprintf(tbuf, "clWhenConnected.%d=%ld\n",
		    ci, cr->ClMib.clWhenConnected);
	    strcat(lbuf, tbuf);
	    }

	if (all_reg || strstr(reglist, "lasthe"))  {
	    sprintf(tbuf, "clLastHeardFrom.%d=%ld\n",
		    ci, cr->ClMib.clLastHeardFrom);
	    strcat(lbuf, tbuf);
	    }

	my_mid = glmid;
	++glmid;
	send_msg_to_clients(-1, requestor, lbuf, my_mid);
	}
}


/*ARGSUSED*/
void	 catcher(sig, code, scp, addr)
int			 sig;
int			 code;
struct sigcontext	*scp;
char			*addr;
{
    if (l_flag)  {
	sprintf(vbuf, "caught signal %d\n", sig);
	mdlog(vbuf);
	}
}


char	*get_gmt_timestamp(systime)
time_t	 systime;
/*
 *  convert systime, the number of seconds since 00:00 Zulu time on
 *  January 1, 1970, to a GMT timestamp in the form "YYMMDD.HHMM.SS".
 */
{
    struct tm	*t;
    static char  timestamp[40];

    t = gmtime(&systime);

    sprintf(timestamp, "%02d%02d%02d.%02d%02d.%02d",
			t->tm_year,
			t->tm_mon+1,
			t->tm_mday,
			t->tm_hour,
			t->tm_min,
			t->tm_sec);
    return(timestamp);
}


char	*get_local_timestamp(systime)
time_t	 systime;
/*
 *  convert systime, the number of seconds since 00:00 Zulu time on
 *  January 1, 1970, to a local time timestamp in the form "YYMMDD.HHMM.SS".
 */
{
    struct tm	*t;
    static char  timestamp[40];

    t = localtime(&systime);

    sprintf(timestamp, "%02d%02d%02d.%02d%02d.%02d",
			t->tm_year,
			t->tm_mon+1,
			t->tm_mday,
			t->tm_hour,
			t->tm_min,
			t->tm_sec);
    return(timestamp);
}
