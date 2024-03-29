/* Copyright (c) 2006 Russ Cox */

#include <u.h>
//#include <sys/select.h>
#include <libc.h>
#include <thread.h>
#include <draw.h>
#include <mouse.h>
#include <cursor.h>
#include <drawfcall.h>
#include <mux.h>

int chattydrawclient = 0;

static int	drawgettag(Mux *mux, void *vmsg);
static void*	drawrecv(Mux *mux);
static int	drawnbrecv(Mux *mux, void**);
static int	drawsend(Mux *mux, void *vmsg);
static int	drawsettag(Mux *mux, void *vmsg, uint tag);
static int canreadfd(int);

int
_displayconnect(Display *d)
{
	int pid, p[2], fd[3];
	
	fmtinstall('W', drawfcallfmt);
	fmtinstall('H', encodefmt);
	
	if(pipe(p) < 0)
		return -1;

	fd[0] = p[1];
	fd[1] = p[1];
	fd[2] = dup(2, -1);
		/* execl("strace", "strace", "-o", "drawsrv.out", "drawsrv", nil); */
		/*
		 * The argv0 has no meaning to devdraw.
		 * Pass it along only so that the various
		 * devdraws in psu -a can be distinguished.
		 * The NOLIBTHREADDAEMONIZE keeps devdraw from
		 * forking before threadmain. OS X hates it when
		 * guis fork.
		 *
		 * If client didn't use ARGBEGIN, argv0 == nil.
		 * Can't send nil through because OS X expects
		 * argv[0] to be non-nil.  Also, OS X apparently
		 * expects argv[0] to be a valid executable name,
		 * so "(argv0)" is not okay.  Use "devdraw"
		 * instead.
		 */
		putenv("NOLIBTHREADDAEMONIZE", "1");
		if(argv0 == nil)
			argv0 = "devdraw";
	pid = threadspawnl(fd, "devdraw", "devdraw", argv0, nil);
	if(pid<0){
		close(fd[0]);
		close(fd[2]);
		sysfatal("threadspawn devdraw: %r");
	}
#ifdef __MINGW32__
	d->srvfd = recvfd(p[0]);
	close(p[0]);
	if(d->srvfd == -1)
		sysfatal("recvfd: %r");
#else
	d->srvfd = p[0];
#endif
	
	return 0;
}

int
_displaymux(Display *d)
{
	if((d->mux = mallocz(sizeof(*d->mux), 1)) == nil)
		return -1;

	d->mux->mintag = 1;
	d->mux->maxtag = 255;
	d->mux->send = drawsend;
	d->mux->recv = drawrecv;
	d->mux->nbrecv = drawnbrecv;
	d->mux->gettag = drawgettag;
	d->mux->settag = drawsettag;
	d->mux->aux = d;
	muxinit(d->mux);
	
	return 0;
}

static int
drawsend(Mux *mux, void *vmsg)
{
	int n;
	uchar *msg;
	Display *d;
	
	msg = vmsg;
	GET(msg, n);
	d = mux->aux;
	return write(d->srvfd, msg, n);
}

static int
_drawrecv(Mux *mux, int canblock, void **vp)
{
	int n;
	uchar buf[4], *p;
	Display *d;

	d = mux->aux;
	*vp = nil;
	if(!canblock && !canreadfd(d->srvfd))
		return 0;
	if((n=readn(d->srvfd, buf, 4)) != 4)
		return 1;
	GET(buf, n);
	p = malloc(n);
	if(p == nil){
		fprint(2, "out of memory allocating %d in drawrecv\n", n);
		return 1;
	}
	memmove(p, buf, 4);
	if(readn(d->srvfd, p+4, n-4) != n-4){
		free(p);
		return 1;
	}
	*vp = p;
	return 1;
}

static void*
drawrecv(Mux *mux)
{
	void *p;
	_drawrecv(mux, 1, &p);
	return p;
}

static int
drawnbrecv(Mux *mux, void **vp)
{
	return _drawrecv(mux, 0, vp);
}

static int
drawgettag(Mux *mux, void *vmsg)
{
	uchar *msg;
	USED(mux);
	
	msg = vmsg;
	return msg[4];
}

static int
drawsettag(Mux *mux, void *vmsg, uint tag)
{
	uchar *msg;
	USED(mux);
	
	msg = vmsg;
	msg[4] = tag;
	return 0;
}

