/*
 *  Squeeze2upnp - LMS to uPNP gateway
 *
 *  Squeezelite : (c) Adrian Smith 2012-2014, triode1@btinternet.com
 *  Additions & gateway : (c) Philippe 2014, philippe_44@outlook.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef __UTIL_H
#define __UTIL_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "squeeze2cast.h"
#include "pthread.h"
#include "ixml.h" /* for IXML_Document, IXML_Element */
#include "upnp.h" /* for Upnp_EventType */
#include "util_common.h"
#include "jansson.h"

typedef struct {
	pthread_mutex_t	*mutex;
	void (*cleanup)(void*);
	struct sQueue_e {
		struct sQueue_e *next;
		void 			*item;
	} list;
} tQueue;

typedef struct list_s {
	struct list_s *next;
} list_t;

list_t*		push_item(list_t *item, list_t **list);
list_t*		add_tail_item(list_t *item, list_t **list);
list_t*		add_ordered_item(list_t *item, list_t **list, int (*compare)(void *a, void *b));
list_t*		pop_item(list_t **list);
list_t*   	remove_item(list_t *item, list_t **list);
void 		clear_list(list_t **list, void (*free_func)(void *));

void 		QueueInit(tQueue *queue, bool mutex, void (*f)(void*));
void 		QueueInsert(tQueue *queue, void *item);
void 		*QueueExtract(tQueue *queue);
void 		QueueFlush(tQueue *queue);

char 		*uPNPEvent2String(Upnp_EventType S);
void 		MakeMacUnique(struct sMR *Device);
unsigned 	Time2Int(char *Time);
int			pthread_cond_reltimedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, u32_t msWait);

char 	   	*XMLGetChangeItem(IXML_Document *doc, char *Tag, char *SearchAttr, char *SearchVal, char *RetAttr);
IXML_Node  	*XMLAddNode(IXML_Document *doc, IXML_Node *parent, char *name, char *fmt, ...);
IXML_Node 	*XMLUpdateNode(IXML_Document *doc, IXML_Node *parent, bool refresh, char *name, char *fmt, ...);
int 	   	XMLAddAttribute(IXML_Document *doc, IXML_Node *parent, char *name, char *fmt, ...);
char 	   	*XMLGetFirstDocumentItem(IXML_Document *doc, const char *item);
int 	   	XMLFindAndParseService(IXML_Document *DescDoc, const char *location,
							const char *serviceType, char **serviceId,
							char **eventURL, char **controlURL);

int 		GetMediaItem_I(json_t *root, int n, char *item);
double 		GetMediaItem_F(json_t *root, int n, char *item);
const char 	*GetMediaItem_S(json_t *root, int n, char *item);
const char  *GetAppIdItem(json_t *root, char* appId, char *item);
bool 		GetMediaVolume(json_t *root, int n, double *volume, bool *muted);

void	  	SaveConfig(char *name, void *ref, bool full);
void	   	*LoadConfig(char *name, tMRConfig *Conf, sq_dev_param_t *sq_conf);
void	  	*FindMRConfig(void *ref, char *UDN);
void 	  	*LoadMRConfig(void *ref, char *UDN, tMRConfig *Conf, sq_dev_param_t *sq_conf);

#if WIN
void  		winsock_init(void);
void		winsock_close(void);
#endif

#endif
