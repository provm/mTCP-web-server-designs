#ifndef __APP_QUEUE_
#define __APP_QUEUE_

#include <stdint.h>

/*---------------------------------------------------------------------------*/
typedef struct app_queue* app_queue_t;
/*---------------------------------------------------------------------------*/

typedef struct app_info{
	int s_id;
	int j_id;
} app_info;

app_queue_t
CreateAppQueue(int size);
/*---------------------------------------------------------------------------*/
void 
DestroyAppQueue(app_queue_t sq);
/*---------------------------------------------------------------------------*/
int 
AppEnqueue(app_queue_t sq, struct app_info *app);
/*---------------------------------------------------------------------------*/
struct app_info *
AppDequeue(app_queue_t sq);
/*---------------------------------------------------------------------------*/
int 
AppQueueIsEmpty(app_queue_t sq);
/*---------------------------------------------------------------------------*/

#endif /* __APP_QUEUE_ */
