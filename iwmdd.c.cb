/************************************************************************
 *  file:	iwmd.c
 *  date:	August 15, 1991
 *  author:	Bob Weidlich
 *  purpose:	IW Message Dispatcher: a socket server that allows
 *		multiple clients to connect, register their process
 *		names, and then pass messages transparently from one
 *		client to another.
 *
 *  todo:	- solve the "sirius" problem, with iwdb support.
 *		- convert process names to lower-case???
 *		- handle running in the background
 *		- should exec connect to iwdb via a socket, or to
 *		  stdin/stdout/stderr.  If the former, then the exec
 *		  would be "just another" iwdb process, whereas if the
 *		  latter, then the exec would spawn the iwmd as a
 *		  separate process and use pipes to control stdin, stdout,
 *		  and stderr.  I'm leaning toward the latter case.
 *
 *  mods:	Version 1.0 - released June 22, 1990
 *
 *		Version 1.0.1 - released July 27, 1990
 *		   (corresponds to sccs version 1.7)
 *		  -strip leading blanks from stdin and from messages
 *		   from clients
 *		  -send message to all instances of a repeated client
 *		   name instead of just the first instance.
 *		  -consider the 2nd argument of a message from a client
 *		   as a list of comma-separated processes, rather than
 *		   a single process ("commproc iwui,iwnn polldone ...")
 *		   do the same for the 1st argument of the command-line
 *		   interface
 *
 *		Version 1.1.0 - released August 31, 1990
 *		  -incorporates a new command set designed both for
 *		   manual control and control by an executive process.
 *		  -Enhance process name to consist of <class>.<instance>.
 *		   Enable sending messages to a single instance, or an
 *		   entire class of process instances.  (Note: the new iwdb
 *		   procedure call "IWGetExecId()" provides IW processes
 *		   with a unique (to the process) name for use with this
 *		   feature.)
 *		  -Provide list of connected processes to clients upon
 *		   request.
 *		  -Provide a background mode where the process can be
 *		   put in the shell background.  This set by either
 *		   command line argument or an interactive command
 *		
 ************************************************************************/
#ifndef lint
static char sccsid[] = "@(#)iwmd.c	1.24	1/27/92	GTE-TSC";
#endif
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <ctype.h>

#define  MAX_CLIENTS           100
#define  MAX_PROCNAME_LEN       40
#define  BLEN                 2048

typedef  struct _ClientRecord  {
	char	Name[MAX_PROCNAME_LEN+2];
	int	Active;	 /*  1 if this record is active; 0 if not	*/
	int	Sock;	 /*  socket file descriptor			*/
	}  ClientRecord;

ClientRecord	client_table[MAX_CLIENTS];

char	 rbuf[BLEN+3], wbuf[BLEN+3], buf[BLEN+3];

int	 local_sock;
int	 width;
int	 bg_mode;
extern	 char	*index(), *rindex();

int	 init_local_sock();
void	 show_header();
void	 show_help();
void	 show_active_clients();
void	 service_msg_from_stdin();
void	 service_msg_from_client();
void	 service_msg_to_me();
void	 send_msg_to_client();
int	 disconnect_channel();
int	 disconnect_process();

/*  info on would-be clients for security  */
struct	  sockaddr  sockinfo;
int	  infosize;

