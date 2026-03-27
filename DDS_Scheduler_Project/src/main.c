/* Standard includes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "stm32f4_discovery.h"
#include "stm32f4xx_adc.h"
#include "stm32f4xx_gpio.h"

/* Kernel includes. */
#include "stm32f4xx.h"
#include "../FreeRTOS_Source/include/FreeRTOS.h"
#include "../FreeRTOS_Source/include/queue.h"
#include "../FreeRTOS_Source/include/semphr.h"
#include "../FreeRTOS_Source/include/task.h"
#include "../FreeRTOS_Source/include/timers.h"

static void prvSetupHardware(void);
void myDDSInit(void);

/*------------------------------------------------------------------*/
/*---------------------------- DEFINES ---------------------------- */
#define mainQUEUE_LENGTH 10

#define YES         1
#define NO          0

#define PRIORITY_DDS        4  // Highest priority for the scheduler itself
#define PRIORITY_HIGH       3  // For the Generator
#define PRIORITY_MED		2  // For the EDF Head task
#define PRIORITY_LOW        1  // For all other active tasks

// Let's create handles for all 3 tasks and the generator
TaskHandle_t xUserTask1;
TaskHandle_t xUserTask2;
TaskHandle_t xUserTask3;
TaskHandle_t xGeneratorTask;
TaskHandle_t xMonitorTask;

TimerHandle_t xTimerUser1;
TimerHandle_t xTimerUser2;
TimerHandle_t xTimerUser3;

/*---------------------------- TEST BENCHES ---------------------------- */
// Change this to TB_2 or TB_3 to test the other scenarios
#define TB_1

#ifdef TB_1
    #define T1_EXEC   pdMS_TO_TICKS(95)
    #define T1_PERIOD pdMS_TO_TICKS(500)
    #define T2_EXEC   pdMS_TO_TICKS(150)
    #define T2_PERIOD pdMS_TO_TICKS(500)
    #define T3_EXEC   pdMS_TO_TICKS(250)
    #define T3_PERIOD pdMS_TO_TICKS(750)
#elif defined(TB_2)
    #define T1_EXEC   pdMS_TO_TICKS(95)
    #define T1_PERIOD pdMS_TO_TICKS(250)
    #define T2_EXEC   pdMS_TO_TICKS(150)
    #define T2_PERIOD pdMS_TO_TICKS(500)
    #define T3_EXEC   pdMS_TO_TICKS(250)
    #define T3_PERIOD pdMS_TO_TICKS(750)
#elif defined(TB_3)
    #define T1_EXEC   pdMS_TO_TICKS(100)
    #define T1_PERIOD pdMS_TO_TICKS(500)
    #define T2_EXEC   pdMS_TO_TICKS(200)
    #define T2_PERIOD pdMS_TO_TICKS(500)
    #define T3_EXEC   pdMS_TO_TICKS(200)
    #define T3_PERIOD pdMS_TO_TICKS(500)
#endif


/*---------------------------- Structures ---------------------------- */

typedef enum {PERIODIC, APERIODIC} task_type;

typedef struct {
    TaskHandle_t t_handle;
    task_type type;
    uint32_t task_id;
    uint32_t release_time;
    uint32_t absolute_deadline;
    uint32_t completion_time;
} dd_task;

typedef struct dd_task_list {
    dd_task task;
    struct dd_task_list *next_task;
} dd_task_list;

typedef enum {
    MSG_CREATE,
    MSG_DELETE,
    MSG_ACTIVE_LIST,
    MSG_COMPLETED_LIST,
    MSG_OVERDUE_LIST
} message_type;

typedef struct {
    message_type type;
    dd_task task;
    uint32_t task_id; // Used for DELETE messages
} dd_message;

/*---------------------------- Function Prototypes ---------------------------- */
void insert_node(dd_task_list **head, dd_task new_task);
dd_task_list* remove_node(dd_task_list **head, uint32_t task_id);
uint32_t get_list_size(dd_task_list *head);

void adjust_priorities(void);
void handle_release(dd_task new_task);
void handle_complete(uint32_t task_id);
void handle_deadline_miss(void);

void release_dd_task(TaskHandle_t t_handle, task_type type, uint32_t task_id, uint32_t absolute_deadline);
void complete_dd_task(uint32_t task_id);
dd_task_list* get_active_dd_task_list(void);
dd_task_list* get_completed_dd_task_list(void);
dd_task_list* get_overdue_dd_task_list(void);

