#include "os.h"
#include "os_idle_task.h"

#define OS_TASKS_COUNT	(OS_CONFIG_MAX_TASKS + 1)	/* +1 for idle task */

typedef enum {
	OS_TASK_STATUS_IDLE = 1,
	OS_TASK_STATUS_ACTIVE,
	OS_TASK_STATUS_WAITING,
} os_task_status_t;

typedef struct {
	/* The stack pointer (sp) has to be the first element as it is located
	   at the same address as the structure itself (which makes it possible
	   to locate it safely from assembly implementation of PendSV_Handler).
	   The compiler might add padding between other structure elements. */
	volatile uint32_t sp;
	void (*handler)(void *p_params);
	void *p_params;
	volatile uint32_t wait_ticks;
	volatile os_task_status_t status;
} os_task_t;

static enum {
	OS_STATE_DEFAULT = 1,
	OS_STATE_INITIALIZED,
	OS_STATE_TASKS_INITIALIZED,
	OS_STATE_STARTED,
} m_state = OS_STATE_DEFAULT;

static struct {
	os_task_t tasks[OS_TASKS_COUNT];
	volatile uint32_t current_task;
	uint32_t size;
} m_task_table;

volatile os_task_t *os_curr_task;
volatile os_task_t *os_next_task;

static void task_finished(void)
{
	/* This function is called when some task handler returns. */
	OS_ERROR_HANDLER(OS_ERROR_TASK_FINISHED);

	volatile uint32_t i = 0;
	while (1)
		i++;
}

static void reschedule(void)
{
	/* TODO: Is this neccessary? The function might always run in the
	   highest priority. */
	__disable_irq();

	int i;
	bool no_ready_task = true;

	os_curr_task = &m_task_table.tasks[m_task_table.current_task];

	/* Check if there is any task ready to be scheduled: */
	if (m_task_table.current_task != 0 &&
	    os_curr_task->status == OS_TASK_STATUS_ACTIVE) {
		os_curr_task->status = OS_TASK_STATUS_IDLE;
		no_ready_task = false;
	} else {
		for (i = 1; i < m_task_table.size; i++) {
			if (m_task_table.tasks[i].status == OS_TASK_STATUS_IDLE)
			{
				no_ready_task = false;
				break;
			}
		}
	}

	if (no_ready_task) {
		/* No task is ready, use the idle task: */
		m_task_table.current_task = 0;
		os_next_task = &m_task_table.tasks[m_task_table.current_task];
	} else {
		/* Select the next ready task: */
		do {
			m_task_table.current_task++;
			if (m_task_table.current_task >= m_task_table.size)
				m_task_table.current_task = 1;

			os_next_task = &m_task_table.tasks[m_task_table.current_task];
		} while (os_next_task->status != OS_TASK_STATUS_IDLE);
	}

	os_next_task->status = OS_TASK_STATUS_ACTIVE;

	/* Trigger PendSV which performs the actual context switch: */
	if (os_curr_task != os_next_task)
		SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;

	__enable_irq();
}

os_error_t os_init(void)
{
	if (m_state != OS_STATE_DEFAULT)
		return OS_ERROR_WRONG_STATE;

	memset(&m_task_table, 0, sizeof(m_task_table));

	m_state = OS_STATE_INITIALIZED;
	os_error_t err_code = os_idle_task_init();

	if (err_code != OS_ERROR_OK) {
		m_state = OS_STATE_DEFAULT;
		return err_code;
	}

	m_task_table.current_task = 1;

	return OS_ERROR_OK;
}