main(argc, argv)
int	 argc;
char	*argv[];
{
    int		 ac;
    int		 excess_fd;
    fd_set	 read_ready;
    int		 bufsize;
    int		 i;
    char	*s;
    int		 need_new_prompt;

    if (argc < 2)  {
	fprintf(stderr, "Usage: iwmd [-bg] <port-number>\n");
	exit(1);
	}
    bg_mode = 0;

    ac = 1;
    while ((ac < argc) && (*argv[ac] == '-'))  {
	if (strncmp(argv[ac], "-bg", 3)  == 0)  {
	    bg_mode = 1;
	    }
	else  {
	    fprintf(stderr, "Usage: iwmd [-bg] <port-number>\n");
	    exit(1);
	    }
	++ac;
	}

    if (ac >= argc)  {
	fprintf(stderr, "Usage: iwmd [-bg] <port-number>\n");
	exit(1);
	}

    if (!init_local_sock(atoi(argv[ac])))
	exit(1);

    if (bg_mode)
	printf("iwmd: starting up in background mode (ignoring stdin)\n");

    for (i = 0; i < MAX_CLIENTS; i++)  {  /*  clear out client_table	*/
	client_table[i].Active = 0;
	client_table[i].Name[0] = '\0';
	client_table[i].Sock = 0;
	}
    show_header();
    need_new_prompt = 1;
    do  {
	if (need_new_prompt)  {
	    printf("iwmd> "); fflush(stdout);
	    }
	need_new_prompt = 1;
	bzero(rbuf, sizeof(rbuf));
	bzero(wbuf, sizeof(wbuf));
	FD_ZERO(&read_ready);
	if (!bg_mode)
	    FD_SET(0, &read_ready);	  /*  check stdin  */
	FD_SET(local_sock, &read_ready);  /*  check incoming connections  */
	for(i = 0; i < MAX_CLIENTS; i++)  {
	    if (client_table[i].Active != 0)  /*  check active clients	*/
		FD_SET(client_table[i].Sock, &read_ready);
	    }

	/*  block until stdin, remote_sock, or active client		*/
	/*  has data to be read.					*/
	select(width+1, &read_ready, (fd_set *)NULL, (fd_set *)NULL,
		(struct timeval *)NULL);

	if (FD_ISSET(local_sock, &read_ready))  {
	    need_new_prompt = 0;
	    /*  new connection requested		   		*/
	    /*  get first free socket descriptor in client table	*/
	    for (i = 0; i < MAX_CLIENTS; i++)  {
		if (client_table[i].Active == 0)
		    break;
		}
	    if (i >= MAX_CLIENTS)  {  /*  no free descriptors left	*/
		excess_fd = accept(local_sock, &sockinfo, &infosize);
		if (excess_fd == -1)
		    perror("accept()");
		else if (excess_fd > 0)  {
		    close(excess_fd);
		    printf("iwmd: incoming-connect-attempt-overflow\n");
		    fflush(stdout);
		    }
		}
	    else  {
		infosize = sizeof(sockinfo);
		/*printf("infosize(before) = %d\n", infosize);*/
		client_table[i].Sock = accept(local_sock,
				(struct sockaddr *)NULL, (int *)NULL);
		if (client_table[i].Sock == -1)  {
		    perror("accept()");
		    client_table[i].Sock = 0;
		    }
		else  {
		    client_table[i].Active = 1;
		    client_table[i].Name[0] = '\0';
		    /*
		    printf("infosize = %d\n", infosize);
		    printf("sockinfo.sa_family: %d\n", sockinfo.sa_family);
		    printf("sockinfo.sa_data: \"%s\"\n", sockinfo.sa_data);
		    */
		    if (width < client_table[i].Sock)
			width = client_table[i].Sock;
		    }
		}
	    }
	if (FD_ISSET(0, &read_ready))  {
	    /*  stdin is ready to read.  First token of input line is	*/
	    /*  command.						*/
	    bufsize = read(0, rbuf, sizeof(rbuf));
	    if (bufsize < 0)
		perror("reading stream message");
	    if (bufsize > 1)  {	/*  we got more than just newline  */
		rbuf[bufsize-1] = '\0';	/*  get rid of newline  */
		service_msg_from_stdin(rbuf);
		}
	    }
	for (i = 0; i < MAX_CLIENTS; i++)  {
	    if (client_table[i].Active == 0)
		continue;
	    if (FD_ISSET(client_table[i].Sock, &read_ready))  {
		/*  socket ready to read  */
		bufsize = read(client_table[i].Sock, rbuf, BLEN);
		/*  todo: check that bufsize < BLEN.  In the meantime,	*/
		/*  a very large BLEN size practically assures no	*/
		/*  problems.						*/
		if (bufsize < 0)  {
		    perror("reading stream message\n");
		    continue;
		    }
		else if (bufsize == 0)  {
		    /*  client has disconnected  */
		    if (strlen(client_table[i].Name) > 0) 
			printf(
			  "iwmd: client-disconnect channel:%d process:%s\n",
			  i, client_table[i].Name);
		    else  /*  process name never registered  */
			printf( "iwmd: client-disconnect channel:%d\n", i);
		    fflush(stdout);
		    (void)disconnect_channel(i);
		    }
		else  {
		    rbuf[bufsize] = (char) NULL;
		    service_msg_from_client(i, rbuf);
		    }
		}
	    }
	}  while (1);
    exit(0);
}


