/* Standard includes. */
#include <stdint.h>
#include <stdio.h>
#include "stm32f4_discovery.h"
#include "stm32f4xx_adc.h"
#include "stm32f4xx_gpio.h"
#include <stdlib.h>
#include <time.h>

/* Kernel includes. */
#include "stm32f4xx.h"
#include "../FreeRTOS_Source/include/FreeRTOS.h"
#include "../FreeRTOS_Source/include/queue.h"
#include "../FreeRTOS_Source/include/semphr.h"
#include "../FreeRTOS_Source/include/task.h"
#include "../FreeRTOS_Source/include/timers.h"

/*-----------------------------------------------------------*/
#define mainQUEUE_LENGTH 100

#define red_led     GPIO_Pin_0
#define amber_led   GPIO_Pin_1
#define green_led   GPIO_Pin_2
#define POT_PIN     GPIO_Pin_3

#define DATA_PIN    GPIO_Pin_6
#define CLOCK_PIN   GPIO_Pin_7
#define RESET_PIN   GPIO_Pin_8

#define YES         1
#define NO          0

#define red         0x01
#define amber       0x02
#define green       0x04
#define all         0x07

//Functions
void ADC_init(void);
void GPIOC_init(void);
void Queue_init(void);

//Task Prototypes
void vPotTask (void *pvParameters);

//Traffic Tasks Prototypes
void vTrafficFlowTask (void *pvParameters);
void vTrafficLightAdjustmentTask (void *pvParameters);
void vTrafficLight (void *pvParameters);
void vTrafficCreate (void *pvParameters);
void vTrafficDisplay (void *pvParameters);

// Timer Call backs for traffic light phases
void vCallBackGreen(TimerHandle_t xTimer);
void vCallBackAmber(TimerHandle_t xTimer);
void vCallBackRed(TimerHandle_t xTimer);

// Helper function for accumulating traffic
void accumulate_traffic(uint32_t *road_pattern);

//Queues
xQueueHandle xTrafficFlowQueue_handle;
xQueueHandle xTrafficNewLightQueue_handle;
xQueueHandle xTrafficExpiredLightQueue_handle;
xQueueHandle xTrafficAdjustRateQueue_handle;
xQueueHandle xTrafficLightAdjustTimingQueue_handle;

//Software Timer Definitions
TimerHandle_t xTimer_Green;
TimerHandle_t xTimer_Amber;
TimerHandle_t xTimer_Red;

static void prvSetupHardware( void );

/*-----------------------------------------------------------*/
TaskHandle_t controlTaskHandle = NULL;

int main(void)
{
   prvSetupHardware();

   ADC_init();     //Initialize ADC
   GPIOC_init();   //Initialize Port C
   Queue_init();   //Initialize Queues and Timers

   srand(time(NULL));	//completely randomizes the rand() function seed ensuring a random sequence whenever ran

   xTaskCreate(vPotTask,"Pot",128,NULL,1,NULL);

   xTaskCreate(vTrafficLight,"TrafficLight",128,NULL,1,NULL);
   xTaskCreate(vTrafficCreate,"TrafficCreate",128,NULL,1,NULL);
   xTaskCreate(vTrafficDisplay,"TrafficDisplay",128,NULL,1,NULL);

   vTaskStartScheduler();  //Allows tasks to be run

   return 0;
}

/*-----------------------------------------------------------*/

void GPIOC_init(void){

   /* Enable clock for GPIOC peripheral*/
   RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);

   GPIO_InitTypeDef GPIO_InitStruct;

   GPIO_InitStruct.GPIO_Pin =    red_led | amber_led | green_led | DATA_PIN | CLOCK_PIN | RESET_PIN;
   GPIO_InitStruct.GPIO_Mode =   GPIO_Mode_OUT;
   GPIO_InitStruct.GPIO_Speed =  GPIO_Speed_50MHz;
   GPIO_InitStruct.GPIO_OType =  GPIO_OType_PP;
   GPIO_InitStruct.GPIO_PuPd =   GPIO_PuPd_NOPULL;
   GPIO_Init(GPIOC, &GPIO_InitStruct);

   //Pot Pin Initialization
   GPIO_InitStruct.GPIO_Pin =    POT_PIN;
   GPIO_InitStruct.GPIO_Mode =   GPIO_Mode_AN;
   GPIO_InitStruct.GPIO_PuPd =   GPIO_PuPd_NOPULL;
   GPIO_Init(GPIOC, &GPIO_InitStruct);

   GPIO_ResetBits(GPIOC, RESET_PIN);
   for(int d = 0; d<10000; d++);

   //Enable Shift Register
   GPIO_SetBits(GPIOC, RESET_PIN);

   //Set Data Line HIgh
   GPIO_SetBits(GPIOC, DATA_PIN);

   //Initialization of Traffic LEDs (ensuring they are all plugged in correctly)
   for(int i = 0; i < 24; i++){

           GPIO_SetBits(GPIOC, CLOCK_PIN);

           for(int j = 0; j < 1000000; j++);

           GPIO_ResetBits(GPIOC, CLOCK_PIN);

           for(int j = 0; j < 1000000; j++);
       }

   GPIO_ResetBits(GPIOC, RESET_PIN);
   for(int d = 0; d<10000; d++);
}

