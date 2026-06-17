#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>


#define SCHEDULER_DEFAULT 0
#define SCHEDULER_RMS     1
#define SCHEDULER_EDF     2
#define SCHEDULER_LOTTERY 3

extern volatile uint8_t ucCurrentScheduler;

// Root node of the EDF Treap
PRIVILEGED_DATA static tskTCB *pxEDFTreapRoot = NULL;

#define MAX_CUSTOM_TASKS 16
PRIVILEGED_DATA static TaskHandle_t xRegisteredTasks[MAX_CUSTOM_TASKS];
PRIVILEGED_DATA static uint8_t ucRegisteredTaskCount = 0;

extern void vSystemPrint(const char *pcString);

void vSetTaskCustomParams( TaskHandle_t xTask, TickType_t xPeriod, uint32_t ulTickets )
{
    tskTCB *pxTCB = ( tskTCB * ) xTask;
    if( pxTCB != NULL )
    {
        pxTCB->xPeriod = xPeriod;
        pxTCB->ulTickets = ulTickets;
        pxTCB->xAbsoluteDeadline = 0;

        uint8_t isDuplicate = 0;
        for( uint8_t i = 0; i < ucRegisteredTaskCount; i++ )
        {
            if( xRegisteredTasks[i] == xTask ) isDuplicate = 1;
        }

        if( !isDuplicate && ucRegisteredTaskCount < MAX_CUSTOM_TASKS )
        {
            xRegisteredTasks[ucRegisteredTaskCount++] = xTask;
        }
    }
}

void vTaskUpdateDeadline( TaskHandle_t xTask, TickType_t xNextDeadline )
{
    tskTCB *pxTCB = ( tskTCB * ) xTask;
    if( pxTCB != NULL )
    {
        pxTCB->xAbsoluteDeadline = xNextDeadline;
    }
}

void vRecalculateRMSPriorities( void )
{
    if( ucRegisteredTaskCount == 0 ) return;

    // Create a sorted array to avoid modifying the registration order
    TaskHandle_t xSortedTasks[MAX_CUSTOM_TASKS];
    for( uint8_t i = 0; i < ucRegisteredTaskCount; i++ )
    {
        xSortedTasks[i] = xRegisteredTasks[i];
    }

    for( uint8_t i = 0; i < ucRegisteredTaskCount - 1; i++ )
    {
        for( uint8_t j = 0; j < ucRegisteredTaskCount - i - 1; j++ )
        {
            tskTCB *pxTCB1 = (tskTCB *)xSortedTasks[j];
            tskTCB *pxTCB2 = (tskTCB *)xSortedTasks[j+1];

            if( pxTCB1->xPeriod > pxTCB2->xPeriod )
            {
                TaskHandle_t temp = xSortedTasks[j];
                xSortedTasks[j] = xSortedTasks[j+1];
                xSortedTasks[j+1] = temp;
            }
        }
    }

    // Assign priorities (highest is configMAX_PRIORITIES - 1)
    // RMS rule: same period tasks share the same priority
    UBaseType_t uxCurrentPriority = configMAX_PRIORITIES - 1;
    TickType_t xLastPeriod = 0;

    for( uint8_t i = 0; i < ucRegisteredTaskCount; i++ )
    {
        tskTCB *pxTCB = (tskTCB *)xSortedTasks[i];

        // If period is longer than the last one and priority hasn't reached the bottom (1), decrease it
        // Reserve priority 0 for the FreeRTOS native Idle Task
        if( i > 0 && pxTCB->xPeriod > xLastPeriod && uxCurrentPriority > 1 )
        {
            uxCurrentPriority--;
        }

        // Call FreeRTOS native API safely to change priority and move to Ready List
        vTaskPrioritySet( xSortedTasks[i], uxCurrentPriority );

        xLastPeriod = pxTCB->xPeriod;
    }
}


static uint32_t prvXorshift32( void )
{
    static uint32_t ulState = 123456789;
    ulState ^= ulState << 13;
    ulState ^= ulState >> 17;
    ulState ^= ulState << 5;
    return ulState;
}

static tskTCB* prvSelectHighestPriorityReadyTask( void )
{
    List_t *pxList;
    UBaseType_t uxPriority;

    for( uxPriority = configMAX_PRIORITIES; uxPriority > 0; uxPriority-- )
    {
        pxList = &( pxReadyTasksLists[ uxPriority - 1 ] );
        if( listCURRENT_LIST_LENGTH( pxList ) > 0 )
        {
            return ( tskTCB * ) listGET_OWNER_OF_HEAD_ENTRY( pxList );
        }
    }

    return pxCurrentTCB;
}