void DD_Scheduler(void *pvParameters);
void vUserTask1(void *pvParameters);
void vUserTask2(void *pvParameters);
void vUserTask3(void *pvParameters);
void vTaskGenerator(void *pvParameters);
void vCallbackTask1(TimerHandle_t xTimer);
void vCallbackTask2(TimerHandle_t xTimer);
void vCallbackTask3(TimerHandle_t xTimer);
void vMonitorTask(void *pvParameters);

/*---------------------------- FreeRTOS Queues ---------------------------- */
QueueHandle_t xMessageQueue;
QueueHandle_t xReplyQueue;
QueueHandle_t xListQueue;
QueueHandle_t xGeneratorQueue;

/*---------------------------- Global Variables ---------------------------- */
// Global pointers for our 3 lists
dd_task_list *active_list = NULL;
dd_task_list *completed_list = NULL;
dd_task_list *overdue_list = NULL;

// Global timeout tracker for the queue
TickType_t current_timeout = portMAX_DELAY;

/*---------------------------- MAIN ---------------------------- */

int main(void)
{
    // 1. Initialize hardware (clocks, etc.)
    prvSetupHardware();

    // 2. Initialize DDS Queues, Tasks, and Timers
    myDDSInit();

    // 3. Start the RTOS scheduler
    vTaskStartScheduler();


    // The program should never reach here
    for (;;);
    return 0;
}

/*---------------------------- Initialization ---------------------------- */

void myDDSInit(void) {
    // 1. Create Queues
    xMessageQueue = xQueueCreate(mainQUEUE_LENGTH, sizeof(dd_message));
    xReplyQueue   = xQueueCreate(mainQUEUE_LENGTH, sizeof(uint32_t));
    xListQueue    = xQueueCreate(mainQUEUE_LENGTH, sizeof(dd_task_list*));
    xGeneratorQueue = xQueueCreate(mainQUEUE_LENGTH, sizeof(uint32_t));

    if (xMessageQueue == NULL || xReplyQueue == NULL || xListQueue == NULL || xGeneratorQueue == NULL) {
        printf("Queue Creation Failed!\n");
        for (;;);
    }

    // 2. Create DDS Task (Highest Priority)
    xTaskCreate(DD_Scheduler, "DDS", 256, NULL, PRIORITY_DDS, NULL);

    // 3. Create User Tasks (Start Suspended)
    xTaskCreate(vUserTask1, "T1", configMINIMAL_STACK_SIZE, NULL, PRIORITY_LOW, &xUserTask1);
    vTaskSuspend(xUserTask1);
    xTaskCreate(vUserTask2, "T2", configMINIMAL_STACK_SIZE, NULL, PRIORITY_LOW, &xUserTask2);
    vTaskSuspend(xUserTask2);
    xTaskCreate(vUserTask3, "T3", configMINIMAL_STACK_SIZE, NULL, PRIORITY_LOW, &xUserTask3);
    vTaskSuspend(xUserTask3);

    // 4. Create Generator Task
    xTaskCreate(vTaskGenerator, "GEN", configMINIMAL_STACK_SIZE, NULL, PRIORITY_HIGH, &xGeneratorTask);

    // 5. Create and Start the Timers
    xTimerUser1 = xTimerCreate("TMR1", T1_PERIOD, pdTRUE, 0, vCallbackTask1);
    xTimerUser2 = xTimerCreate("TMR2", T2_PERIOD, pdTRUE, 0, vCallbackTask2);
    xTimerUser3 = xTimerCreate("TMR3", T3_PERIOD, pdTRUE, 0, vCallbackTask3);

    xTimerStart(xTimerUser1, 0);
    xTimerStart(xTimerUser2, 0);
    xTimerStart(xTimerUser3, 0);

    // 6. Jump-start the generator so all 3 tasks release at t=0
    uint32_t id1 = 1, id2 = 2, id3 = 3;
    xQueueSend(xGeneratorQueue, &id1, 0);
    xQueueSend(xGeneratorQueue, &id2, 0);
    xQueueSend(xGeneratorQueue, &id3, 0);

    // 7. Create Monitor Task
    xTaskCreate(vMonitorTask, "MON", configMINIMAL_STACK_SIZE, NULL, PRIORITY_LOW, &xMonitorTask);
}


/*---------------------------- List Helper Functions ---------------------------- */