os_error_t os_task_init(void (*handler)(void *p_params), void *p_task_params,
			os_stack_t *p_stack, size_t stack_size)
{
	if (m_state != OS_STATE_INITIALIZED &&
	    m_state != OS_STATE_TASKS_INITIALIZED)
		return OS_ERROR_WRONG_STATE;

	if (m_task_table.size >= OS_TASKS_COUNT-1)
		return OS_ERROR_NO_MEM;

	/* Initialize the task structure and set SP to the top of the stack
	   minus 16 words (64 bytes) to leave space for storing 16 registers: */
	os_task_t *p_task = &m_task_table.tasks[m_task_table.size];
	p_task->handler = handler;
	p_task->p_params = p_task_params;
	p_task->sp = (uint32_t)(p_stack+stack_size-16);
	p_task->status = OS_TASK_STATUS_IDLE;

	/* Save init. values of registers which will be restored on exc. return:
	   - XPSR: Default value (0x01000000)
	   - PC: Point to the handler function
	   - LR: Point to a function to be called when the handler returns
	   - R0: Point to the handler function's parameter */
	p_stack[stack_size-1] = 0x01000000;
	p_stack[stack_size-2] = (uint32_t)handler;
	p_stack[stack_size-3] = (uint32_t)&task_finished;
	p_stack[stack_size-8] = (uint32_t)p_task_params;

#ifdef OS_CONFIG_DEBUG
	uint32_t base = (m_task_table.size+1)*1000;
	p_stack[stack_size-4] = base+12;  /* R12 */
	p_stack[stack_size-5] = base+3;   /* R3  */
	p_stack[stack_size-6] = base+2;   /* R2  */
	p_stack[stack_size-7] = base+1;   /* R1  */
	/* p_stack[stack_size-8] is R0 */
	p_stack[stack_size-9] = base+7;   /* R7  */
	p_stack[stack_size-10] = base+6;  /* R6  */
	p_stack[stack_size-11] = base+5;  /* R5  */
	p_stack[stack_size-12] = base+4;  /* R4  */
	p_stack[stack_size-13] = base+11; /* R11 */
	p_stack[stack_size-14] = base+10; /* R10 */
	p_stack[stack_size-15] = base+9;  /* R9  */
	p_stack[stack_size-16] = base+8;  /* R8  */
#endif

	m_state = OS_STATE_TASKS_INITIALIZED;
	m_task_table.size++;

	return OS_ERROR_OK;
}

os_error_t os_start(uint32_t systick_ticks)
{
	if (m_state != OS_STATE_TASKS_INITIALIZED)
		return OS_ERROR_WRONG_STATE;

	NVIC_SetPriority(PendSV_IRQn, 0xff); /* Lowest possible priority */
	NVIC_SetPriority(SVC_IRQn, 0x00); /* Highest possible priority */
	NVIC_SetPriority(SysTick_IRQn, 0x00);

	/* Start the SysTick timer: */
	uint32_t ret_val = SysTick_Config(systick_ticks);
	if (ret_val != 0)
		return OS_ERROR_INVALID_PARAM;

	/* Start the first task: */
	os_curr_task = &m_task_table.tasks[m_task_table.current_task];
	m_state = OS_STATE_STARTED;

	__set_PSP(os_curr_task->sp+64); /* Set PSP to the top of task's stack */
	__set_CONTROL(0x03); /* Switch to Unprivilleged Thread Mode with PSP */
	__ISB(); /* Execute ISB after changing CONTORL (recommended) */

	os_curr_task->handler(os_curr_task->p_params);

	return OS_ERROR_OK;
}

os_error_t os_delay(uint32_t ticks)
{
	if (m_state != OS_STATE_STARTED)
		return OS_ERROR_WRONG_STATE;

	/* TODO: Is it neccessary to disable IRQ? Shouldn't this code be invoked
	   by SVCall as well? Is it even possible to call __disable_irq from
	   tasks which run in unprivileged mode? */
	__disable_irq();
	os_task_t *p_task = &m_task_table.tasks[m_task_table.current_task];
	p_task->status = OS_TASK_STATUS_WAITING;
	p_task->wait_ticks = ticks;
	__enable_irq();

	asm volatile("svc 0x01"); /* reschedule() */

	while (p_task->status == OS_TASK_STATUS_WAITING);

	return OS_ERROR_OK;
}

void SysTick_Handler(void)
{
	int i;

	/* Decrease waiting counters: */
	for (i = 1; i < m_task_table.size; i++) {
		os_task_t *p_task = &m_task_table.tasks[i];
		if (p_task->status == OS_TASK_STATUS_WAITING) {
			p_task->wait_ticks--;
			if (p_task->wait_ticks == 0)
				p_task->status = OS_TASK_STATUS_IDLE;
		}
	}

	os_reschedule();
}

void os_svcall_handler(uint8_t svc_num, uint32_t *p_stack_frame)
{
	/* Stack frame format:
	+------+
	| xPSR | <- p_stack_frame[7]
	|  PC  |
	|  LR  |
	|  R12 |
	|  R3  |
	|  R2  |
	|  R1  |
	|  R0  | <- p_stack_frame[0]
	+------+
	*/

	switch (svc_num) {
	case 0x01:
		reschedule();
		break;
	default:
		/* No action */
		break;
	}
}