void ADC_init(void)
{
  // Enable clock for ADC1
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);

  // Initialize ADC Structure
  ADC_InitTypeDef ADC_InitStruct;

  ADC_InitStruct.ADC_ContinuousConvMode =      DISABLE;
  ADC_InitStruct.ADC_DataAlign          =      ADC_DataAlign_Right;
  ADC_InitStruct.ADC_Resolution         =      ADC_Resolution_12b;
  ADC_InitStruct.ADC_ScanConvMode       =      DISABLE;
  ADC_InitStruct.ADC_ExternalTrigConv   =      DISABLE;
  ADC_InitStruct.ADC_ExternalTrigConvEdge =    DISABLE;

  ADC_Init(ADC1, &ADC_InitStruct);
  ADC_Cmd(ADC1, ENABLE);

  // Configure Channel 13 (PC3) with a sample time
  ADC_RegularChannelConfig(ADC1, ADC_Channel_13, 1, ADC_SampleTime_144Cycles);
}

void Queue_init(void){
   //Queues
   xTrafficFlowQueue_handle =              xQueueCreate( 5, sizeof(uint32_t) );    //19-bit car pattern
   xTrafficNewLightQueue_handle =          xQueueCreate( 5, sizeof(uint8_t) );     //Notifies of a new light state
   xTrafficExpiredLightQueue_handle =      xQueueCreate( 5, sizeof(uint8_t) );     //Notifies that the current phase timer has expired
   xTrafficAdjustRateQueue_handle =        xQueueCreate( 5, sizeof(uint16_t) );    //pot value to control car gen frequency  [correction]
   xTrafficLightAdjustTimingQueue_handle = xQueueCreate( 5, sizeof(uint16_t) );    //potentiometer value to control light phase duration [correction]

   vQueueAddToRegistry(xTrafficFlowQueue_handle, "TrafficFlowQueue");
   vQueueAddToRegistry(xTrafficNewLightQueue_handle, "TrafficNewLightQueue");
   vQueueAddToRegistry(xTrafficExpiredLightQueue_handle, "TrafficExpiredLightQueue");
   vQueueAddToRegistry(xTrafficAdjustRateQueue_handle, "TrafficAdjustRateQueue");
   vQueueAddToRegistry(xTrafficLightAdjustTimingQueue_handle, "TrafficLightAdjustTimingQueue");

   // Software Timer Definitions
   xTimer_Green  = xTimerCreate("TMR1", pdMS_TO_TICKS(10000), pdFALSE, 0, vCallBackGreen);
   xTimer_Amber  = xTimerCreate("TMR2", pdMS_TO_TICKS(2000),  pdFALSE, 0, vCallBackAmber);
   xTimer_Red    = xTimerCreate("TMR3", pdMS_TO_TICKS(7000),  pdFALSE, 0, vCallBackRed);
}

void vPotTask(void *pvParameters){
   uint16_t potValue;
   uint16_t flowRate;
   uint16_t dummy;

   const TickType_t xDelay = pdMS_TO_TICKS(1000); // 20 Hz, changed to 1 second --> Step 6 of the manual

   for (;;)
   {
       // Start conversion
       ADC_SoftwareStartConv(ADC1);

       // Wait for conversion complete
       while (!ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC));

       // Read ADC value
       potValue = ADC_GetConversionValue(ADC1);

       // Convert potValue to a 0-100 value that can be easily used
       flowRate = (potValue * 100) / 0xFFF;

       // Ensure there is space in the queue so we don't drop the freshest data!
       if(uxQueueSpacesAvailable(xTrafficAdjustRateQueue_handle) == 0) {
    	   xQueueReceive(xTrafficAdjustRateQueue_handle, &dummy, 0);
       }
       if(uxQueueSpacesAvailable(xTrafficLightAdjustTimingQueue_handle) == 0) {
    	   xQueueReceive(xTrafficLightAdjustTimingQueue_handle, &dummy, 0);
       }

       xQueueSend(xTrafficAdjustRateQueue_handle, &flowRate, 0);   // No blocking
       xQueueSend(xTrafficLightAdjustTimingQueue_handle, &flowRate, 0);   // No blocking

       vTaskDelay(xDelay);
   }
}