void insert_node(dd_task_list **head, dd_task new_task){

	//If memory is avaiable, create a new node
    dd_task_list *new_node = (dd_task_list *)pvPortMalloc(sizeof(dd_task_list));

    //If the node is unsuccessfully enabled, ie not enough memory, send a warning
    if (new_node == NULL) {
        printf("Memory allocation failed\n");
        return;
    }

    new_node->task = new_task;
    new_node->next_task = NULL;

    // Case 1: Empty list OR new task has the earliest deadline (new head)
    // If the head is empty, or the new deadline is less than the current head deadline
    if (*head == NULL || (*head)->task.absolute_deadline > new_task.absolute_deadline) {
        new_node->next_task = *head;
        *head = new_node;
        return;
    }

    // Case 2: Insert in the middle or at the tail
    // Search each node within the stack to find where it should sit based on deadline
    dd_task_list *current = *head;
    while (current->next_task != NULL &&
           current->next_task->task.absolute_deadline <= new_task.absolute_deadline) {
        current = current->next_task;
    }

    new_node->next_task = current->next_task;
    current->next_task = new_node;
}

dd_task_list* remove_node(dd_task_list **head, uint32_t task_id){
    if (*head == NULL) return NULL; // Empty list

    dd_task_list *current = *head;
    dd_task_list *prev = NULL;

    // Search for the node
    while (current != NULL && current->task.task_id != task_id) {
        prev = current;
        current = current->next_task;
    }

    // Target not found
    if (current == NULL) return NULL;

    // Case 1: Removing the head
    if (prev == NULL) {
        *head = current->next_task;
    }
    // Case 2: Removing from the middle or tail
    else {
        prev->next_task = current->next_task;
    }

    current->next_task = NULL; // Sever the link to the old list
    return current;
}

uint32_t get_list_size(dd_task_list *head){
    uint32_t count = 0;
    dd_task_list *current = head;

    while (current != NULL) {
        count++;
        current = current->next_task;
    }
    return count;
}


/*---------------------------- DDS State Machine Functions ---------------------------- */

//adjust_priorities() — set head of Active List to MED, others to LOW; recalculate timeout
void adjust_priorities(void) {
    if (active_list == NULL) {
        current_timeout = portMAX_DELAY; // Nothing to do, wait forever
        return;
    }

    // 1. Set the head task to MED priority AND wake it up!
    if (active_list->task.t_handle != NULL) {
        vTaskPrioritySet(active_list->task.t_handle, PRIORITY_MED);
        vTaskResume(active_list->task.t_handle);
    }

    // 2. Demote all other active tasks to LOW priority
    dd_task_list *curr = active_list->next_task;
    while (curr != NULL) {
        if (curr->task.t_handle != NULL) {
            vTaskPrioritySet(curr->task.t_handle, PRIORITY_LOW);
        }
        curr = curr->next_task;
    }

    // 3. Recalculate queue timeout based on the head task's deadline
    TickType_t current_time = xTaskGetTickCount();
    if (active_list->task.absolute_deadline > current_time) {
        current_timeout = active_list->task.absolute_deadline - current_time;
    } else {
        current_timeout = 0; // Deadline has already passed
    }
}

// handle_release() — assign release time, insert into Active List, adjust priorities
void handle_release(dd_task new_task) {
    new_task.release_time = xTaskGetTickCount();
    insert_node(&active_list, new_task);
    adjust_priorities();
}

// handle_complete() — assign completion time, move from Active to Completed, adjust priorities
void handle_complete(uint32_t task_id) {
    dd_task_list *completed_node = remove_node(&active_list, task_id);

    if (completed_node != NULL) {
        completed_node->task.completion_time = xTaskGetTickCount();

        // Push to the head of the completed list (order doesn't matter here)
        completed_node->next_task = completed_list;
        completed_list = completed_node;
    }
    adjust_priorities();
}

// handle_deadline_miss() — move head of Active to Overdue, signal Generator
void handle_deadline_miss(void) {
    if (active_list == NULL) return;

    // The head is the one that missed the deadline
    dd_task_list *missed_node = active_list;
    active_list = active_list->next_task; // Remove from active

    // Suspend the F-Task so it stops consuming CPU time
    if (missed_node->task.t_handle != NULL) {
        vTaskSuspend(missed_node->task.t_handle);
    }

    // Move to overdue list
    missed_node->next_task = overdue_list;
    overdue_list = missed_node;

    // Output for testing/checkpoint
    printf("\n>>> DEADLINE MISSED for Task ID: %u <<<\n", missed_node->task.task_id);

    uint32_t missed_id = missed_node->task.task_id;
    xQueueSend(xGeneratorQueue, &missed_id, portMAX_DELAY);

    adjust_priorities(); // Setup for the next task in line
}