int	init_local_sock(port)
int	port;
{

    struct sockaddr_in	 server;
    int			 sockadd_in_size;
    sockadd_in_size = sizeof(struct sockaddr_in);

    local_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (local_sock < 0)  {
	perror("opening stream socket\n");
	return(0);
	}
    width = local_sock;

    bzero(&server, sockadd_in_size);
    server.sin_family = AF_INET;
    server.sin_port = (u_short) port;

    if (bind(local_sock, (struct sockaddr *) &server, sockadd_in_size) < 0)  {
	perror("binding stream socket");
	return(0);
	}

    listen(local_sock, 5);
    return(1);
}


void	service_msg_from_stdin(msg)
char	*msg;
/*
 *  parse the msg string for a command.  Execute that command.
 */
{
    char	*s, *tmp;
    char	*command;
    char	*disconnectee;
    char	*src, *destlist, *dest, *body, *show_what;
    int		 i;
    int		 found;

    /*  exit quietly if nothing is in msg  */
    if ((msg == NULL) || (strlen(msg) < 1))
	return;

    /*  strip leading blanks  */
    for (s = msg; *s; s++)  {
	if ((*s != '\t') && (*s != ' '))
	    break;
	}
    if (!*s)	/*  nothing in line  */
	return;

    if(tmp = index(s, '\n'))	/*  get rid of newline character,	*/
	*tmp = '\0';		/*  if present.				*/

    command = strtok(s, " \t");
    if (!command)  {
	printf("iwmd: command-failed could not parse command\n");
	fflush(stdout);
	return;
	}

    if (strcmp(command, "help") == 0)  {
	show_help();
	return;
	}

    if (strcmp(command, "bg") == 0)  {
	bg_mode = 1;
	printf("going into background mode (ignoring stdin)\n");
	return;
	}

    if (strcmp(command, "disconnect") == 0)  {
	disconnectee = strtok((char *)NULL, " \t");
	if (!disconnectee)  {
	    printf("iwmd: command-failed expecting 2 arguments\n");
	    fflush(stdout);
	    return;
	    }
	/*  is disconnectee a channel number or a process name?  */
	if (isdigit(disconnectee[0]))  {  /*  must be a channel number	*/
	    i = atoi(disconnectee);
	    if ((i < 0) || (i >= MAX_CLIENTS))  {
		/*  out of range of legit channel numbers	*/
		printf("iwmd: command-failed bad channel number: %d\n",
				i);
		fflush(stdout);
		return;
		}
	    if (!disconnect_channel(i))  {
		printf(
		"iwmd: command-failed could not disconnect channel: %d\n",
		  i);
		fflush(stdout);
		}
	    }
	else  {
	    if (!disconnect_process(disconnectee))  {
		printf(
		"iwmd: command-failed could not disconnect process: %s\n",
		  disconnectee);
		fflush(stdout);
		}
	    }
	return;
	}

    if (strcmp(command, "msg") == 0)  {
	src = strtok((char *)NULL, " \t");
	if (src)
	    destlist = strtok((char *)NULL, " \t");
	if (destlist)
	    body = strtok((char *)NULL, "");
	if (!body)  {
	    printf("iwmd: command-failed expected at least 4 tokens\n");
	    fflush(stdout);
	    return;
	    }
	for (dest = strtok(destlist, ","); dest;
					dest = strtok((char *)NULL, ","))  {
	    bzero(wbuf, sizeof(wbuf));
	    sprintf(wbuf, "%s %s %s", src, dest, body);
	    send_msg_to_client(dest, wbuf);
	    }
	return;
	}

    if (strcmp(command, "msgto") == 0)  {
	destlist = strtok((char *)NULL, " \t");
	if (destlist)
	    body = strtok((char *)NULL, "");
	if (!body)  {	
	    printf("iwmd: command-failed expected at least 3 tokens\n");
	    fflush(stdout);
	    return;
	    }
	for (dest = strtok(destlist, " \t"); dest;
					dest = strtok((char *)NULL, " \t"))  {
	    bzero(wbuf, sizeof(wbuf));
	    send_msg_to_client(dest, body);
	    }
	return;
	}

    if (strcmp(command, "show") == 0)  {
	show_what = strtok((char *)NULL, " \t");
	if (!show_what)  {
	    printf("iwmd: command-failed expected at least 2 tokens\n");
	    fflush(stdout);
	    return;
	    }
	if (strncmp(show_what, "conn", 4) == 0)
	    show_active_clients();
	else  {
	    printf("iwmd: command-failed do not recognize 2nd token\n");
	    fflush(stdout);
	    }
	return;
	}

    if (strcmp(command, "quit") == 0)  {
	for (i = 0; i <= MAX_CLIENTS; i++)  {
	    if (client_table[i].Active)
		(void)disconnect_channel(i);
	    }
	close(local_sock);
	exit(0);
	}

    printf("iwmd: command-failed 1st token not recognized\n");
    fflush(stdout);
}


