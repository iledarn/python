/***********************************************************
Copyright 1991-1995 by Stichting Mathematisch Centrum, Amsterdam,
The Netherlands.

                        All Rights Reserved

Permission to use, copy, modify, and distribute this software and its 
documentation for any purpose and without fee is hereby granted, 
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in 
supporting documentation, and that the names of Stichting Mathematisch
Centrum or CWI not be used in advertising or publicity pertaining to
distribution of the software without specific, written prior permission.

STICHTING MATHEMATISCH CENTRUM DISCLAIMS ALL WARRANTIES WITH REGARD TO
THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS, IN NO EVENT SHALL STICHTING MATHEMATISCH CENTRUM BE LIABLE
FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

******************************************************************/

#include "Python.h"
#include "macglue.h"

#include <OSUtils.h> /* for Set(Current)A5 */
#include <Files.h>
#include <Resources.h>
#include <Memory.h>
#include <Events.h>
#include <Windows.h>
#include <Desk.h>
#include <Traps.h>

#ifdef __MWERKS__
#include <SIOUX.h>
#endif /* __MWERKS__ */

#include <signal.h>

/* XXX We should include Errors.h here, but it has a name conflict
** with the python errors.h. */
#define fnfErr -43

/* Declared in macfsmodule.c: */
extern FSSpec *mfs_GetFSSpecFSSpec();

/*
** We attempt to be a good citizen by giving up the CPU periodically.
** When in the foreground we do this less often and for shorter periods
** than when in the background. At this time we also check for events and
** pass them off to stdwin (or SIOUX, if compiling with mwerks).
** The counts here are in ticks of 1/60th second.
** XXXX The initial values here are not based on anything.
** FG-python gives up the cpu for 1/60th 5 times per second,
** BG-python for .2 second 10 times per second.
*/
static long interval_fg = 12;
static long interval_bg = 6;
static long yield_fg = 1;
static long yield_bg = 12;
static long lastyield, in_background;

/* Convert C to Pascal string. Returns pointer to static buffer. */
unsigned char *
Pstring(char *str)
{
	static Str255 buf;
	int len;

	len = strlen(str);
	if (len > 255)
		len = 255;
	buf[0] = (unsigned char)len;
	strncpy((char *)buf+1, str, len);
	return buf;
}

/* Replace strerror with something that might work */
char *macstrerror(int err)
{
	static char buf[256];
	Handle h;
	char *str;
	
	h = GetResource('Estr', err);
	if ( h ) {
		HLock(h);
		str = (char *)*h;
		memcpy(buf, str+1, (unsigned char)str[0]);
		HUnlock(h);
		ReleaseResource(h);
	} else {
		sprintf(buf, "Mac OS error code %d", err);
	}
	return buf;
}

/* Exception object shared by all Mac specific modules for Mac OS errors */
PyObject *PyMac_OSErrException;

/* Initialize and return PyMac_OSErrException */
PyObject *
PyMac_GetOSErrException()
{
	if (PyMac_OSErrException == NULL)
		PyMac_OSErrException = PyString_FromString("Mac OS Error");
	return PyMac_OSErrException;
}

/* Set a MAC-specific error from errno, and return NULL; return None if no error */
PyObject * 
PyErr_Mac(PyObject *eobj, int err)
{
	char *msg;
	PyObject *v;
	
	if (err == 0 && !PyErr_Occurred()) {
		Py_INCREF(Py_None);
		return Py_None;
	}
	if (err == -1 && PyErr_Occurred())
		return NULL;
	msg = macstrerror(err);
	v = Py_BuildValue("(is)", err, msg);
	PyErr_SetObject(eobj, v);
	Py_DECREF(v);
	return NULL;
}

/* Call PyErr_Mac with PyMac_OSErrException */
PyObject *
PyMac_Error(OSErr err)
{
	return PyErr_Mac(PyMac_GetOSErrException(), err);
}

/*
** Set yield timeouts
*/
void
PyMac_SetYield(long fgi, long fgy, long bgi, long bgy)
{
	interval_fg = fgi;
	yield_fg = fgy;
	interval_bg = bgi;
	yield_bg = bgy;
}

/*
** Yield the CPU to other tasks.
*/
static
PyMac_DoYield()
{
	EventRecord ev;
	WindowPtr wp;
	long yield;
	extern int StdwinIsActive;
	static int no_waitnextevent = -1;
	int gotone;
	
	if ( no_waitnextevent < 0 ) {
		no_waitnextevent = (NGetTrapAddress(_WaitNextEvent, ToolTrap) ==
							NGetTrapAddress(_Unimplemented, ToolTrap));
	}

	if ( StdwinIsActive )
		return;
		
	if ( in_background )
		yield = yield_bg;
	else
		yield = yield_fg;
	while ( 1 ) {
		if ( no_waitnextevent ) {
			SystemTask();
			gotone = GetNextEvent(0xffff, &ev);
		} else {
			gotone = WaitNextEvent(everyEvent, &ev, yield, NULL);
		}	
		/* Get out quickly if nothing interesting is happening */
		if ( !gotone || ev.what == nullEvent )
			break;
			
		/* Check whether we've moved to foreground or background */
		if ( ev.what == osEvt &&
		         (ev.message & osEvtMessageMask) == (suspendResumeMessage<<24)) {
			if ( ev.message & resumeFlag ) {
				in_background = 0;
			} else {
				in_background = 1;
			}
		}
#ifdef __MWERKS__
		/* If SIOUX wants it we're done too */
		(void)SIOUXHandleOneEvent(&ev);
#else
		/* Other compilers are just unlucky: we only weed out clicks in other applications */
		if ( ev.what == mouseDown ) {
			if ( FindWindow(ev.where, &wp) == inSysWindow )
				SystemClick(&ev, wp);
		}
#endif /* !__MWERKS__ */
	}
	lastyield = TickCount();
}

