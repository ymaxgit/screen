/* Copyright (c) 2010
 *      Juergen Weigert (jnweiger@immd4.informatik.uni-erlangen.de)
 *      Sadrul Habib Chowdhury (sadrul@users.sourceforge.net)
 * Copyright (c) 2008, 2009
 *      Juergen Weigert (jnweiger@immd4.informatik.uni-erlangen.de)
 *      Michael Schroeder (mlschroe@immd4.informatik.uni-erlangen.de)
 *      Micah Cowan (micah@cowan.name)
 *      Sadrul Habib Chowdhury (sadrul@users.sourceforge.net)
 * Copyright (c) 1993-2002, 2003, 2005, 2006, 2007
 *      Juergen Weigert (jnweiger@immd4.informatik.uni-erlangen.de)
 *      Michael Schroeder (mlschroe@immd4.informatik.uni-erlangen.de)
 * Copyright (c) 1987 Oliver Laumann
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING); if not, see
 * http://www.gnu.org/licenses/, or contact Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 *
 ****************************************************************
 */

#include "winmsg.h"
#include "fileio.h"
#include "process.h"
#include "sched.h"
#include "mark.h"


char winmsg_buf[MAXSTR];
uint64_t winmsg_rend[MAX_WINMSG_REND];
int winmsg_rendpos[MAX_WINMSG_REND];
int winmsg_numrend;

#define CHRPAD 127

struct backtick {
	struct backtick *next;
	int num;
	int tick;
	int lifespan;
	time_t bestbefore;
	char result[MAXSTR];
	char **cmdv;
	Event ev;
	char *buf;
	int bufi;
} *backticks;


static char *pad_expand(char *buf, char *p, int numpad, int padlen)
{
	char *pn, *pn2;
	int i, r;

	padlen = padlen - (p - buf);	/* space for rent */
	if (padlen < 0)
		padlen = 0;
	pn2 = pn = p + padlen;
	r = winmsg_numrend;
	while (p >= buf) {
		if (r && *p != CHRPAD && p - buf == winmsg_rendpos[r - 1]) {
			winmsg_rendpos[--r] = pn - buf;
			continue;
		}
		*pn-- = *p;
		if (*p-- == CHRPAD) {
			pn[1] = ' ';
			i = numpad > 0 ? (padlen + numpad - 1) / numpad : 0;
			padlen -= i;
			while (i-- > 0)
				*pn-- = ' ';
			numpad--;
			if (r && p - buf == winmsg_rendpos[r - 1])
				winmsg_rendpos[--r] = pn - buf;
		}
	}
	return pn2;
}

static void backtick_filter(struct backtick *bt)
{
	char *p, *q;
	int c;

	for (p = q = bt->result; (c = (unsigned char)*p++) != 0;) {
		if (c == '\t')
			c = ' ';
		if (c >= ' ' || c == '\005')
			*q++ = c;
	}
	*q = 0;
}

static void backtick_fn(Event *ev, char *data)
{
	struct backtick *bt;
	int i, j, k, l;

	bt = (struct backtick *)data;
	i = bt->bufi;
	l = read(ev->fd, bt->buf + i, MAXSTR - i);
	if (l <= 0) {
		evdeq(ev);
		close(ev->fd);
		ev->fd = -1;
		return;
	}
	i += l;
	for (j = 0; j < l; j++)
		if (bt->buf[i - j - 1] == '\n')
			break;
	if (j < l) {
		for (k = i - j - 2; k >= 0; k--)
			if (bt->buf[k] == '\n')
				break;
		k++;
		memmove(bt->result, bt->buf + k, i - j - k);
		bt->result[i - j - k - 1] = 0;
		backtick_filter(bt);
		WindowChanged(0, '`');
	}
	if (j == l && i == MAXSTR) {
		j = MAXSTR / 2;
		l = j + 1;
	}
	if (j < l) {
		if (j)
			memmove(bt->buf, bt->buf + i - j, j);
		i = j;
	}
	bt->bufi = i;
}

