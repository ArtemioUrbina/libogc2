/*-------------------------------------------------------------

$Id: lwp.c,v 1.27 2006-05-02 14:39:55 shagkur Exp $

lwp.c -- Thread subsystem I

Copyright (C) 2004
Michael Wiedenbauer (shagkur)
Dave Murphy (WinterMute)

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1.	The origin of this software must not be misrepresented; you
must not claim that you wrote the original software. If you use
this software in a product, an acknowledgment in the product
documentation would be appreciated but is not required.

2.	Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3.	This notice may not be removed or altered from any source
distribution.

$Log: not supported by cvs2svn $
Revision 1.26  2006/05/02 14:18:14  shagkur
- changed LWP_GetSelf() to not issue a context switch

Revision 1.25  2006/05/02 11:56:10  shagkur
- changed object handling & thread protection

Revision 1.24  2006/05/02 09:34:08  shagkur
- changed object handling and handle typedef
- removed useless ISR disabling

Revision 1.23  2006/04/10 05:29:59  shagkur
- changed function prototypes of thread queue functions
- added LWP_ThreadSignal to wake up a single thread
- changed LWP_WakeThread to LWP_ThreadBroadcast
  (used to wake up all threads waiting on this particular thread queue)

Revision 1.22  2005/12/09 09:35:45  shagkur
no message

Revision 1.21  2005/11/21 12:15:46  shagkur
no message


-------------------------------------------------------------*/


#include <stdlib.h>
#include <errno.h>
#include "asm.h"
#include "processor.h"
#include "lwp_threadq.h"
#include "lwp_threads.h"
#include "lwp_wkspace.h"
#include "lwp_objmgr.h"
#include "lwp_config.h"
#include "lwp.h"

typedef struct _tqueue_st {
	lwp_obj object;
	lwp_thrqueue tqueue;
} tqueue_st;

static lwp_objinfo _lwp_thr_objects;
static lwp_objinfo _lwp_tqueue_objects;

extern int main();

extern u8 __stack_addr[],__stack_end[];

static __inline__ u32 __lwp_priotocore(u32 prio)
{
	return (255 - prio);
}

static __inline__ lwp_cntrl* __lwp_cntrl_open(lwp_t thr_id)
{
	return (lwp_cntrl*)__lwp_objmgr_get(&_lwp_thr_objects,thr_id);
}

static __inline__ tqueue_st* __lwp_tqueue_open(lwpq_t tqueue)
{
	return (tqueue_st*)__lwp_objmgr_get(&_lwp_tqueue_objects,tqueue);
}

static lwp_cntrl* __lwp_cntrl_allocate()
{
	lwp_cntrl *thethread;
	
	__lwp_thread_dispatchdisable();
	thethread = (lwp_cntrl*)__lwp_objmgr_allocate(&_lwp_thr_objects);
	if(thethread) {
		thethread->information = &_lwp_thr_objects;
		__lwp_objmgr_open(&_lwp_thr_objects,&thethread->object);
		return thethread;
	}
	__lwp_thread_dispatchenable();
	return NULL;
}

static tqueue_st* __lwp_tqueue_allocate()
{
	tqueue_st *tqueue;

	__lwp_thread_dispatchdisable();
	tqueue = (tqueue_st*)__lwp_objmgr_allocate(&_lwp_tqueue_objects);
	if(tqueue) {
		__lwp_objmgr_open(&_lwp_tqueue_objects,&tqueue->object);
		return tqueue;
	}
	__lwp_thread_dispatchenable();
	return NULL;
}

static __inline__ void __lwp_cntrl_free(lwp_cntrl *thethread)
{
	thethread->information = NULL;
	__lwp_objmgr_close(&_lwp_thr_objects,&thethread->object);
	__lwp_objmgr_free(&_lwp_thr_objects,&thethread->object);
}

static __inline__ void __lwp_tqueue_free(tqueue_st *tq)
{
	__lwp_objmgr_close(&_lwp_tqueue_objects,&tq->object);
	__lwp_objmgr_free(&_lwp_tqueue_objects,&tq->object);
}

static void* idle_func(void *arg)
{
	while(1);
	return 0;
}

