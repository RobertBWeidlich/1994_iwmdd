/************************************************************************
 *  file:	mdlog.c
 *  author:	Bob Weidlich
 *  date:	October 8, 1993
 *  purpose:	manage log and debug files.  Create two files; the first
 *		is written into.  When the size of that file exceeds a
 *		size specified by the user, then copy that file to the
 *		second file, erase the first file, then start writing
 *		to the first file again.  This strategy ensures that
 *		we don't write infinitely long log files.
 *
 *  todo:	chmod logfiles to 0644.
 ************************************************************************/
#ifndef lint
#ifndef SABER
static char sccsid[] = "@(#)mdlog.c	1.5 10/8/93 GTE-TSC";
#endif
#endif

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/time.h>
#include <memory.h>

static	char	*fname1[MAXPATHLEN+1];
static	char	*fname2[MAXPATHLEN+1];
static	FILE	*fp = (FILE *)NULL;
static	int	 ceiling;

static	int	 rollover();


int	 open_mdlog(pname, maxsize)
/*
 *  pname is pathname; must be absolute (first character = '/').  maxsize
 *  is maximum file size; once log file reaches this size then roll over
 *  into secondary file.
 *
 *  return 1 if successful, 0 otherwise.
 */
char	*pname;
int	 maxsize;
{
    if (fp)
	return(0);
    if (*pname != '/')  {
	fprintf(stderr,
"iwmd: Warning - \"%s\" not absolute pathname; logging disabled\n",
	  pname);
	return(0);
	}

    strncpy(fname1, pname, MAXPATHLEN-3);
    fname1[MAXPATHLEN-3] = '\0';
	
    strncpy(fname2, pname, MAXPATHLEN-3);
    fname1[MAXPATHLEN-3] = '\0';
    strcat(fname2, ".b");

    fp = fopen(fname1, "w"); 
    if (!fp)  {
	fprintf(stderr,
	  "iwmd: Warning - cannot open log file \"%s\" for writing\n",
	  fname1);
	return(0);
	}

    ceiling = maxsize;
    /*  reject unreasonably small values  */
    if (ceiling < 1000)
	ceiling = 1000;

    return(1);
}


int	 close_mdlog()
/*
 *  close logfile, halt logging
 */
{
    if (!fp)
	return(0);
    fclose(fp);
    return(1);
}


static	int	rollover()
/*
 *  close first log file, move first file to second file, empty first,
 *  and reopen for writing.
 */
{
    if (!fp)
	return(0);
    fclose(fp);  fp = (FILE *)NULL;
    if (rename(fname1, fname2) != 0)
	return(0);
    fp = fopen(fname1, "w"); 
    if (!fp)
	return(0);
    return(1);
}


int	 mdlog(s)
char	*s;
/*
 *  write buffer s to log file.  Prepend timestamp; append newline.
 *  return length written.
 */
{
    struct	 timeval	 timev;
    struct	 timezone	 timez;
    struct	 tm		*t;
    long	 systime;
    char	 lbuf[10000];
    char	*u;

    if (!fp)
	return(0);

    if ((int)ftell(fp) > ceiling)  {
	if (!rollover())
	    return(0);
	}

    (void)gettimeofday(&timev, &timez);
    systime = timev.tv_sec;
    t = localtime(&systime);

    /*  don't log entire message if length is unreasonably long	*/
    if (strlen(s) > (sizeof(lbuf)-75))  {
	memset(lbuf, '\0', sizeof(lbuf));
	strcpy(lbuf, asctime(t));
	u = strchr(lbuf, '\n');
	if (u)
	    *u = '\0';
	strcat(lbuf, ": ");
	strncat(lbuf, s, sizeof(lbuf)-75);
	if (lbuf[strlen(lbuf)-1] != '\n')
	    lbuf[strlen(lbuf)-1] = '\0';
	strcat(lbuf, " ### mdlog() - message truncated ###\n");
	fprintf(fp, "%s", lbuf);
	return(sizeof(lbuf)-75);
	}
    strcpy(lbuf, asctime(t));
    u = strchr(lbuf, '\n');
    if (u)
	*u = '\0';
    strcat(lbuf, ": ");
    strcat(lbuf, s);
    if (lbuf[strlen(lbuf)-1] != '\n')
	strcat(lbuf, "\n");
    fprintf(fp, "%s", lbuf);
    fflush(fp);
    return(strlen(s));
}