// Lottery Scheduling (彩票排程器)
tskTCB* pxSelectTaskByLottery( void )
{
    uint32_t ulTotalTickets = 0;
    uint32_t ulWinningTicket = 0;
    uint32_t ulTicketCount = 0;
    tskTCB *pxTask = NULL;
    ListItem_t *pxListItem;
    List_t *pxList;
    UBaseType_t uxPriority;

    // Stage 1: Calculate the total number of tickets for all ready tasks
    for( uxPriority = 0; uxPriority < configMAX_PRIORITIES; uxPriority++ )
    {
        pxList = &( pxReadyTasksLists[ uxPriority ] );
        if( listCURRENT_LIST_LENGTH( pxList ) > 0 )
        {
            pxListItem = listGET_HEAD_ENTRY( pxList );
            while( pxListItem != ( ListItem_t * ) listGET_END_MARKER( pxList ) )
            {
                pxTask = ( tskTCB * ) listGET_LIST_ITEM_OWNER( pxListItem );
                ulTotalTickets += (pxTask->ulTickets > 0) ? pxTask->ulTickets : 1;
                pxListItem = listGET_NEXT( pxListItem );
            }
        }
    }

    if( ulTotalTickets == 0 ) return pxCurrentTCB; // Guard against zero tickets

    // Stage 2: Draw the winning ticket number
    ulWinningTicket = prvXorshift32() % ulTotalTickets;

    // Stage 3: Find the winning task
    for( uxPriority = 0; uxPriority < configMAX_PRIORITIES; uxPriority++ )
    {
        pxList = &( pxReadyTasksLists[ uxPriority ] );
        if( listCURRENT_LIST_LENGTH( pxList ) > 0 )
        {
            pxListItem = listGET_HEAD_ENTRY( pxList );
            while( pxListItem != ( ListItem_t * ) listGET_END_MARKER( pxList ) )
            {
                pxTask = ( tskTCB * ) listGET_LIST_ITEM_OWNER( pxListItem );
                ulTicketCount += (pxTask->ulTickets > 0) ? pxTask->ulTickets : 1;

                if( ulTicketCount > ulWinningTicket ) return pxTask;
                pxListItem = listGET_NEXT( pxListItem );
            }
        }
    }
    return pxCurrentTCB;
}

// Earliest Deadline First (EDF) based on Treap implementation
static void prvTreapRightRotate( tskTCB **ppxRoot, tskTCB *y )
{
    tskTCB *x = y->pxTreapLeft;
    y->pxTreapLeft = x->pxTreapRight;
    if( x->pxTreapRight != NULL ) x->pxTreapRight->pxTreapParent = y;

    x->pxTreapParent = y->pxTreapParent;
    if( y->pxTreapParent == NULL ) *ppxRoot = x;
    else if( y == y->pxTreapParent->pxTreapRight ) y->pxTreapParent->pxTreapRight = x;
    else y->pxTreapParent->pxTreapLeft = x;

    x->pxTreapRight = y;
    y->pxTreapParent = x;
}

static void prvTreapLeftRotate( tskTCB **ppxRoot, tskTCB *x )
{
    tskTCB *y = x->pxTreapRight;
    x->pxTreapRight = y->pxTreapLeft;
    if( y->pxTreapLeft != NULL ) y->pxTreapLeft->pxTreapParent = x;

    y->pxTreapParent = x->pxTreapParent;
    if( x->pxTreapParent == NULL ) *ppxRoot = y;
    else if( x == x->pxTreapParent->pxTreapLeft ) x->pxTreapParent->pxTreapLeft = y;
    else x->pxTreapParent->pxTreapRight = y;

    y->pxTreapLeft = x;
    x->pxTreapParent = y;
}


void vTreapInsert( tskTCB *pxNewTask )
{
    pxNewTask->pxTreapLeft = NULL;
    pxNewTask->pxTreapRight = NULL;
    pxNewTask->pxTreapParent = NULL;
    pxNewTask->ulTreapPriority = prvXorshift32();

    tskTCB *pxCurrent = pxEDFTreapRoot;
    tskTCB *pxParent = NULL;

    while( pxCurrent != NULL )
    {
        pxParent = pxCurrent;
        if( pxNewTask->xAbsoluteDeadline < pxCurrent->xAbsoluteDeadline )
            pxCurrent = pxCurrent->pxTreapLeft;
        else
            pxCurrent = pxCurrent->pxTreapRight;
    }

    pxNewTask->pxTreapParent = pxParent;
    if( pxParent == NULL ) {
        pxEDFTreapRoot = pxNewTask;
    } else if( pxNewTask->xAbsoluteDeadline < pxParent->xAbsoluteDeadline ) {
        pxParent->pxTreapLeft = pxNewTask;
    } else {
        pxParent->pxTreapRight = pxNewTask;
    }

    // Maintain Max-Heap property: rotate up until parent's priority is larger
    while( pxNewTask->pxTreapParent != NULL &&
           pxNewTask->ulTreapPriority > pxNewTask->pxTreapParent->ulTreapPriority )
    {
        if( pxNewTask == pxNewTask->pxTreapParent->pxTreapLeft )
            prvTreapRightRotate( &pxEDFTreapRoot, pxNewTask->pxTreapParent );
        else
            prvTreapLeftRotate( &pxEDFTreapRoot, pxNewTask->pxTreapParent );
    }
}