static int
displayrpc(Display *d, Wsysmsg *tx, Wsysmsg *rx, void **freep)
{
	int n, nn;
	void *tpkt, *rpkt;
	
	n = sizeW2M(tx);
	tpkt = malloc(n);
	if(freep)
		*freep = nil;
	if(tpkt == nil)
		return -1;
	tx->tag = 0;
	if(chattydrawclient)
		fprint(2, "<- %W\n", tx);
	nn = convW2M(tx, tpkt, n);
	if(nn != n){
		free(tpkt);
		werrstr("drawclient: sizeW2M convW2M mismatch");
		fprint(2, "%r\n");
		return -1;
	}
	/*
	 * This is the only point where we might reschedule.
	 * Muxrpc might need to acquire d->mux->lk, which could
	 * be held by some other proc (e.g., the one reading from
	 * the keyboard via Trdkbd messages).  If we need to wait
	 * for the lock, don't let other threads from this proc
	 * run.  This keeps up the appearance that writes to /dev/draw
	 * don't cause rescheduling.  If you *do* allow rescheduling
	 * here, then flushimage(display, 1) happening in two different
	 * threads in the same proc can cause a buffer of commands
	 * to be written out twice, leading to interesting results
	 * on the screen.
	 *
	 * Threadpin and threadunpin were added to the thread library
	 * to solve exactly this problem.  Be careful!  They are dangerous.
	 *
	 * _pin and _unpin are aliases for threadpin and threadunpin
	 * in a threaded program and are no-ops in unthreaded programs.
	 */
	_pin();
	rpkt = muxrpc(d->mux, tpkt);
	_unpin();
	free(tpkt);
	if(rpkt == nil){
		werrstr("muxrpc: %r");
		return -1;
	}
	GET((uchar*)rpkt, n);
	nn = convM2W(rpkt, n, rx);
	if(nn != n){
		free(rpkt);
		werrstr("drawclient: convM2W packet size mismatch %d %d %.*H", n, nn, n, rpkt);
		fprint(2, "%r\n");
		return -1;
	}
	if(chattydrawclient)
		fprint(2, "-> %W\n", rx);
	if(rx->type == Rerror){
		werrstr("%s", rx->error);
		free(rpkt);
		return -1;
	}
	if(rx->type != tx->type+1){
		werrstr("packet type mismatch -- tx %d rx %d",
			tx->type, rx->type);
		free(rpkt);
		return -1;
	}
	if(freep)
		*freep = rpkt;
	else
		free(rpkt);
	return 0;
}

int
_displayinit(Display *d, char *label, char *winsize)
{
	Wsysmsg tx, rx;

	tx.type = Tinit;
	tx.label = label;
	tx.winsize = winsize;
	return displayrpc(d, &tx, &rx, nil);
}

int
_displayrdmouse(Display *d, Mouse *m, int *resized)
{
	Wsysmsg tx, rx;

	tx.type = Trdmouse;
	if(displayrpc(d, &tx, &rx, nil) < 0)
		return -1;
	*m = rx.mouse;
	*resized = rx.resized;
	return 0;
}

int
_displayrdkbd(Display *d, Rune *r)
{
	Wsysmsg tx, rx;

	tx.type = Trdkbd;
	if(displayrpc(d, &tx, &rx, nil) < 0)
		return -1;
	*r = rx.rune;
	return 0;
}

int
_displaymoveto(Display *d, Point p)
{
	Wsysmsg tx, rx;

	tx.type = Tmoveto;
	tx.mouse.xy = p;
	return displayrpc(d, &tx, &rx, nil);
}

int
_displaycursor(Display *d, Cursor *c)
{
	Wsysmsg tx, rx;
	
	tx.type = Tcursor;
	if(c == nil){
		memset(&tx.cursor, 0, sizeof tx.cursor);
		tx.arrowcursor = 1;
	}else{
		tx.arrowcursor = 0;
		tx.cursor = *c;
	}
	return displayrpc(d, &tx, &rx, nil);
}

int
_displaybouncemouse(Display *d, Mouse *m)
{
	Wsysmsg tx, rx;
	
	tx.type = Tbouncemouse;
	tx.mouse = *m;
	return displayrpc(d, &tx, &rx, nil);
}

int
_displaylabel(Display *d, char *label)
{
	Wsysmsg tx, rx;
	
	tx.type = Tlabel;
	tx.label = label;
	return displayrpc(d, &tx, &rx, nil);
}

char*
_displayrdsnarf(Display *d)
{
	void *p;
	char *s;
	Wsysmsg tx, rx;
	
	tx.type = Trdsnarf;
	if(displayrpc(d, &tx, &rx, &p) < 0)
		return nil;
	s = strdup(rx.snarf);
	free(p);
	return s;
}

int
_displaywrsnarf(Display *d, char *snarf)
{
	Wsysmsg tx, rx;
	
	tx.type = Twrsnarf;
	tx.snarf = snarf;
	return displayrpc(d, &tx, &rx, nil);
}

int
_displayrddraw(Display *d, void *v, int n)
{
	void *p;
	Wsysmsg tx, rx;
	
	tx.type = Trddraw;
	tx.count = n;
	if(displayrpc(d, &tx, &rx, &p) < 0)
		return -1;
	memmove(v, rx.data, rx.count);
	free(p);
	return rx.count;
}

int
_displaywrdraw(Display *d, void *v, int n)
{
	Wsysmsg tx, rx;
	
	tx.type = Twrdraw;
	tx.count = n;
	tx.data = v;
	if(displayrpc(d, &tx, &rx, nil) < 0)
		return -1;
	return rx.count;
}

int
_displaytop(Display *d)
{
	Wsysmsg tx, rx;

	tx.type = Ttop;
	return displayrpc(d, &tx, &rx, nil);
}

int
_displayresize(Display *d, Rectangle r)
{
	Wsysmsg tx, rx;
	
	tx.type = Tresize;
	tx.rect = r;
	return displayrpc(d, &tx, &rx, nil);
}

static int
canreadfd(int fd)
{
#ifdef __MINGW32__
	sysfatal("canreadfd");
#else
	fd_set rs, ws, xs;
	struct timeval tv;
	
	FD_ZERO(&rs);
	FD_ZERO(&ws);
	FD_ZERO(&xs);
	FD_SET(fd, &rs);
	FD_SET(fd, &xs);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	if(select(fd+1, &rs, &ws, &xs, &tv) < 0)
		return 0;
	if(FD_ISSET(fd, &rs) || FD_ISSET(fd, &xs))
		return 1;
#endif
	return 0;
}