void vTrafficLight (void *pvParameters){
   uint8_t current_light = green;
   uint8_t expired_light;
   uint16_t flowRate = 50;

   // Initial state: start with Green light
   xTimerStart(xTimer_Green, 0);
   xQueueSend(xTrafficNewLightQueue_handle, &current_light, 0);

   for (;;) {
       // Update physical LEDs
       GPIO_ResetBits(GPIOC, red_led | amber_led | green_led);
       if (current_light == red) GPIO_SetBits(GPIOC, red_led);
       else if (current_light == amber) GPIO_SetBits(GPIOC, amber_led);
       else if (current_light == green) GPIO_SetBits(GPIOC, green_led);

       // Wait for a timer to expire
       if (xQueueReceive(xTrafficExpiredLightQueue_handle, &expired_light, portMAX_DELAY)) {

       // Peek at latest potentiometer value to adjust timings
       // Get latest flow rate and empty the queue to prevent it from filling up
       uint16_t temp_flow_light;

       while (xQueueReceive(xTrafficLightAdjustTimingQueue_handle, &temp_flow_light, 0) == pdPASS){
           flowRate = temp_flow_light; // Keeps the most recent value
       }
           if (expired_light == green) {
               current_light = amber;
               xTimerChangePeriod(xTimer_Amber, pdMS_TO_TICKS(2000), 0); // Amber is constant 2s
           }
           else if (expired_light == amber) {
               current_light = red;
               // Red time inversely proportional to flow
               uint32_t red_time = 10000 - (50 * flowRate);	//sets time
               xTimerChangePeriod(xTimer_Red, pdMS_TO_TICKS(red_time), 0);	//runs the set time
           }
           else if (expired_light == red) {
               current_light = green;
               // Green time proportional to flow
               uint32_t green_time = 5000 + (50 * flowRate);	//sets time
               xTimerChangePeriod(xTimer_Green, pdMS_TO_TICKS(green_time), 0);	//runs the set time
           }

           // Notify Create_Traffic task of the new light state
           xQueueSend(xTrafficNewLightQueue_handle, &current_light, 0);
       }
   }
}
void vTrafficCreate (void *pvParameters){
   uint32_t road_pattern = 0; // 19-bit road representation
   uint8_t current_light = green;
   uint16_t flowRate = 50;

   for (;;) {
       // Check if light state changed (non-blocking)
       xQueueReceive(xTrafficNewLightQueue_handle, &current_light, 0);

       // Get latest flow rate (probability of new car)
       uint16_t temp_flow_create;

       while (xQueueReceive(xTrafficAdjustRateQueue_handle, &temp_flow_create, 0) == pdPASS) {
           flowRate = temp_flow_create; // Keeps the most recent value
       }

       if (current_light == green) {
           // Move cars forward
           road_pattern <<= 1;
       } else {
           // Amber or Red: Pile up cars before the intersection
    	   accumulate_traffic(&road_pattern);
       }

       // Randomly add a new car based on traffic flow rate
       // rand() % 100 generates 0-99. flowRate is 0-100.
       // Ensure that at 0 some traffic will still flow (+ 10)
       if ((rand() % 100) < ((flowRate) + 10)) {
           road_pattern |= 1; // Add car to start of the road (LSB)
       }

       // Keep it strictly 19 bits
       road_pattern &= 0x7FFFF;

       // Send to display task
       xQueueSend(xTrafficFlowQueue_handle, &road_pattern, 0);

       vTaskDelay(pdMS_TO_TICKS(500)); // Determines car speed (update rate) (500 2/s)
   }
}