void	 service_msg_from_client(ind, msg)
int	 ind;
char	*msg;
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
 *  we "register" process names.
 */
{
    int		 i;
    char	*src, *destlist, *dest, *body;
    char	*s, *tmp;

    bzero(buf, sizeof(buf));
    strcpy(buf, msg);
    /*  strip leading blanks  */
    for (s = buf; *s; s++)  {
	if ((*s != '\t') && (*s != ' '))
	    break;
	}
    if (!*s)	/*  nothing in buffer but spaces and/or tabs, so ignore	*/
	return;

    if(tmp = index(s, '\n'))	/*  get rid of newline character,	*/
	*tmp = '\0';		/*  if present.	 NOTE: do we really	*/
				/*  want to preclude this???		*/

    src = strtok(buf, " \t");
    if (src)
	destlist = strtok((char *)NULL, " \t");
    if (destlist)
	body = strtok((char *)NULL, " \t");

    if (!src)  {
	printf(
	"iwmd: bad-message: =>%s<= from channel %d; cannot parse 1st token\n",
		msg, ind);
	fflush(stdout);
	return;
	}

    /*  register client if not yet registered  */
    if (strlen(client_table[ind].Name) < 1)  {
	strncpy(client_table[ind].Name, src, MAX_PROCNAME_LEN);
	client_table[ind].Name[MAX_PROCNAME_LEN] = '\0';
	printf("iwmd: new-client channel:%d process:%s\n",
		ind, client_table[ind].Name);
	fflush(stdout);
	}

    if (!destlist)	/* can't do anything more with it	*/
	return;

    for (dest = strtok(destlist, ","); dest;
					dest = strtok((char *)NULL, ","))  {
	if (strcmp(dest, "iwmd") == 0)
	    service_msg_to_me(src, dest, body, msg);
	else  {
	    send_msg_to_client(dest, msg);


	    }
	}
}


void	 service_msg_to_me(src, dest, body, msg)
/*
 *  deal with messages directed to iwmd.  currently only the "show"
 *  command is supported.
 */