/*---------------------------- Interface Functions ---------------------------- */

void release_dd_task(TaskHandle_t t_handle, task_type type, uint32_t task_id, uint32_t absolute_deadline) {
    uint32_t reply;

    // Package the task data
    dd_task new_task = {
        .t_handle = t_handle,
        .type = type,
        .task_id = task_id,
        .absolute_deadline = absolute_deadline,
        .release_time = 0,     // Set by DDS
        .completion_time = 0   // Set by DDS
    };

    // Package the message
    dd_message message = { .type = MSG_CREATE, .task = new_task };

    // Send to DDS and wait for SUCCESS reply
    xQueueSend(xMessageQueue, &message, portMAX_DELAY);
    xQueueReceive(xReplyQueue, &reply, portMAX_DELAY);
}

void complete_dd_task(uint32_t task_id) {
    uint32_t reply;
    dd_message message = { .type = MSG_DELETE, .task_id = task_id };

    xQueueSend(xMessageQueue, &message, portMAX_DELAY);
    xQueueReceive(xReplyQueue, &reply, portMAX_DELAY);
}

dd_task_list* get_active_dd_task_list(void) {
    dd_task_list *list_copy;
    dd_message message = { .type = MSG_ACTIVE_LIST };

    xQueueSend(xMessageQueue, &message, portMAX_DELAY);
    xQueueReceive(xListQueue, &list_copy, portMAX_DELAY);
    return list_copy;
}

dd_task_list* get_completed_dd_task_list(void) {
    dd_task_list *list_copy;
    dd_message message = { .type = MSG_COMPLETED_LIST };

    xQueueSend(xMessageQueue, &message, portMAX_DELAY);
    xQueueReceive(xListQueue, &list_copy, portMAX_DELAY);
    return list_copy;
}

dd_task_list* get_overdue_dd_task_list(void) {
    dd_task_list *list_copy;
    dd_message message = { .type = MSG_OVERDUE_LIST };

    xQueueSend(xMessageQueue, &message, portMAX_DELAY);
    xQueueReceive(xListQueue, &list_copy, portMAX_DELAY);
    return list_copy;
}

/*---------------------------- Tasks ---------------------------- */

//DD_Scheduler task — main loop: receive message -> handle it -> block again
void DD_Scheduler(void *pvParameters) {
    dd_message message_received;
    uint32_t reply_status = YES;

    for (;;) {
        // Block until a message arrives OR the deadline timeout expires
        if (xQueueReceive(xMessageQueue, &message_received, current_timeout) == pdTRUE) {

            // We received a message before a deadline fired
            switch (message_received.type) {
                case MSG_CREATE:
                    handle_release(message_received.task);
                    xQueueSend(xReplyQueue, &reply_status, portMAX_DELAY);
                    break;

                case MSG_DELETE:
                    handle_complete(message_received.task_id);
                    xQueueSend(xReplyQueue, &reply_status, portMAX_DELAY);
                    break;

                case MSG_ACTIVE_LIST:
                    xQueueSend(xListQueue, &active_list, portMAX_DELAY);
                    break;

                case MSG_COMPLETED_LIST:
                    xQueueSend(xListQueue, &completed_list, portMAX_DELAY);
                    break;

                case MSG_OVERDUE_LIST:
                    xQueueSend(xListQueue, &overdue_list, portMAX_DELAY);
                    break;
            }
        }
        else {
            // The queue timed out! A deadline has been missed.
            handle_deadline_miss();
        }
    }
}

/*---------------------------- User Tasks ---------------------------- */
void vUserTask1(void *pvParameters) {
    TickType_t currentTick, executeTick, prevTick;
    for (;;) {
        executeTick = T1_EXEC;
        currentTick = xTaskGetTickCount();
        while (executeTick > 0) {
            prevTick = currentTick;
            currentTick = xTaskGetTickCount();
            if (currentTick != prevTick) executeTick--;
        }
        printf("Task 1 Completed at %u\n", xTaskGetTickCount());
        complete_dd_task(1);
        vTaskSuspend(NULL);
    }
}

void vUserTask2(void *pvParameters) {
    TickType_t currentTick, executeTick, prevTick;
    for (;;) {
        executeTick = T2_EXEC;
        currentTick = xTaskGetTickCount();
        while (executeTick > 0) {
            prevTick = currentTick;
            currentTick = xTaskGetTickCount();
            if (currentTick != prevTick) executeTick--;
        }
        printf("Task 2 Completed at %u\n", xTaskGetTickCount());
        complete_dd_task(2);
        vTaskSuspend(NULL);
    }
}