// Rebuild EDF Treap to resolve Blocked tasks
void vRebuildEDFTreap( void )
{
    pxEDFTreapRoot = NULL;

    for( uint8_t i = 0; i < ucRegisteredTaskCount; i++ )
    {
        tskTCB *pxTCB = ( tskTCB * ) xRegisteredTasks[i];

        pxTCB->pxTreapLeft = NULL;
        pxTCB->pxTreapRight = NULL;
        pxTCB->pxTreapParent = NULL;

        if( listIS_CONTAINED_WITHIN( &( pxReadyTasksLists[ pxTCB->uxPriority ] ),
                                     &( pxTCB->xStateListItem ) ) != pdFALSE )
        {
            vTreapInsert( pxTCB );
        }
    }
}

void vTreapRemove( tskTCB *pxTaskToRemove )
{
    if( pxTaskToRemove == NULL ) return;

    while( pxTaskToRemove->pxTreapLeft != NULL || pxTaskToRemove->pxTreapRight != NULL )
    {
        if( pxTaskToRemove->pxTreapLeft == NULL ) {
            prvTreapLeftRotate( &pxEDFTreapRoot, pxTaskToRemove );
        } else if( pxTaskToRemove->pxTreapRight == NULL ) {
            prvTreapRightRotate( &pxEDFTreapRoot, pxTaskToRemove );
        } else if( pxTaskToRemove->pxTreapLeft->ulTreapPriority > pxTaskToRemove->pxTreapRight->ulTreapPriority ) {
            prvTreapRightRotate( &pxEDFTreapRoot, pxTaskToRemove );
        } else {
            prvTreapLeftRotate( &pxEDFTreapRoot, pxTaskToRemove );
        }
    }

    if( pxTaskToRemove->pxTreapParent == NULL ) {
        pxEDFTreapRoot = NULL; /* 樹被清空了 */
    } else if( pxTaskToRemove->pxTreapParent->pxTreapLeft == pxTaskToRemove ) {
        pxTaskToRemove->pxTreapParent->pxTreapLeft = NULL;
    } else {
        pxTaskToRemove->pxTreapParent->pxTreapRight = NULL;
    }

    pxTaskToRemove->pxTreapParent = NULL;
}
tskTCB* pxTreapExtractMin( void )
{
    if( pxEDFTreapRoot == NULL ) return prvSelectHighestPriorityReadyTask();

    tskTCB *pxMin = pxEDFTreapRoot;
    while( pxMin->pxTreapLeft != NULL )
    {
        pxMin = pxMin->pxTreapLeft;
    }

    vTreapRemove( pxMin );

    return pxMin;
}
void vTreapSafeRemove( void *pvOwner )
{
    if( ucCurrentScheduler != SCHEDULER_EDF || pvOwner == NULL )
    {
        return;
    }

    tskTCB *pxTask = ( tskTCB * ) pvOwner;

    if( pxTask != pxEDFTreapRoot &&
        pxTask->pxTreapParent == NULL &&
        pxTask->pxTreapLeft == NULL &&
        pxTask->pxTreapRight == NULL )
    {
        return;
    }

    vTreapRemove( pxTask );
}


static void prvBufferList(List_t *pxList, const char *pcState, char *pcBuffer)
{
    ListItem_t *pxListItem;
    tskTCB *pxTCB;
    char cTemp[128];

    if( listCURRENT_LIST_LENGTH( pxList ) > 0 )
    {
        pxListItem = listGET_HEAD_ENTRY( pxList );

        while( pxListItem != ( ListItem_t * ) listGET_END_MARKER( pxList ) )
        {
            pxTCB = ( tskTCB * ) listGET_LIST_ITEM_OWNER( pxListItem );

            sprintf(cTemp, "| %-10s | %-4lu | %-9s | %-6lu | %-8lu | %-7lu |\r\n",
                    pxTCB->pcTaskName,
                    pxTCB->uxPriority,
                    pcState,
                    pxTCB->xPeriod,
                    pxTCB->xAbsoluteDeadline,
                    pxTCB->ulTickets);

            strcat(pcBuffer, cTemp);

            pxListItem = listGET_NEXT( pxListItem );
        }
    }
}


void Taskmonitor(void)
{
	static char cBigBuffer[2048];
	cBigBuffer[0] = '\0';

	char cHeader[] = "\r\n=================================================================\r\n"
					 "| Task Name  | PR   | State     | Period | Deadline | Tickets |\r\n"
					 "=================================================================\r\n";
	strcat(cBigBuffer, cHeader);

	vTaskSuspendAll();
	{
		for( UBaseType_t ux = 0; ux < configMAX_PRIORITIES; ux++ )
		{
			prvBufferList( &( pxReadyTasksLists[ ux ] ), "Ready", cBigBuffer );
		}
		prvBufferList( pxDelayedTaskList, "Blocked", cBigBuffer );
	}
	xTaskResumeAll();

	vSystemPrint(cBigBuffer);
}