void __lwp_sysinit()
{
	__lwp_objmgr_initinfo(&_lwp_thr_objects,LWP_MAX_THREADS,sizeof(lwp_cntrl));
	__lwp_objmgr_initinfo(&_lwp_tqueue_objects,LWP_MAX_TQUEUES,sizeof(tqueue_st));

	// create idle thread, is needed iff all threads are locked on a queue
	_thr_idle = (lwp_cntrl*)__lwp_objmgr_allocate(&_lwp_thr_objects);
	_thr_idle->information = &_lwp_thr_objects;
	__lwp_thread_init(_thr_idle,NULL,0,255,0,TRUE);
	_thr_executing = _thr_heir = _thr_idle;
	__lwp_thread_start(_thr_idle,idle_func,NULL);
	__lwp_objmgr_open(&_lwp_thr_objects,&_thr_idle->object);

	// create main thread, as this is our entry point
	// for every GC application.
	_thr_main = (lwp_cntrl*)__lwp_objmgr_allocate(&_lwp_thr_objects);
	_thr_main->information = &_lwp_thr_objects;
	__lwp_thread_init(_thr_main,__stack_end,((u32)__stack_addr-(u32)__stack_end),191,0,TRUE);
	_thr_executing = _thr_heir = _thr_main;
	__lwp_thread_start(_thr_main,(void*)main,NULL);
	__lwp_objmgr_open(&_lwp_thr_objects,&_thr_main->object);
}

BOOL __lwp_thread_isalive(lwp_t thr_id)
{
	lwp_cntrl *thethread = (lwp_cntrl*)__lwp_objmgr_get_noprotection(&_lwp_thr_objects,thr_id);
	
	if(thethread && 
		(!__lwp_statedormant(thethread->cur_state) && !__lwp_statetransient(thethread->cur_state)))
		return TRUE;
	
	return FALSE;
}

lwp_t __lwp_thread_currentid()
{
	return _thr_executing->object.id;
}

BOOL __lwp_thread_exists(lwp_t thr_id)
{
	return (__lwp_objmgr_get_noprotection(&_lwp_thr_objects,thr_id)!=NULL);
}

frame_context* __lwp_thread_context(lwp_t thr_id)
{
	lwp_cntrl *thethread;
	frame_context *ctx = NULL;

	thethread = (lwp_cntrl*)__lwp_objmgr_get_noprotection(&_lwp_thr_objects,thr_id);
	if(thethread)  {
		ctx = &thethread->context;
	}
	return ctx;
}

s32 LWP_CreateThread(lwp_t *thethread,void* (*entry)(void *),void *arg,void *stackbase,u32 stack_size,u8 prio)
{
	u32 status;
	lwp_cntrl *lwp_thread;
	
	if(!thethread || !entry) return -1;

	lwp_thread = __lwp_cntrl_allocate();
	if(!lwp_thread) return -1;

	status = __lwp_thread_init(lwp_thread,stackbase,stack_size,__lwp_priotocore(prio),0,TRUE);
	if(!status) {
		__lwp_cntrl_free(lwp_thread);
		__lwp_thread_dispatchenable();
		return -1;
	}
	
	status = __lwp_thread_start(lwp_thread,entry,arg);
	if(!status) {
		__lwp_cntrl_free(lwp_thread);
		__lwp_thread_dispatchenable();
		return -1;
	}

	*thethread = lwp_thread->object.id;
	__lwp_thread_dispatchenable();

	return 0;
}

s32 LWP_SuspendThread(lwp_t thethread)
{
	lwp_cntrl *lwp_thread;

	lwp_thread = __lwp_cntrl_open(thethread);
	if(!lwp_thread) return -1;

	if(!__lwp_statesuspended(lwp_thread->cur_state)) {
		__lwp_thread_suspend(lwp_thread);
		__lwp_thread_dispatchenable();
		return LWP_SUCCESSFUL;
	}
	__lwp_thread_dispatchenable();
	return LWP_ALLREADY_SUSPENDED;
}

s32 LWP_ResumeThread(lwp_t thethread)
{
	lwp_cntrl *lwp_thread;

	lwp_thread = __lwp_cntrl_open(thethread);
	if(!lwp_thread) return -1;

	if(__lwp_statesuspended(lwp_thread->cur_state)) {
		__lwp_thread_resume(lwp_thread,TRUE);
		__lwp_thread_dispatchenable();
		return LWP_SUCCESSFUL;
	}
	__lwp_thread_dispatchenable();
	return LWP_NOT_SUSPENDED;
}

lwp_t LWP_GetSelf()
{
	lwp_t ret;

	__lwp_thread_dispatchdisable();
	ret = (lwp_t)_thr_executing->object.id;
	__lwp_thread_dispatchunnest();

	return ret;
}

void LWP_SetThreadPriority(lwp_t thethread,u32 prio)
{
	lwp_cntrl *lwp_thread;

	if(thethread==-1) thethread = LWP_GetSelf();

	lwp_thread = __lwp_cntrl_open(thethread);
	if(!lwp_thread) return;

	__lwp_thread_changepriority(lwp_thread,__lwp_priotocore(prio),TRUE);
	__lwp_thread_dispatchenable();
}

