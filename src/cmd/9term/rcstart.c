#include <u.h>
#include <libc.h>
#include <thread.h>
#include "term.h"

int loginshell;


int
rcstart(int argc, char **argv, int *pfd, int *tfd)
{
	int fd[2], fds[3], pid;
	char *cmd, *xargv[3];
	char slave[256];

	if(argc == 0){
		argc = 2;
		argv = xargv;
		argv[0] = getenv("SHELL");
		if(argv[0] == 0)
			argv[0] = "rc";
		argv[1] = "-i";
		argv[2] = 0;
	}
	cmd = argv[0];
	if(loginshell){
		argv[0] = malloc(strlen(cmd)+2);
		strcpy(argv[0]+1, cmd);
		argv[0][0] = '-';
	}

	/*
	 * fd0 is slave (tty), fd1 is master (pty)
	 */
	fd[0] = fd[1] = -1;
	if(getpts(fd, slave) < 0){
		exit(3);
		sysfatal("getpts: %r\n");
	}
	/*
	 * notedisable("sys: window size change");
	 * 
	 * Can't disable because will be inherited by other programs
	 * like if you run an xterm from the prompt, and then xterm's
	 * resizes won't get handled right.  Sigh.  
	 *
	 * Can't not disable because when we stty below we'll get a
	 * signal, which will drop us into the thread library note handler,
	 * which will get all confused because we just forked and thus
	 * have an unknown pid. 
	 *
	 * So disable it internally.  ARGH!
	 */
	notifyoff("sys: window size change");

	putenv("TERM", "9term");
	fds[0] = fd[0];
	fds[1] = fd[0];
	fds[2] = fd[0];
	pid = threadspawn(fds, cmd, argv);
	if(pid == -1)
		sysfatal("threadspawn failed: %r");
	*pfd = fd[1];
	return pid;
}

struct {
	Lock l;
	char buf[1<<20];
	int r, w;
} echo;

void
echoed(char *p, int n)
{
	lock(&echo.l);
	if(echo.r > 0) {
		memmove(echo.buf, echo.buf+echo.r, echo.w-echo.r);
		echo.w -= echo.r;
		echo.r = 0;
	}
	if(echo.w+n > sizeof echo.buf)
		echo.r = echo.w = 0;
	if(echo.w+n > sizeof echo.buf)
		n = 0;
	memmove(echo.buf+echo.w, p, n);
	echo.w += n;	
	unlock(&echo.l);
}

int
echocancel(char *p, int n)
{
	int i;

	lock(&echo.l);
	for(i=0; i<n; i++) {
		if(echo.r < echo.w) {
			if(echo.buf[echo.r] == p[i]) {
				echo.r++;
				continue;
			}
			if(echo.buf[echo.r] == '\n' && p[i] == '\r')
				continue;
			if(p[i] == 0x08) {
				if(i+2 <= n && p[i+1] == ' ' && p[i+2] == 0x08)
					i += 2;
				continue;
			}
		}
		echo.r = echo.w;
		break;
	}
	unlock(&echo.l);
	if(i > 0)
		memmove(p, p+i, n-i);
	return n-i;
}

int
dropcrnl(char *p, int n)
{
	char *r, *w;

	for(r=w=p; r<p+n; r++) {
		if(r+1<p+n && *r == '\r' && *(r+1) == '\n')
			continue;
		if(*r == 0x08) {
			if(r+2<=p+n && *(r+1) == ' ' && *(r+2) == 0x08)
				r += 2;
			continue;
		}
		*w++ = *r;
	}
	return w-p;
}