void setbacktick(int num, int lifespan, int tick, char **cmdv)
{
	struct backtick **btp, *bt;
	char **v;

	for (btp = &backticks; (bt = *btp) != 0; btp = &bt->next)
		if (bt->num == num)
			break;
	if (!bt && !cmdv)
		return;
	if (bt) {
		for (v = bt->cmdv; *v; v++)
			free(*v);
		free(bt->cmdv);
		if (bt->buf)
			free(bt->buf);
		if (bt->ev.fd >= 0)
			close(bt->ev.fd);
		evdeq(&bt->ev);
	}
	if (bt && !cmdv) {
		*btp = bt->next;
		free(bt);
		return;
	}
	if (!bt) {
		bt = malloc(sizeof *bt);
		if (!bt) {
			Msg(0, "%s", strnomem);
			return;
		}
		memset(bt, 0, sizeof(*bt));
		bt->next = 0;
		*btp = bt;
	}
	bt->num = num;
	bt->tick = tick;
	bt->lifespan = lifespan;
	bt->bestbefore = 0;
	bt->result[0] = 0;
	bt->buf = 0;
	bt->bufi = 0;
	bt->cmdv = cmdv;
	bt->ev.fd = -1;
	if (bt->tick == 0 && bt->lifespan == 0) {
		bt->buf = malloc(MAXSTR);
		if (bt->buf == 0) {
			Msg(0, "%s", strnomem);
			setbacktick(num, 0, 0, (char **)0);
			return;
		}
		bt->ev.type = EV_READ;
		bt->ev.fd = readpipe(bt->cmdv);
		bt->ev.handler = backtick_fn;
		bt->ev.data = (char *)bt;
		if (bt->ev.fd >= 0)
			evenq(&bt->ev);
	}
}


static char *runbacktick(struct backtick *bt, int *tickp, time_t now)
{
	int f, i, l, j;
	time_t now2;

	if (bt->tick && (!*tickp || bt->tick < *tickp))
		*tickp = bt->tick;
	if ((bt->lifespan == 0 && bt->tick == 0) || now < bt->bestbefore) {
		return bt->result;
	}
	f = readpipe(bt->cmdv);
	if (f == -1)
		return bt->result;
	i = 0;
	while ((l = read(f, bt->result + i, sizeof(bt->result) - i)) > 0) {
		i += l;
		for (j = 1; j < l; j++)
			if (bt->result[i - j - 1] == '\n')
				break;
		if (j == l && i == sizeof(bt->result)) {
			j = sizeof(bt->result) / 2;
			l = j + 1;
		}
		if (j < l) {
			memmove(bt->result, bt->result + i - j, j);
			i = j;
		}
	}
	close(f);
	bt->result[sizeof(bt->result) - 1] = '\n';
	if (i && bt->result[i - 1] == '\n')
		i--;
	bt->result[i] = 0;
	backtick_filter(bt);
	(void)time(&now2);
	bt->bestbefore = now2 + bt->lifespan;
	return bt->result;
}

int AddWinMsgRend(const char *str, uint64_t r)
{
	if (winmsg_numrend >= MAX_WINMSG_REND || str < winmsg_buf || str >= winmsg_buf + MAXSTR)
		return -1;

	winmsg_rend[winmsg_numrend] = r;
	winmsg_rendpos[winmsg_numrend] = str - winmsg_buf;
	winmsg_numrend++;

	return 0;
}


static char *WinMsgPid(char *s, char **p, int plusflg)
{
	sprintf(*p, "%d", (plusflg && display) ? D_userpid : getpid());
	(*p) += strlen(*p) - 1;

	return s;
}

static char *WinMsgCopyMode(char *s, char **p, Event *ev, int *qmflag)
{
	(*p)--;
	if (display && ev && ev != &D_hstatusev) {	/* Hack */
		/* Is the layer in the current canvas in copy mode? */
		Canvas *cv = (Canvas *)ev->data;
		if (ev == &cv->c_captev && cv->c_layer->l_layfn == &MarkLf)
			*qmflag = 1;
	}

	return s;
}

/**
 * Processes rendition
 *
 * The first character of s is assumed to be (unverified) the opening brace
 * of the sequence.
 */