void LWP_YieldThread()
{
	__lwp_thread_dispatchdisable();
	__lwp_thread_yield();
	__lwp_thread_dispatchenable();
}

void LWP_Reschedule(u32 prio)
{
	__lwp_thread_dispatchdisable();
	__lwp_rotate_readyqueue(prio);
	__lwp_thread_dispatchenable();
}

BOOL LWP_ThreadIsSuspended(lwp_t thethread)
{
	BOOL state;
	lwp_cntrl *lwp_thread;

	lwp_thread = __lwp_cntrl_open(thethread);
  	if(!lwp_thread) return FALSE;
	
	state = (__lwp_statesuspended(lwp_thread->cur_state) ? TRUE : FALSE);

	__lwp_thread_dispatchenable();
	return state;
}


s32 LWP_JoinThread(lwp_t thethread,void **value_ptr)
{
	u32 level;
	void *return_ptr;
	lwp_cntrl *exec,*lwp_thread;
	
	lwp_thread = __lwp_cntrl_open(thethread);
	if(!lwp_thread) return 0;

	if(__lwp_thread_isexec(lwp_thread)) {
		__lwp_thread_dispatchenable();
		return EDEADLK;			//EDEADLK
	}

	exec = _thr_executing;
	_CPU_ISR_Disable(level);
	__lwp_threadqueue_csenter(&lwp_thread->join_list);
	exec->wait.ret_code = 0;
	exec->wait.ret_arg_1 = NULL;
	exec->wait.ret_arg = (void*)&return_ptr;
	exec->wait.queue = &lwp_thread->join_list;
	exec->wait.id = lwp_thread->object.id;
	_CPU_ISR_Restore(level);
	__lwp_threadqueue_enqueue(&lwp_thread->join_list,LWP_WD_NOTIMEOUT);
	__lwp_thread_dispatchenable();

	if(value_ptr) *value_ptr = return_ptr;
	return 0;
}

s32 LWP_InitQueue(lwpq_t *thequeue)
{
	tqueue_st *tq;

	if(!thequeue) return -1;

	tq = __lwp_tqueue_allocate();
	if(!tq) return -1;

	__lwp_threadqueue_init(&tq->tqueue,LWP_THREADQ_MODEFIFO,LWP_STATES_WAITING_ON_THREADQ,0);

	*thequeue = tq->object.id;
	__lwp_thread_dispatchenable();

	return 0;
}

void LWP_CloseQueue(lwpq_t thequeue)
{
	lwp_cntrl *thethread;
	tqueue_st *tq = (tqueue_st*)thequeue;

	tq = __lwp_tqueue_open(thequeue);
	if(!tq) return;
	
	do {
		thethread = __lwp_threadqueue_dequeue(&tq->tqueue);
	} while(thethread);
	__lwp_thread_dispatchenable();

	__lwp_tqueue_free(tq);
	return;
}

s32 LWP_ThreadSleep(lwpq_t thequeue)
{
	u32 level;
	tqueue_st *tq;
	lwp_cntrl *exec = NULL;

	tq = __lwp_tqueue_open(thequeue);
	if(!tq) return -1;

	exec = _thr_executing;
	_CPU_ISR_Disable(level);
	__lwp_threadqueue_csenter(&tq->tqueue);
	exec->wait.ret_code = 0;
	exec->wait.ret_arg = NULL;
	exec->wait.ret_arg_1 = NULL;
	exec->wait.queue = &tq->tqueue;
	exec->wait.id = tq->object.id;
	_CPU_ISR_Restore(level);
	__lwp_threadqueue_enqueue(&tq->tqueue,LWP_THREADQ_NOTIMEOUT);
	__lwp_thread_dispatchenable();
	return 0;
}

void LWP_ThreadBroadcast(lwpq_t thequeue)
{
	tqueue_st *tq;
	lwp_cntrl *thethread;

	tq = __lwp_tqueue_open(thequeue);
	if(!tq) return;
	
	do {
		thethread = __lwp_threadqueue_dequeue(&tq->tqueue);
	} while(thethread);
	__lwp_thread_dispatchenable();
}

void LWP_ThreadSignal(lwpq_t thequeue)
{
	tqueue_st *tq;
	lwp_cntrl *thethread;

	tq = __lwp_tqueue_open(thequeue);
	if(!tq) return;

	thethread = __lwp_threadqueue_dequeue(&tq->tqueue);
	__lwp_thread_dispatchenable();
}