char	*src;
char	*dest;
char	*body;
char	*msg;
{
    int		i, first;

    first = 1;
    printf("iwmd: message-for-me =>%s<=\n", msg);
    if ((body) && strncmp(body, "show", 4) == 0)  {
	/*  return comma-separated list of active process  */
	bzero(wbuf, sizeof(wbuf));
	sprintf(wbuf, "iwmd %s show-clients ", src);
	for (i = 0; i < MAX_CLIENTS; i++)  {
	    if ( (client_table[i].Active) &&
		 (strlen(client_table[i].Name)>0) )  {
		if (first)
		    first = 0;
		else
		    strcat(wbuf, ",");
		strcat(wbuf, client_table[i].Name);
		}
	    }
	send_msg_to_client(src, wbuf);
	}
}


void	 send_msg_to_client(destproc, msg)
char	*destproc;
char	*msg;
/*
 *  send socket messagea to all registered clients whose first
 *  strlen(destproc) characters in their process names match those of
 *  destproc.
 *
 *  treat "all" as a special case which means to send msg to all
 *  registered clients.
 */
{
    int  i, len, found;
    int	 allflag;

    len = strlen(destproc);
    if (strcmp(destproc, "all") == 0)  {
	allflag = 1;
	found = 1;
	}
    else  {
	allflag = 0;
	found = 0;
	}

    for (i = 0; i < MAX_CLIENTS; i++)  {
	if (!client_table[i].Active)
	    continue;
	if (client_table[i].Name[0] == '\0')
	    continue;	/*  unregistered clients are ignored  */
	if ( (allflag) ||
	     (strncmp(client_table[i].Name, destproc, len) == 0) )  {
	    found = 1;
	    write(client_table[i].Sock, msg, strlen(msg)+1);
				/*  note: sending terminating null  */
	    printf("iwmd: transfer-message to \"%s\" =>%s<=\n", destproc, msg);
	    }
	}
    if (!found)  {  /*  note that we don't complain if destproc is	*/
		    /*  "all" and nobody is registered.			*/
	printf(
"iwmd: cannot-transfer-message: =>%s<= process \"%s\" not registered\n",
		  msg, destproc);
	fflush(stdout);
	}
}


int	disconnect_channel(i)
/*
 *  disconnect channel i and return 1 if it is active, otherwise, return 0.
 */
{
    if ((i < 0) || (i >= MAX_CLIENTS))
	return(0);
    if (client_table[i].Active)  {
	close(client_table[i].Sock);
	client_table[i].Sock = 0;
	client_table[i].Active = 0;
	client_table[i].Name[0] = '\0';
	}
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
    for(i = 0; i < MAX_CLIENTS; i++)  {
	if (!client_table[i].Active)
	    continue;
	if (strncmp(proc, client_table[i].Name, len) == 0)  {
	    (void)disconnect_channel(i);
	    found = 1;
	    }
	}
    return(found);
}


void	show_header()
{
    printf("\n");
    printf("  #############################################\n");
    printf("  # IW Message Dispatcher - Version 1.1.1     #\n");
    printf("  #             September 11, 1990            #\n");
    printf("  #     type \"help\" for list of commands      #\n");
    printf("  #############################################\n");
    printf("\n");
    fflush(stdout);
}


void	show_help()
{
    static	char	*help_msg = "\n\
    IW Message Dispatcher command summary:\n\
\n\
  help                                    - show this message\n\
  bg                                      - background mode\n\
  disconnect <channel>                    - terminate connection\n\
  disconnect <process>                    - terminate connection\n\
  msg <src-proc> <dest-proc-list> <body>  - send standard-format message\n\
  msgto <dest-proc-list> <msg>            - send free-format message\n\
  show connections                        - list active clients\n\
  quit                                    - disconnect all clients\n\
                                            and terminate\n\
\n";

    printf(help_msg);
    fflush(stdout);
}


void	show_active_clients()
/*
 *  dump list of active sockets to stdout
 */
{
    int		i;

    printf("  Channel                 Process\n");
    printf("  ======= ========================================\n");
    for (i = 0; i < MAX_CLIENTS; i++)  {
	if (client_table[i].Active)
	    printf("  %5d   %-40s\n", i, client_table[i].Name);
	}
    fflush(stdout);
}