/*
** Yield the CPU to other tasks if opportune
*/
void
PyMac_Yield() {
	long iv;
	
	if ( in_background )
		iv = interval_bg;
	else
		iv = interval_fg;
	if ( TickCount() > lastyield + iv )
		PyMac_DoYield();
}

/*
** Idle routine for busy-wait loops.
** Gives up CPU, handles events and returns true if an interrupt is pending
** (but not actually handled yet).
*/
int
PyMac_Idle()
{
	PyMac_DoYield();
	return intrpeek();
}


/* Convert a 4-char string object argument to an OSType value */
int
PyMac_GetOSType(PyObject *v, OSType *pr)
{
	if (!PyString_Check(v) || PyString_Size(v) != 4) {
		PyErr_SetString(PyExc_TypeError,
			"OSType arg must be string of 4 chars");
		return 0;
	}
	memcpy((char *)pr, PyString_AsString(v), 4);
	return 1;
}

/* Convert an OSType value to a 4-char string object */
PyObject *
PyMac_BuildOSType(OSType t)
{
	return PyString_FromStringAndSize((char *)&t, 4);
}


/* Convert a Python string object to a Str255 */
int
PyMac_GetStr255(PyObject *v, Str255 pbuf)
{
	int len;
	if (!PyString_Check(v) || (len = PyString_Size(v)) > 255) {
		PyErr_SetString(PyExc_TypeError,
			"Str255 arg must be string of at most 255 chars");
		return 0;
	}
	pbuf[0] = len;
	memcpy((char *)(pbuf+1), PyString_AsString(v), len);
	return 1;
}

/* Convert a Str255 to a Python string object */
PyObject *
PyMac_BuildStr255(Str255 s)
{
	return PyString_FromStringAndSize((char *)&s[1], (int)s[0]);
}


/*
** Convert a Python object to an FSSpec.
** The object may either be a full pathname or a triple
** (vrefnum, dirid, path).
** NOTE: This routine will fail on pre-sys7 machines. 
** The caller is responsible for not calling this routine
** in those cases (which is fine, since everyone calling
** this is probably sys7 dependent anyway).
*/
int
PyMac_GetFSSpec(PyObject *v, FSSpec *fs)
{
	Str255 path;
	short refnum;
	long parid;
	OSErr err;
	FSSpec *fs2;

	/* first check whether it already is an FSSpec */
	fs2 = mfs_GetFSSpecFSSpec(v);
	if ( fs2 ) {
		fs = fs2;
		return 1;
	}
	if ( PyString_Check(v) ) {
		/* It's a pathname */
		if( !PyArg_Parse(v, "O&", PyMac_GetStr255, &path) )
			return 0;
		refnum = 0; /* XXXX Should get CurWD here... */
		parid = 0;
	} else {
		if( !PyArg_Parse(v, "(hlO&); FSSpec should be fullpath or (vrefnum,dirid,path)",
							&refnum, &parid, PyMac_GetStr255, &path)) {
			return 0;
		}
	}
	err = FSMakeFSSpec(refnum, parid, path, fs);
	if ( err && err != fnfErr ) {
		PyErr_Mac(PyExc_ValueError, err);
		return 0;
	}
	return 1;
}



/* Convert a Python object to a Rect.
   The object must be a (left, top, right, bottom) tuple.
   (This differs from the order in the struct but is consistent with
   the arguments to SetRect(), and also with STDWIN). */
int
PyMac_GetRect(PyObject *v, Rect *r)
{
	return PyArg_Parse(v, "(hhhh)", &r->left, &r->top, &r->right, &r->bottom);
}

/* Convert a Rect to a Python object */
PyObject *
PyMac_BuildRect(Rect *r)
{
	return Py_BuildValue("(hhhh)", r->left, r->top, r->right, r->bottom);
}


/* Convert a Python object to a Point.
   The object must be a (h, v) tuple.
   (This differs from the order in the struct but is consistent with
   the arguments to SetPoint(), and also with STDWIN). */
int
PyMac_GetPoint(PyObject *v, Point *p)
{
	return PyArg_Parse(v, "(hh)", &p->h, &p->v);
}

/* Convert a Point to a Python object */
PyObject *
PyMac_BuildPoint(Point p)
{
	return Py_BuildValue("(hh)", p.h, p.v);
}


/* Convert a Python object to an EventRecord.
   The object must be a (what, message, when, (v, h), modifiers) tuple. */
int
PyMac_GetEventRecord(PyObject *v, EventRecord *e)
{
	return PyArg_Parse(v, "(hll(hh)h)",
	                   &e->what,
	                   &e->message,
	                   &e->when,
	                   &e->where.h,
	                   &e->where.v,                   
	                   &e->modifiers);
}

/* Convert a Rect to an EventRecord object */
PyObject *
PyMac_BuildEventRecord(EventRecord *e)
{
	return Py_BuildValue("(hll(hh)h)",
	                     e->what,
	                     e->message,
	                     e->when,
	                     e->where.h,
	                     e->where.v,
	                     e->modifiers);
}