// Helper to pile up cars at red/amber light
void accumulate_traffic(uint32_t *road) {
    uint32_t temp_road = *road;

    // Stop line is between LED 10 and LED 11 (bits 10 and 11)
    uint32_t after_intersection = temp_road & 0x7FF800; // Top 12 bits (past the light)
    uint32_t before_intersection = temp_road & 0x007FF; // Bottom 11 bits (before light)

    // Cars after the light keep moving forward
    after_intersection <<= 1;

    // Cars before the light pile up (shift only if the spot ahead is empty)
    uint32_t new_before = 0;

    // Loop through the bottom 11 bits (Index 10 down to 0)
    for(int i = 10; i >= 0; i--) {
        if (before_intersection & (1 << i)) {
            if (i < 10 && ((new_before & (1 << (i+1))) == 0)) {
                new_before |= (1 << (i+1)); // Move forward into empty space
            } else {
                new_before |= (1 << i); // Stop and pile up at the line
            }
        }
    }

    // Recombine the road
    *road = (after_intersection & 0x07F800) | (new_before & 0x007FF);
}


void vTrafficDisplay (void *pvParameters){
   uint32_t road_pattern = 0;

   for (;;) {
       if (xQueueReceive(xTrafficFlowQueue_handle, &road_pattern, portMAX_DELAY)) {

           // Clear shift register
           GPIO_ResetBits(GPIOC, RESET_PIN);
           for(volatile int d=0; d<10; d++);
           GPIO_SetBits(GPIOC, RESET_PIN);

           // Shift out the 19 bits (MSB to LSB)
           for (int i = 18; i >= 0; i--) {
               if (road_pattern & (1 << i)) {
                   GPIO_SetBits(GPIOC, DATA_PIN);
               } else {
                   GPIO_ResetBits(GPIOC, DATA_PIN);
               }

               // Clock pulse
               GPIO_SetBits(GPIOC, CLOCK_PIN);
               for(volatile int d=0; d<10; d++); // Short delay
               GPIO_ResetBits(GPIOC, CLOCK_PIN);
           }
       }
   }
}
/*-----------------------------------------------------------*/
/* Timer Callbacks (Send expired light state to queue) */

void vCallBackGreen(TimerHandle_t xTimer) {
   uint8_t expired = green;
   xQueueSend(xTrafficExpiredLightQueue_handle, &expired, 0);
}

void vCallBackAmber(TimerHandle_t xTimer) {
   uint8_t expired = amber;
   xQueueSend(xTrafficExpiredLightQueue_handle, &expired, 0);
}

void vCallBackRed(TimerHandle_t xTimer) {
   uint8_t expired = red;
   xQueueSend(xTrafficExpiredLightQueue_handle, &expired, 0);
}

/*-----------------------------------------------------------*/

void vApplicationMallocFailedHook( void )
{
   /* The malloc failed hook is enabled by setting
   configUSE_MALLOC_FAILED_HOOK to 1 in FreeRTOSConfig.h.

   Called if a call to pvPortMalloc() fails because there is insufficient
   free memory available in the FreeRTOS heap.  pvPortMalloc() is called
   internally by FreeRTOS API functions that create tasks, queues, software
   timers, and semaphores.  The size of the FreeRTOS heap is set by the
   configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */
   for( ;; );
}
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook( xTaskHandle pxTask, signed char *pcTaskName )
{
   ( void ) pcTaskName;
   ( void ) pxTask;

   /* Run time stack overflow checking is performed if
   configconfigCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
   function is called if a stack overflow is detected.  pxCurrentTCB can be
   inspected in the debugger if the task name passed into this function is
   corrupt. */
   for( ;; );
}
/*-----------------------------------------------------------*/

void vApplicationIdleHook( void )
{
volatile size_t xFreeStackSpace;

   /* The idle task hook is enabled by setting configUSE_IDLE_HOOK to 1 in
   FreeRTOSConfig.h.

   This function is called on each cycle of the idle task.  In this case it
   does nothing useful, other than report the amount of FreeRTOS heap that
   remains unallocated. */
   xFreeStackSpace = xPortGetFreeHeapSize();

   if( xFreeStackSpace > 100 )
   {
       /* By now, the kernel has allocated everything it is going to, so
       if there is a lot of heap remaining unallocated then
       the value of configTOTAL_HEAP_SIZE in FreeRTOSConfig.h can be
       reduced accordingly. */
   }
}
/*-----------------------------------------------------------*/

static void prvSetupHardware( void )
{
   /* Ensure all priority bits are assigned as preemption priority bits.
   http://www.freertos.org/RTOS-Cortex-M3-M4.html */
   NVIC_SetPriorityGrouping( 0 );

   /* TODO: Setup the clocks, etc. here, if they were not configured before
   main() was called. */
}