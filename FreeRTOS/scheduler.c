#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>


/* 確保這些狀態與 tasks.c 頂部的定義一致 */
#define SCHEDULER_DEFAULT 0
#define SCHEDULER_RMS     1
#define SCHEDULER_EDF     2
#define SCHEDULER_LOTTERY 3

extern volatile uint8_t ucCurrentScheduler;

/* EDF 專用全域變數：Treap 的根節點 */
PRIVILEGED_DATA static tskTCB *pxEDFTreapRoot = NULL;

/* ========================================================================= */
/* 任務註冊表 (用於 RMS 遍歷與全域管理)                                      */
/* ========================================================================= */
#define MAX_CUSTOM_TASKS 16 /* 依您的系統需求調整 */
PRIVILEGED_DATA static TaskHandle_t xRegisteredTasks[MAX_CUSTOM_TASKS];
PRIVILEGED_DATA static uint8_t ucRegisteredTaskCount = 0;

extern void vSystemPrint(const char *pcString);

/* ========================================================================= */
/* 給 main.c 呼叫的外部 API (Bridge Functions)                               */
/* ========================================================================= */

/* 設定任務的擴展排程參數，並自動註冊 */
void vSetTaskCustomParams( TaskHandle_t xTask, TickType_t xPeriod, uint32_t ulTickets )
{
    tskTCB *pxTCB = ( tskTCB * ) xTask;
    if( pxTCB != NULL )
    {
        pxTCB->xPeriod = xPeriod;
        pxTCB->ulTickets = ulTickets;
        pxTCB->xAbsoluteDeadline = 0;

        /* 避免重複註冊 */
        uint8_t isDuplicate = 0;
        for( uint8_t i = 0; i < ucRegisteredTaskCount; i++ )
        {
            if( xRegisteredTasks[i] == xTask ) isDuplicate = 1;
        }

        /* 將任務加入註冊表 */
        if( !isDuplicate && ucRegisteredTaskCount < MAX_CUSTOM_TASKS )
        {
            xRegisteredTasks[ucRegisteredTaskCount++] = xTask;
        }
    }
}

/* 任務每次執行完畢準備進入 Delay 前，需呼叫此函式更新下一次的 Deadline */
void vTaskUpdateDeadline( TaskHandle_t xTask, TickType_t xNextDeadline )
{
    tskTCB *pxTCB = ( tskTCB * ) xTask;
    if( pxTCB != NULL )
    {
        pxTCB->xAbsoluteDeadline = xNextDeadline;
    }
}

/* ========================================================================= */
/* 核心 API：重新計算並指派 RMS 優先級                                       */
/* ========================================================================= */
void vRecalculateRMSPriorities( void )
{
    if( ucRegisteredTaskCount == 0 ) return;

    /* 1. 建立排序用陣列 (避免更動原本的註冊順序) */
    TaskHandle_t xSortedTasks[MAX_CUSTOM_TASKS];
    for( uint8_t i = 0; i < ucRegisteredTaskCount; i++ )
    {
        xSortedTasks[i] = xRegisteredTasks[i];
    }

    /* 2. 氣泡排序：依照 xPeriod 由小到大排序 (週期越短，優先級應越高) */
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

    /* 3. 依序指派優先級 (最高為 configMAX_PRIORITIES - 1) */
    /* RMS 規則：週期相同者，享有相同的優先級 */
    UBaseType_t uxCurrentPriority = configMAX_PRIORITIES - 1;
    TickType_t xLastPeriod = 0;

    for( uint8_t i = 0; i < ucRegisteredTaskCount; i++ )
    {
        tskTCB *pxTCB = (tskTCB *)xSortedTasks[i];

        /* 如果週期比上一個長，且優先級還沒降到底 (1)，就將優先級降一級 */
        /* 保留優先級 0 給 FreeRTOS 原生的 Idle Task */
        if( i > 0 && pxTCB->xPeriod > xLastPeriod && uxCurrentPriority > 1 )
        {
            uxCurrentPriority--;
        }

        /* 呼叫 FreeRTOS 原生 API 安全地更改優先級與移動 Ready List */
        vTaskPrioritySet( xSortedTasks[i], uxCurrentPriority );

        xLastPeriod = pxTCB->xPeriod;
    }
}

