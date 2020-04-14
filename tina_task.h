#include <stdint.h>
#include <threads.h>
#include <assert.h>

#include "tina.h"

#ifndef TINA_TASK_H
#define TINA_TASK_H

#define TINA_TASKS_MAX_COROS (256)
#define TINA_TASKS_MAX_TASKS (1024)
#define TINA_TASKS_STACK_SIZE (64*1024)

typedef struct tina_task tina_task;
typedef struct tina_counter tina_counter;
typedef void tina_task_func(tina_task* task);

struct tina_task {
	const char* name;
	tina_task_func* func;
	void* ptr;
	
	tina* _coro;
	tina_counter* _counter;
};

typedef struct {
	tina* coros[TINA_TASKS_MAX_COROS];
	unsigned coro_head, coro_tail, coro_count;
	
	tina_task* pool[TINA_TASKS_MAX_TASKS];
	unsigned pool_head, pool_tail, pool_count;
	
	tina_task* tasks[TINA_TASKS_MAX_TASKS];
	unsigned task_head, task_tail, task_count;
	
	mtx_t lock;
	cnd_t tasks_available;
	uint8_t CORO_BUFFER[TINA_TASKS_MAX_COROS][TINA_TASKS_STACK_SIZE];
	tina_task TASK_BUFFER[TINA_TASKS_MAX_TASKS];
} tina_tasks;

struct tina_counter {
	unsigned count;
	tina_task* task;
};

void tina_tasks_init(tina_tasks *tasks);
void tina_tasks_worker_loop(tina_tasks* tasks);

void tina_tasks_enqueue(tina_tasks* tasks, const tina_task* list, size_t count, tina_counter* counter);
void tina_tasks_wait(tina_tasks* tasks, tina_task* task, tina_counter* counter);

#ifdef TINA_IMPLEMENTATION

static uintptr_t _task_body(tina* coro, uintptr_t value){
	tina_tasks* tasks = (tina_tasks*)coro->user_data;
	
	while(true){
		mtx_unlock(&tasks->lock);
		
		tina_task* task = (tina_task*)value;
		task->func(task);
		
		mtx_lock(&tasks->lock);
		value = tina_yield(coro, true);
	}
}

void tina_tasks_init(tina_tasks *tasks){
	// TODO this needs to be zeroed.
	
	for(unsigned i = 0; i < TINA_TASKS_MAX_COROS; i++){
		tina* coro = tina_init(tasks->CORO_BUFFER[i], TINA_TASKS_STACK_SIZE, _task_body, &tasks);
		coro->name = "TINA TASK WORKER";
		coro->user_data = tasks;
		tasks->coros[i] = coro;
	}
	tasks->coro_count = TINA_TASKS_MAX_COROS;
	
	for(unsigned i = 0; i < TINA_TASKS_MAX_TASKS; i++) tasks->pool[i] = &tasks->TASK_BUFFER[i];
	tasks->pool_count = TINA_TASKS_MAX_TASKS;
}

void tina_tasks_worker_loop(tina_tasks* tasks){
	mtx_lock(&tasks->lock);
	while(true){
		// Wait for a task to become available.
		while(tasks->task_count == 0){
			// TODO should be a timed wait to allow shutting down the thread.
			cnd_wait(&tasks->tasks_available, &tasks->lock);
		}
		
		// Dequeue a task and a coroutine to run it on.
		tina_task* task = tasks->tasks[tasks->task_tail++ & (TINA_TASKS_MAX_TASKS - 1)];
		tasks->task_count--;
		
		assert(tasks->coro_count > 0);
		task->_coro = tasks->coros[tasks->coro_tail++ & (TINA_TASKS_MAX_COROS - 1)];
		tasks->coro_count--;
		
		run_again: {
			bool finished = tina_yield(task->_coro, (uintptr_t)task);
			
			if(finished){
				// Task is finished. Return it and it's coroutine to the pool.
				tasks->pool[tasks->pool_head++ & (TINA_TASKS_MAX_TASKS - 1)] = task;
				tasks->pool_count++;
				
				tasks->coros[tasks->coro_head++ & (TINA_TASKS_MAX_COROS - 1)] = task->_coro;
				tasks->coro_count++;
				
				tina_counter* counter = task->_counter;
				if(counter){
					task = counter->task;
					// This was the last task the counter was waiting on. Wake up the waiting task.
					if(--counter->count == 0) goto run_again;
				}
			}
		}
	}
	mtx_unlock(&tasks->lock);
}

void tina_tasks_enqueue(tina_tasks* tasks, const tina_task* list, size_t count, tina_counter* counter){
	if(counter) counter->count = count + 1;
	
	mtx_lock(&tasks->lock);
	assert(count <= tasks->pool_count);
	
	for(size_t i = 0; i < count; i++){
		tina_task task_copy = list[i];
		task_copy._counter = counter;
		
		tina_task* task = tasks->pool[tasks->pool_tail++ & (TINA_TASKS_MAX_TASKS - 1)];
		tasks->pool_count--;
		*task = task_copy;
		
		tasks->tasks[tasks->task_head++ & (TINA_TASKS_MAX_TASKS - 1)] = task;
		tasks->task_count++;
	}
	
	cnd_broadcast(&tasks->tasks_available);
	mtx_unlock(&tasks->lock);
}

void tina_tasks_wait(tina_tasks* tasks, tina_task* task, tina_counter* counter){
	assert(counter->task == NULL);
	counter->task = task;
	
	mtx_lock(&tasks->lock);
	// If there are any unfinished tasks, yield.
	if(--counter->count > 0) tina_yield(counter->task->_coro, false);
	mtx_unlock(&tasks->lock);
}

#endif // TINA_TASK_IMPLEMENTATION
#endif // TINA_TASK_H