void vUserTask3(void *pvParameters) {
    TickType_t currentTick, executeTick, prevTick;
    for (;;) {
        executeTick = T3_EXEC;
        currentTick = xTaskGetTickCount();
        while (executeTick > 0) {
            prevTick = currentTick;
            currentTick = xTaskGetTickCount();
            if (currentTick != prevTick) executeTick--;
        }
        printf("Task 3 Completed at %u\n", xTaskGetTickCount());
        complete_dd_task(3);
        vTaskSuspend(NULL);
    }
}

/*---------------------------- Task Generator ---------------------------- */

// Timers feed the Generator Queue
void vCallbackTask1(TimerHandle_t xTimer) { uint32_t id = 1; xQueueSendFromISR(xGeneratorQueue, &id, NULL); }
void vCallbackTask2(TimerHandle_t xTimer) { uint32_t id = 2; xQueueSendFromISR(xGeneratorQueue, &id, NULL); }
void vCallbackTask3(TimerHandle_t xTimer) { uint32_t id = 3; xQueueSendFromISR(xGeneratorQueue, &id, NULL); }

void vTaskGenerator(void *pvParameters) {
    uint32_t current_task_id;
    // Arrays to track the previous deadlines to calculate the new absolute deadline
    uint32_t previous_deadlines[3] = {0, 0, 0};

    for (;;) {
        // Block until a timer OR a deadline miss tells us to release a task
        if (xQueueReceive(xGeneratorQueue, &current_task_id, portMAX_DELAY) == pdTRUE) {

            uint32_t period = 0;
            TaskHandle_t handle = NULL;

            // Map ID to its parameters
            if (current_task_id == 1)      { period = T1_PERIOD; handle = xUserTask1; }
            else if (current_task_id == 2) { period = T2_PERIOD; handle = xUserTask2; }
            else if (current_task_id == 3) { period = T3_PERIOD; handle = xUserTask3; }

            // Calculate new absolute deadline: previous deadline + relative deadline
            previous_deadlines[current_task_id - 1] += period;
            uint32_t absolute_deadline = previous_deadlines[current_task_id - 1];

            printf("Generator: Releasing Task %u (Deadline: %u)\n", current_task_id, absolute_deadline);

            // Call the interface function
            release_dd_task(handle, PERIODIC, current_task_id, absolute_deadline);
        }
    }
}

/*---------------------------- Monitor Task ---------------------------- */

void vMonitorTask(void *pvParameters) {
    for (;;) {
        // Wake up every 500ms to check the system state
        vTaskDelay(pdMS_TO_TICKS(500));

        // 1. Request the list pointers from the DDS
        dd_task_list *active = get_active_dd_task_list();
        dd_task_list *completed = get_completed_dd_task_list();
        dd_task_list *overdue = get_overdue_dd_task_list();

        // 2. Count the nodes in each list
        uint32_t active_count = get_list_size(active);
        uint32_t completed_count = get_list_size(completed);
        uint32_t overdue_count = get_list_size(overdue);

        // 3. Print the report
        printf("\n======================================\n");
        printf(" Monitor Report at %u ms\n", xTaskGetTickCount());
        printf("   Active Tasks:    %u\n", active_count);
        printf("   Completed Tasks: %u\n", completed_count);
        printf("   Overdue Tasks:   %u\n", overdue_count);
        printf("======================================\n\n");
    }
}


/*---------------------------- FreeRTOS Hooks & Setup ---------------------------- */

void vApplicationMallocFailedHook( void )
{
   /* Called if a call to pvPortMalloc() fails because there is insufficient
   free memory available in the FreeRTOS heap. */
   for( ;; );
}

void vApplicationStackOverflowHook( TaskHandle_t pxTask, signed char *pcTaskName )
{
   ( void ) pcTaskName;
   ( void ) pxTask;
   for( ;; );
}

void vApplicationIdleHook( void )
{
   volatile size_t xFreeStackSpace;
   xFreeStackSpace = xPortGetFreeHeapSize();
   if( xFreeStackSpace > 100 )
   {
       // Memory is sufficient
   }
}

static void prvSetupHardware( void )
{
   /* Ensure all priority bits are assigned as preemption priority bits. */
   NVIC_SetPriorityGrouping( 0 );
   /* TODO: Setup the clocks, etc. here, if they were not configured before main() was called. */
}