/* ========================================================================= */
/* 1. 共用核心：極速虛擬亂數產生器 (Xorshift32)                              */
/* ========================================================================= */
static uint32_t prvXorshift32( void )
{
    static uint32_t ulState = 123456789; /* 初始種子 */
    ulState ^= ulState << 13;
    ulState ^= ulState >> 17;
    ulState ^= ulState << 5;
    return ulState;
}

/* ========================================================================= */
/* 2. Lottery Scheduling (彩票排程器)                                        */
/* ========================================================================= */
tskTCB* pxSelectTaskByLottery( void )
{
    uint32_t ulTotalTickets = 0;
    uint32_t ulWinningTicket = 0;
    uint32_t ulTicketCount = 0;
    tskTCB *pxTask = NULL;
    ListItem_t *pxListItem;
    List_t *pxList;
    UBaseType_t uxPriority;

    /* 階段一：計算目前所有處於 Ready 狀態任務的彩票總數 */
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

    if( ulTotalTickets == 0 ) return pxCurrentTCB; /* 防禦機制 */

    /* 階段二：抽出中獎號碼 */
    ulWinningTicket = prvXorshift32() % ulTotalTickets;

    /* 階段三：尋找中獎任務 */
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

/* ========================================================================= */
/* 3. Earliest Deadline First (基於 Treap 實作)                              */
/* ========================================================================= */

/* --- Treap 輔助函式：右旋轉 (Right Rotate) --- */
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

/* --- Treap 輔助函式：左旋轉 (Left Rotate) --- */
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

/* --- EDF 核心：插入任務 --- */
void vTreapInsert( tskTCB *pxNewTask )
{
    /* 初始化 Treap 節點屬性 */
    pxNewTask->pxTreapLeft = NULL;
    pxNewTask->pxTreapRight = NULL;
    pxNewTask->pxTreapParent = NULL;
    pxNewTask->ulTreapPriority = prvXorshift32(); /* 賦予隨機堆疊優先級 */

    /* BST 標準插入邏輯 (依照 Deadline) */
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

    /* 維護 Max-Heap 屬性：向上旋轉直到父節點的 Priority 較大 */
    while( pxNewTask->pxTreapParent != NULL &&
           pxNewTask->ulTreapPriority > pxNewTask->pxTreapParent->ulTreapPriority )
    {
        if( pxNewTask == pxNewTask->pxTreapParent->pxTreapLeft )
            prvTreapRightRotate( &pxEDFTreapRoot, pxNewTask->pxTreapParent );
        else
            prvTreapLeftRotate( &pxEDFTreapRoot, pxNewTask->pxTreapParent );
    }
}

/* --- EDF 核心：刪除任意節點 (解決 Blocked 問題) --- */
void vTreapRemove( tskTCB *pxTaskToRemove )
{
    if( pxTaskToRemove == NULL ) return;

    /* 不斷向下旋轉，直到該節點變成葉節點 (Leaf) */
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

    /* 變成葉節點後，安全剪斷與父節點的連結 */
    if( pxTaskToRemove->pxTreapParent == NULL ) {
        pxEDFTreapRoot = NULL; /* 樹被清空了 */
    } else if( pxTaskToRemove->pxTreapParent->pxTreapLeft == pxTaskToRemove ) {
        pxTaskToRemove->pxTreapParent->pxTreapLeft = NULL;
    } else {
        pxTaskToRemove->pxTreapParent->pxTreapRight = NULL;
    }

    /* 清理乾淨指標防呆 */
    pxTaskToRemove->pxTreapParent = NULL;
}

/* --- EDF 核心：取出最早 Deadline 任務 (Extract-Min) --- */
tskTCB* pxTreapExtractMin( void )
{
    if( pxEDFTreapRoot == NULL ) return pxCurrentTCB;

    /* Deadline 最小的任務永遠在 BST 的最左下角 */
    tskTCB *pxMin = pxEDFTreapRoot;
    while( pxMin->pxTreapLeft != NULL )
    {
        pxMin = pxMin->pxTreapLeft;
    }

    /* 取出後將其從 Treap 中刪除 */
    vTreapRemove( pxMin );

    return pxMin;
}
/* --- EDF 核心：安全防呆移除 (供 list.c 呼叫) --- */
void vTreapSafeRemove( void *pvOwner )
{
    /* 如果當前不是 EDF 模式，或者傳入空指標，直接退出 */
    if( ucCurrentScheduler != SCHEDULER_EDF || pvOwner == NULL )
    {
        return;
    }

    tskTCB *pxTask = ( tskTCB * ) pvOwner;

    /* 防呆檢查：如果這個 TCB 不是 Root，且沒有任何父/子節點，
       代表它根本不在 Treap 裡面 (可能原本就在 Blocked 狀態)，直接跳過 */
    if( pxTask != pxEDFTreapRoot &&
        pxTask->pxTreapParent == NULL &&
        pxTask->pxTreapLeft == NULL &&
        pxTask->pxTreapRight == NULL )
    {
        return;
    }

    /* 確認在 Treap 內，執行真正的 O(log n) 旋轉拔除 */
    vTreapRemove( pxTask );
}

/* 宣告外部的 UART 控制代碼 (請依據您的 STM32 實際變數名稱修改，通常是 huart2) */

/* ========================================================================= */
/* 內部輔助函式：走訪指定的 List 並透過 UART 印出任務資訊                    */
/* ========================================================================= */
static void prvBufferList(List_t *pxList, const char *pcState, char *pcBuffer)
{
    ListItem_t *pxListItem;
    tskTCB *pxTCB;
    char cTemp[128];

    /* 確保 List 裡面有東西才進行走訪 */
    if( listCURRENT_LIST_LENGTH( pxList ) > 0 )
    {
        pxListItem = listGET_HEAD_ENTRY( pxList );

        while( pxListItem != ( ListItem_t * ) listGET_END_MARKER( pxList ) )
        {
            pxTCB = ( tskTCB * ) listGET_LIST_ITEM_OWNER( pxListItem );

            /* 將當前任務的資訊格式化到暫存字串 cTemp */
            sprintf(cTemp, "| %-10s | %-4lu | %-9s | %-6lu | %-8lu | %-7lu |\r\n",
                    pxTCB->pcTaskName,
                    pxTCB->uxPriority,
                    pcState,
                    pxTCB->xPeriod,
                    pxTCB->xAbsoluteDeadline,
                    pxTCB->ulTickets);

            /* 將 cTemp 接續到我們傳進來的大緩衝區 pcBuffer 後面 */
            strcat(pcBuffer, cTemp);

            pxListItem = listGET_NEXT( pxListItem );
        }
    }
}

/* ========================================================================= */
/* Taskmonitor 實作：供外部 main.c 呼叫以印出系統狀態                        */
/* ========================================================================= */
void Taskmonitor(void)
{
	static char cBigBuffer[2048];
	cBigBuffer[0] = '\0';

	char cHeader[] = "\r\n=================================================================\r\n"
					 "| Task Name  | PR   | State     | Period | Deadline | Tickets |\r\n"
					 "=================================================================\r\n";
	strcat(cBigBuffer, cHeader);

	/* 1. 進入安全區（極短時間內完成指標走訪，不進行任何 UART 傳送） */
	vTaskSuspendAll();
	{
		for( UBaseType_t ux = 0; ux < configMAX_PRIORITIES; ux++ )
		{
			// 修改 prvPrintList，讓它只是把字串 strcat 到 cBigBuffer，而不呼叫 vSystemPrint
			prvBufferList( &( pxReadyTasksLists[ ux ] ), "Ready", cBigBuffer );
		}
		prvBufferList( pxDelayedTaskList, "Blocked", cBigBuffer );
	}
	xTaskResumeAll(); /* 2. 立刻恢復排程 */

	/* 3. 在排程器正常運作下，安全地印出完整資料 */
	vSystemPrint(cBigBuffer);
}