static char *WinMsgRend(char *s, char **p)
{
	char rbuf[RENDBUF_SIZE];
	uint8_t i;
	uint64_t r;

	s++;
	for (i = 0; i < (RENDBUF_SIZE-1); i++)
		if (s[i] && s[i] != WINMSG_REND_END)
			rbuf[i] = s[i];
		else
			break;

	if ((s[i] == WINMSG_REND_END) && (winmsg_numrend < MAX_WINMSG_REND)) {
		r = 0;
		rbuf[i] = '\0';
		if (i != 1 || rbuf[0] != WINMSG_REND_POP)
			r = ParseAttrColor(rbuf, 0);
		if (r != 0 || (i == 1 && (rbuf[0] == WINMSG_REND_POP))) {
			AddWinMsgRend(*p, r);
		}
	}
	s += i;
	(*p)--;

	return s;
}

char *MakeWinMsgEv(char *str, Window *win, int esc, int padlen, Event *ev, int rec)
{
	static int tick;
	char *s = str;
	char *p = winmsg_buf;
	register int ctrl;
	struct timeval now;
	int l, i;
	int num;
	int zeroflg;
	int longflg;
	int minusflg;
	int plusflg;
	int qmflag = 0, omflag = 0, qmnumrend = 0;
	char *qmpos = 0;
	int numpad = 0;
	int lastpad = 0;
	int truncpos = -1;
	int truncper = 0;
	int trunclong = 0;
	uint64_t r;
	struct backtick *bt = NULL;

	if (winmsg_numrend >= 0)
		winmsg_numrend = 0;
	else
		winmsg_numrend = -winmsg_numrend;

	*p = '\0';

	tick = 0;
	ctrl = 0;
	gettimeofday(&now, NULL);
	for (s = str; *s && (l = winmsg_buf + MAXSTR - 1 - p) > 0; s++, p++) {
		*p = *s;
		if (ctrl) {
			ctrl = 0;
			if (*s != '^' && *s >= 64)
				*p &= 0x1f;
			continue;
		}
		if (*s != esc) {
			if (esc == '%') {
				switch (*s) {
				case '^':
					ctrl = 1;
					*p-- = '^';
					break;
				default:
					break;
				}
			}
			continue;
		}
		if (*++s == esc)	/* double escape ? */
			continue;
		if ((plusflg = *s == '+') != 0)
			s++;
		if ((minusflg = *s == '-') != 0)
			s++;
		if ((zeroflg = *s == '0') != 0)
			s++;
		num = 0;
		while (*s >= '0' && *s <= '9')
			num = num * 10 + (*s++ - '0');
		if ((longflg = *s == 'L') != 0)
			s++;
		switch (*s) {
		case '?':
			p--;
			if (qmpos) {
				if ((!qmflag && !omflag) || omflag == 1) {
					p = qmpos;
					if (qmnumrend < winmsg_numrend)
						winmsg_numrend = qmnumrend;
				}
				qmpos = 0;
				break;
			}
			qmpos = p;
			qmnumrend = winmsg_numrend;
			qmflag = omflag = 0;
			break;
		case ':':
			p--;
			if (!qmpos)
				break;
			if (qmflag && omflag != 1) {
				omflag = 1;
				qmpos = p;
				qmnumrend = winmsg_numrend;
			} else {
				p = qmpos;
				if (qmnumrend < winmsg_numrend)
					winmsg_numrend = qmnumrend;
				omflag = -1;
			}
			break;
		case '`':
		case 'h':
			if (rec >= 10 || (*s == 'h' && (win == 0 || win->w_hstatus == 0 || *win->w_hstatus == 0))) {
				p--;
				break;
			}
			if (*s == '`') {
				for (bt = backticks; bt; bt = bt->next)
					if (bt->num == num)
						break;
				if (bt == 0) {
					p--;
					break;
				}
			}
			{
				char savebuf[sizeof(winmsg_buf)];
				int oldtick = tick;
				int oldnumrend = winmsg_numrend;

				*p = 0;
				strncpy(savebuf, winmsg_buf, sizeof(winmsg_buf));
				winmsg_numrend = -winmsg_numrend;
				MakeWinMsgEv(*s == 'h' ? win->w_hstatus : runbacktick(bt, &oldtick, now.tv_sec), win,
					     '\005', 0, (Event *)0, rec + 1);
				if (!tick || oldtick < tick)
					tick = oldtick;
				if ((int)strlen(winmsg_buf) < l)
					strncat(savebuf, winmsg_buf, sizeof(savebuf) - strlen(savebuf));
				strncpy(winmsg_buf, savebuf, sizeof(winmsg_buf));
				while (oldnumrend < winmsg_numrend)
					winmsg_rendpos[oldnumrend++] += p - winmsg_buf;
				if (*p)
					qmflag = 1;
				p += strlen(p) - 1;
			}
			break;
		case 'X':
		case 'x':
			*p = 0;
			for (i = 0; win && win->w_cmdargs[i]; i++) {
				if (l < strlen(win->w_cmdargs[i]) + 1)
					break;
				sprintf(p, i ? "%s" : " %s", win->w_cmdargs[i]);
				l -= strlen(p);
				p += strlen(p);
				if (i == 0 && *s == 'X')
					break;
			}
			p--;
			break;
		case 'w':
		case 'W':
			{
				Window *oldfore = 0;

				if (display) {
					oldfore = D_fore;
					D_fore = win;
				}
				AddWindows(p, l - 1,
					   (*s ==
					    'w' ? 0 : 1) | (longflg ? 0 : 2) | (plusflg ? 4 : 0) | (minusflg ? 8 : 0),
					   win ? win->w_number : -1);
				if (display)
					D_fore = oldfore;
			}
			if (*p)
				qmflag = 1;
			p += strlen(p) - 1;
			break;
		case 'f':
			*p = 0;
			if (win)
				AddWindowFlags(p, l - 1, win);
			if (*p)
				qmflag = 1;
			p += strlen(p) - 1;
			break;
		case 't':
			*p = 0;
			if (win && (int)strlen(win->w_title) < l) {
				strncpy(p, win->w_title, l);
				if (*p)
					qmflag = 1;
			}
			p += strlen(p) - 1;
			break;
		case WINMSG_REND_START:
			s = WinMsgRend(s, &p);
			break;
		case 'H':
			*p = 0;
			if ((int)strlen(HostName) < l) {
				strncpy(p, HostName, l);
				if (*p)
					qmflag = 1;
			}
			p += strlen(p) - 1;
			break;
		case 'S':
			{
				char *session_name;
				*p = 0;
				session_name = strchr(SocketName, '.') + 1;
				if ((int)strlen(session_name) < l) {
					strncpy(p, session_name, l);
					if (*p)
						qmflag = 1;
				}
				p += strlen(p) - 1;
			}
			break;
		case WINMSG_PID:
			s = WinMsgPid(s, &p, plusflg);
			break;
		case 'F':
			p--;
			/* small hack */
			if (display && ((ev && ev == &D_forecv->c_captev) || (!ev && win && win == D_fore)))
				minusflg = !minusflg;
			if (minusflg)
				qmflag = 1;
			break;
		case WINMSG_COPY_MODE:
			s = WinMsgCopyMode(s, &p, ev, &qmflag);
			break;
		case 'E':
			p--;
			if (display && D_ESCseen) {
				qmflag = 1;
			}
			break;
		case '>':
			truncpos = p - winmsg_buf;
			truncper = num > 100 ? 100 : num;
			trunclong = longflg;
			p--;
			break;
		case '=':
		case '<':
			*p = ' ';
			if (num || zeroflg || plusflg || longflg || (*s != '=')) {
				/* expand all pads */
				if (minusflg) {
					num = (plusflg ? lastpad : padlen) - num;
					if (!plusflg && padlen == 0)
						num = p - winmsg_buf;
					plusflg = 0;
				} else if (!zeroflg) {
					if (*s != '=' && num == 0 && !plusflg)
						num = 100;
					if (num > 100)
						num = 100;
					if (padlen == 0)
						num = p - winmsg_buf;
					else
						num = (padlen - (plusflg ? lastpad : 0)) * num / 100;
				}
				if (num < 0)
					num = 0;
				if (plusflg)
					num += lastpad;
				if (num > MAXSTR - 1)
					num = MAXSTR - 1;
				if (numpad)
					p = pad_expand(winmsg_buf, p, numpad, num);
				numpad = 0;
				if (p - winmsg_buf > num && !longflg) {
					int left, trunc;

					if (truncpos == -1) {
						truncpos = lastpad;
						truncper = 0;
					}
					trunc = lastpad + truncper * (num - lastpad) / 100;
					if (trunc > num)
						trunc = num;
					if (trunc < lastpad)
						trunc = lastpad;
					left = truncpos - trunc;
					if (left > p - winmsg_buf - num)
						left = p - winmsg_buf - num;
					if (left > 0) {
						if (left + lastpad > p - winmsg_buf)
							left = p - winmsg_buf - lastpad;
						if (p - winmsg_buf - lastpad - left > 0)
							memmove(winmsg_buf + lastpad, winmsg_buf + lastpad + left,
								p - winmsg_buf - lastpad - left);
						p -= left;
						r = winmsg_numrend;
						while (r && winmsg_rendpos[r - 1] > lastpad) {
							r--;
							winmsg_rendpos[r] -= left;
							if (winmsg_rendpos[r] < lastpad)
								winmsg_rendpos[r] = lastpad;
						}
						if (trunclong) {
							if (p - winmsg_buf > lastpad)
								winmsg_buf[lastpad] = '.';
							if (p - winmsg_buf > lastpad + 1)
								winmsg_buf[lastpad + 1] = '.';
							if (p - winmsg_buf > lastpad + 2)
								winmsg_buf[lastpad + 2] = '.';
						}
					}
					if (p - winmsg_buf > num) {
						p = winmsg_buf + num;
						if (trunclong) {
							if (num - 1 >= lastpad)
								p[-1] = '.';
							if (num - 2 >= lastpad)
								p[-2] = '.';
							if (num - 3 >= lastpad)
								p[-3] = '.';
						}
						r = winmsg_numrend;
						while (r && winmsg_rendpos[r - 1] > num)
							winmsg_rendpos[--r] = num;
					}
					truncpos = -1;
					trunclong = 0;
					if (lastpad > p - winmsg_buf)
						lastpad = p - winmsg_buf;
				}
				if (*s == '=') {
					while (p - winmsg_buf < num)
						*p++ = ' ';
					lastpad = p - winmsg_buf;
					truncpos = -1;
					trunclong = 0;
				}
				p--;
			} else if (padlen) {
				*p = CHRPAD;	/* internal pad representation */
				numpad++;
			}
			break;
		case 's':
			*p = 0;
			if (!win)
				sprintf(p, "--x--");
			else
				sprintf(p, "%dx%d", win->w_width, win->w_height);
			p += strlen(p) - 1;
			break;
		case 'n':
			s++;
			/* FALLTHROUGH */
		default:
			s--;
			if (l > 10 + num) {
				if (num == 0)
					num = 1;
				if (!win)
					sprintf(p, "%*s", num, num > 1 ? "--" : "-");
				else
					sprintf(p, "%*d", num, win->w_number);
				qmflag = 1;
				p += strlen(p) - 1;
			}
			break;
		}
	}
	if (qmpos && !qmflag)
		p = qmpos + 1;
	*p = '\0';
	if (numpad) {
		if (padlen > MAXSTR - 1)
			padlen = MAXSTR - 1;
		pad_expand(winmsg_buf, p, numpad, padlen);
	}
	if (ev) {
		evdeq(ev);	/* just in case */
		ev->timeout.tv_sec = 0;
		ev->timeout.tv_usec = 0;
	}
	if (ev && tick) {
		now.tv_usec = 100000;
		if (tick == 1)
			now.tv_sec++;
		else
			now.tv_sec += tick - (now.tv_sec % tick);
		ev->timeout = now;
	}
	return winmsg_buf;
}

char *MakeWinMsg(char *s, Window *win, int esc)
{
	return MakeWinMsgEv(s, win, esc, 0, (Event *)0, 0);
}