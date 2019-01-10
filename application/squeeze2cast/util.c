/*
 *  Squeeze2cast - LMS to Cast gateway
 *
 *  (c) Philippe 2016-2017, philippe_44@outlook.com
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

#include <stdarg.h>

#include "squeezedefs.h"
#include "util.h"
#include "util_common.h"
#include "log_util.h"
#include "upnptools.h"

/*----------------------------------------------------------------------------*/
/* globals */
/*----------------------------------------------------------------------------*/

extern log_level	slimproto_loglevel;
extern log_level	slimmain_loglevel;
extern log_level	stream_loglevel;
extern log_level	decode_loglevel;
extern log_level	output_loglevel;
extern log_level	main_loglevel;
extern log_level	util_loglevel;
extern log_level	cast_loglevel;

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
extern log_level 	util_loglevel;
static log_level 	*loglevel = &util_loglevel;

#if WIN
/*----------------------------------------------------------------------------*/
void winsock_init(void) {
	WSADATA wsaData;
	WORD wVersionRequested = MAKEWORD(2, 2);
	int WSerr = WSAStartup(wVersionRequested, &wsaData);
	if (WSerr != 0) {
		LOG_ERROR("Bad winsock version", NULL);
		exit(1);
	}
}


/*----------------------------------------------------------------------------*/
void winsock_close(void) {
	WSACleanup();
}
#endif


/*----------------------------------------------------------------------------*/
int pthread_cond_reltimedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, u32_t msWait)
{
	struct timespec ts;
	u32_t	nsec;
#if OSX
	struct timeval tv;
#endif

#if WIN
	struct _timeb SysTime;

	_ftime(&SysTime);
	ts.tv_sec = (long) SysTime.time;
	ts.tv_nsec = 1000000 * SysTime.millitm;
#elif LINUX || FREEBSD
	clock_gettime(CLOCK_REALTIME, &ts);
#elif OSX
	gettimeofday(&tv, NULL);
	ts.tv_sec = (long) tv.tv_sec;
	ts.tv_nsec = 1000L * tv.tv_usec;
#endif

	if (!msWait) return pthread_cond_wait(cond, mutex);

	nsec = ts.tv_nsec + (msWait % 1000) * 1000000;
	ts.tv_sec += msWait / 1000 + (nsec / 1000000000);
	ts.tv_nsec = nsec % 1000000000;

	return pthread_cond_timedwait(cond, mutex, &ts);
}

/*----------------------------------------------------------------------------*/
/* 																			  */
/* QUEUE management															  */
/* 																			  */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
void QueueInit(tQueue *queue, bool mutex, void (*cleanup)(void*))
{
	queue->cleanup = cleanup;
	queue->list.item = NULL;
	if (mutex) {
		queue->mutex = malloc(sizeof(pthread_mutex_t));
		pthread_mutex_init(queue->mutex, NULL);
	}
	else queue->mutex = NULL;
}

/*----------------------------------------------------------------------------*/
void QueueInsert(tQueue *queue, void *item)
{
	struct sQueue_e *list;

	if (queue->mutex) pthread_mutex_lock(queue->mutex);
	list = &queue->list;

	while (list->item) list = list->next;
	list->item = item;
	list->next = malloc(sizeof(struct sQueue_e));
	list->next->item = NULL;

	if (queue->mutex) pthread_mutex_unlock(queue->mutex);
}


/*----------------------------------------------------------------------------*/
void *QueueExtract(tQueue *queue)
{
	void *item;
	struct sQueue_e *list;

	if (queue->mutex) pthread_mutex_lock(queue->mutex);

	list = &queue->list;
	item = list->item;

	if (item) {
		struct sQueue_e *next = list->next;
		if (next->item) {
			list->item = next->item;
			list->next = next->next;
		} else list->item = NULL;
		NFREE(next);
	}

	if (queue->mutex) pthread_mutex_unlock(queue->mutex);

	return item;
}

/*----------------------------------------------------------------------------*/
void *QueueHead(tQueue *queue)
{
	void *item;

	if (queue->mutex) pthread_mutex_lock(queue->mutex);
	item = queue->list.item;
	if (queue->mutex) pthread_mutex_unlock(queue->mutex);

	return item;
}

/*----------------------------------------------------------------------------*/
void QueueFlush(tQueue *queue)
{
	struct sQueue_e *list;

	if (queue->mutex) pthread_mutex_lock(queue->mutex);

	list = &queue->list;

	while (list->item) {
		struct sQueue_e *next = list->next;
		if (queue->cleanup)	(*(queue->cleanup))(list->item);
		list = list->next;
		NFREE(next);
	}

	if (queue->mutex) {
		pthread_mutex_unlock(queue->mutex);
		pthread_mutex_destroy(queue->mutex);
		free(queue->mutex);
	}
}


/*----------------------------------------------------------------------------*/
/* 																			  */
/* LIST management															  */
/* 																			  */
/*----------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
list_t *push_item(list_t *item, list_t **list) {
  if (*list) item->next = *list;
  else item->next = NULL;

  *list = item;

  return item;
}


/*---------------------------------------------------------------------------*/
list_t *add_tail_item(list_t *item, list_t **list) {
  if (*list) {
	struct list_s *p = *list;
	while (p->next) p = p->next;
	item->next = p->next;
	p->next = item;
  } else {
	item->next = NULL;
	*list = item;
  }

  return item;
}


/*---------------------------------------------------------------------------*/
list_t *add_ordered_item(list_t *item, list_t **list, int (*compare)(void *a, void *b)) {
  if (*list) {
	struct list_s *p = *list;
	while (p->next && compare(p->next, item) <= 0) p = p->next;
	item->next = p->next;
	p->next = item;
  } else {
	item->next = NULL;
	*list = item;
  }

  return item;
}


/*---------------------------------------------------------------------------*/
list_t *pop_item(list_t **list) {
  if (*list) {
	list_t *item = *list;
	*list = item->next;
	return item;
  } else return NULL;
}


/*---------------------------------------------------------------------------*/
list_t *remove_item(list_t *item, list_t **list) {
  if (item != *list) {
	struct list_s *p = *list;
	while (p && p->next != item) p = p->next;
	if (p) p->next = item->next;
	item->next = NULL;
  } else *list = (*list)->next;

  return item;
}


/*---------------------------------------------------------------------------*/
void clear_list(list_t **list, void (*free_func)(void *)) {
  if (!*list) return;
  while (*list) {
	struct list_s *next = (*list)->next;
	if (free_func) (*free_func)(*list);
	else free(*list);
	*list = next;
  }
  *list = NULL;
}


/*----------------------------------------------------------------------------*/
char *XMLGetFirstDocumentItem(IXML_Document *doc, const char *item)
{
	IXML_NodeList *nodeList = NULL;
	IXML_Node *textNode = NULL;
	IXML_Node *tmpNode = NULL;
	char *ret = NULL;

	nodeList = ixmlDocument_getElementsByTagName(doc, (char *)item);
	if (nodeList) {
		tmpNode = ixmlNodeList_item(nodeList, 0);
		if (tmpNode) {
			textNode = ixmlNode_getFirstChild(tmpNode);
			if (!textNode) {
				LOG_WARN("(BUG) ixmlNode_getFirstChild(tmpNode) returned NULL", NULL);
				ret = strdup("");
			}
			else {
				ret = strdup(ixmlNode_getNodeValue(textNode));
				if (!ret) {
					LOG_WARN("ixmlNode_getNodeValue returned NULL", NULL);
					ret = strdup("");
				}
			}
		} else
			LOG_WARN("ixmlNodeList_item(nodeList, 0) returned NULL", NULL);
	} else
		LOG_SDEBUG("Error finding %s in XML Node", item);

	if (nodeList) ixmlNodeList_free(nodeList);

	return ret;
}

/*----------------------------------------------------------------------------*/
char *XMLGetFirstElementItem(IXML_Element *element, const char *item)
{
	IXML_NodeList *nodeList = NULL;
	IXML_Node *textNode = NULL;
	IXML_Node *tmpNode = NULL;
	char *ret = NULL;

	nodeList = ixmlElement_getElementsByTagName(element, (char *)item);
	if (nodeList == NULL) {
		LOG_WARN("Error finding %s in XML Node", item);
		return NULL;
	}
	tmpNode = ixmlNodeList_item(nodeList, 0);
	if (!tmpNode) {
		LOG_WARN("Error finding %s value in XML Node", item);
		ixmlNodeList_free(nodeList);
		return NULL;
	}
	textNode = ixmlNode_getFirstChild(tmpNode);
	ret = strdup(ixmlNode_getNodeValue(textNode));
	if (!ret) {
		LOG_ERROR("Error allocating memory for %s in XML Node",item);
		ixmlNodeList_free(nodeList);
		return NULL;
	}
	ixmlNodeList_free(nodeList);

	return ret;
}


/*----------------------------------------------------------------------------*/
IXML_Node *XMLAddNode(IXML_Document *doc, IXML_Node *parent, char *name, char *fmt, ...)
{
	IXML_Node *node, *elm;
	char buf[256];
	va_list args;

	elm = (IXML_Node*) ixmlDocument_createElement(doc, name);
	if (parent) ixmlNode_appendChild(parent, elm);
	else ixmlNode_appendChild((IXML_Node*) doc, elm);

	if (fmt) {
		va_start(args, fmt);

		vsprintf(buf, fmt, args);
		node = ixmlDocument_createTextNode(doc, buf);
		ixmlNode_appendChild(elm, node);

		va_end(args);
	}

	return elm;
}


/*----------------------------------------------------------------------------*/
IXML_Node *XMLUpdateNode(IXML_Document *doc, IXML_Node *parent, bool refresh, char *name, char *fmt, ...)
{
	char buf[256];
	va_list args;
	IXML_Node *node = (IXML_Node*) ixmlDocument_getElementById((IXML_Document*) parent, name);

	va_start(args, fmt);
	vsprintf(buf, fmt, args);

	if (!node) XMLAddNode(doc, parent, name, buf);
	else if (refresh) {
		node = ixmlNode_getFirstChild(node);
		ixmlNode_setNodeValue(node, buf);
	}

	va_end(args);

	return node;
}


/*----------------------------------------------------------------------------*/
int XMLAddAttribute(IXML_Document *doc, IXML_Node *parent, char *name, char *fmt, ...)
{
	char buf[256];
	int ret;
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, 256, fmt, args);
	ret = ixmlElement_setAttribute((IXML_Element*) parent, name, buf);
	va_end(args);

	return ret;
}


/*----------------------------------------------------------------------------*/
unsigned Time2Int(char *Time)
{
	char *p;
	unsigned ret;

	p = strrchr(Time, ':');

	if (!p) return 0;

	*p = '\0';
	ret = atol(p + 1);

	p = strrchr(Time, ':');
	if (p) {
		*p = '\0';
		ret += atol(p + 1)*60;
	}

	p = strrchr(Time, ':');
	if (p) {
		*p = '\0';
		ret += atol(p + 1)*3600;
	}

	return ret;
}


/*----------------------------------------------------------------------------*/
void MakeMacUnique(struct sMR *Device)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		if (!glMRDevices[i].Running || Device == &glMRDevices[i]) continue;
		if (!memcmp(&glMRDevices[i].sq_config.mac, &Device->sq_config.mac, 6)) {
			u32_t hash = hash32(Device->UDN);

			LOG_INFO("[%p]: duplicated mac ... updating", Device);
			memset(&Device->sq_config.mac[0], 0xcc, 2);
			memcpy(&Device->sq_config.mac[0] + 2, &hash, 4);
		}
	}
}


/*----------------------------------------------------------------------------*/
void SaveConfig(char *name, void *ref, bool full)
{
	struct sMR *p;
	IXML_Document *doc = ixmlDocument_createDocument();
	IXML_Document *old_doc = ref;
	IXML_Node	 *root, *common;
	IXML_NodeList *list;
	IXML_Element *old_root;
	char *s;
	FILE *file;
	int i;

	old_root = ixmlDocument_getElementById(old_doc, "squeeze2cast");

	if (!full && old_doc) {
		ixmlDocument_importNode(doc, (IXML_Node*) old_root, true, &root);
		ixmlNode_appendChild((IXML_Node*) doc, root);

		list = ixmlDocument_getElementsByTagName((IXML_Document*) root, "device");
		for (i = 0; i < (int) ixmlNodeList_length(list); i++) {
			IXML_Node *device;

			device = ixmlNodeList_item(list, i);
			ixmlNode_removeChild(root, device, &device);
			ixmlNode_free(device);
		}
		if (list) ixmlNodeList_free(list);
		common = (IXML_Node*) ixmlDocument_getElementById((IXML_Document*) root, "common");
	}
	else {
		root = XMLAddNode(doc, NULL, "squeeze2cast", NULL);
		common = (IXML_Node*) XMLAddNode(doc, root, "common", NULL);
	}

	XMLUpdateNode(doc, root, false, "upnp_socket", glUPnPSocket);
	XMLUpdateNode(doc, root, false, "slimproto_log", level2debug(slimproto_loglevel));
	XMLUpdateNode(doc, root, false, "slimmain_log", level2debug(slimmain_loglevel));
	XMLUpdateNode(doc, root, false, "stream_log", level2debug(stream_loglevel));
	XMLUpdateNode(doc, root, false, "output_log", level2debug(output_loglevel));
	XMLUpdateNode(doc, root, false, "decode_log", level2debug(decode_loglevel));
	XMLUpdateNode(doc, root, false, "main_log",level2debug(main_loglevel));
	XMLUpdateNode(doc, root, false, "cast_log",level2debug(cast_loglevel));
	XMLUpdateNode(doc, root, false, "util_log",level2debug(util_loglevel));
	XMLUpdateNode(doc, root, false, "log_limit", "%d", (s32_t) glLogLimit);
	XMLUpdateNode(doc, common, false, "streambuf_size", "%d", (u32_t) glDeviceParam.streambuf_size);
	XMLUpdateNode(doc, common, false, "output_size", "%d", (u32_t) glDeviceParam.outputbuf_size);
	XMLUpdateNode(doc, common, false, "stream_length", "%d", (s32_t) glDeviceParam.stream_length);
	XMLUpdateNode(doc, common, false, "enabled", "%d", (int) glMRConfig.Enabled);
	XMLUpdateNode(doc, common, false, "roon_mode", "%d", (int) glMRConfig.RoonMode);
	XMLUpdateNode(doc, common, false, "stop_receiver", "%d", (int) glMRConfig.StopReceiver);
	XMLUpdateNode(doc, common, false, "mode", glDeviceParam.mode);
	XMLUpdateNode(doc, common, false, "codecs", glDeviceParam.codecs);
	XMLUpdateNode(doc, common, false, "sample_rate", "%d", (int) glDeviceParam.sample_rate);
	XMLUpdateNode(doc, common, false, "flac_header", "%d", (int) glDeviceParam.flac_header);
	XMLUpdateNode(doc, common, false, "send_icy", "%d", (int) glDeviceParam.send_icy);
	XMLUpdateNode(doc, common, false, "volume_on_play", "%d", (int) glMRConfig.VolumeOnPlay);
	XMLUpdateNode(doc, common, false, "media_volume", "%d", (int) (glMRConfig.MediaVolume * 100));
	XMLUpdateNode(doc, common, false, "remove_timeout", "%d", (int) glMRConfig.RemoveTimeout);
	XMLUpdateNode(doc, common, false, "send_metadata", "%d", (int) glMRConfig.SendMetaData);
	XMLUpdateNode(doc, common, false, "send_coverart", "%d", (int) glMRConfig.SendCoverArt);
	XMLUpdateNode(doc, common, false, "auto_play", "%d", (int) glMRConfig.AutoPlay);
	XMLUpdateNode(doc, common, false, "server", glDeviceParam.server);

	for (i = 0; i < MAX_RENDERERS; i++) {
		IXML_Node *dev_node;

		if (!glMRDevices[i].Running) continue;
		else p = &glMRDevices[i];

		// existing device, keep param and update "name" if LMS has requested it
		if (old_doc && ((dev_node = (IXML_Node*) FindMRConfig(old_doc, p->UDN)) != NULL)) {
			ixmlDocument_importNode(doc, dev_node, true, &dev_node);
			ixmlNode_appendChild((IXML_Node*) root, dev_node);

			XMLUpdateNode(doc, dev_node, false, "friendly_name", p->FriendlyName);
			XMLUpdateNode(doc, dev_node, true, "name", p->sq_config.name);
			if (*p->sq_config.dynamic.server) XMLUpdateNode(doc, dev_node, true, "server", p->sq_config.dynamic.server);
		}
		// new device, add nodes
		else {
			dev_node = XMLAddNode(doc, root, "device", NULL);
			XMLAddNode(doc, dev_node, "udn", p->UDN);
			XMLAddNode(doc, dev_node, "name", p->FriendlyName);
			XMLAddNode(doc, dev_node, "friendly_name", p->FriendlyName);
			if (*p->sq_config.dynamic.server) XMLAddNode(doc, dev_node, "server", p->sq_config.dynamic.server);
			XMLAddNode(doc, dev_node, "mac", "%02x:%02x:%02x:%02x:%02x:%02x", p->sq_config.mac[0],
						p->sq_config.mac[1], p->sq_config.mac[2], p->sq_config.mac[3], p->sq_config.mac[4], p->sq_config.mac[5]);
			XMLAddNode(doc, dev_node, "enabled", "%d", (int) p->Config.Enabled);
		}
	}

	// add devices in old XML file that has not been discovered
	list = ixmlDocument_getElementsByTagName((IXML_Document*) old_root, "device");
	for (i = 0; i < (int) ixmlNodeList_length(list); i++) {
		char *udn;
		IXML_Node *device, *node;

		device = ixmlNodeList_item(list, i);
		node = (IXML_Node*) ixmlDocument_getElementById((IXML_Document*) device, "udn");
		node = ixmlNode_getFirstChild(node);
		udn = (char*) ixmlNode_getNodeValue(node);
		if (!FindMRConfig(doc, udn)) {
			ixmlDocument_importNode(doc, device, true, &device);
			ixmlNode_appendChild((IXML_Node*) root, device);
		}
	}
	if (list) ixmlNodeList_free(list);

	file = fopen(name, "wb");
	s = ixmlDocumenttoString(doc);
	fwrite(s, 1, strlen(s), file);
	fclose(file);
	free(s);

	ixmlDocument_free(doc);
}


/*----------------------------------------------------------------------------*/
static void LoadConfigItem(tMRConfig *Conf, sq_dev_param_t *sq_conf, char *name, char *val)
{
	if (!val) return;

	if (!strcmp(name, "stream_length")) sq_conf->stream_length = atol(val);
	if (!strcmp(name, "streambuf_size")) sq_conf->streambuf_size = atol(val);
	if (!strcmp(name, "output_size")) sq_conf->outputbuf_size = atol(val);
	if (!strcmp(name, "send_icy")) sq_conf->send_icy = atol(val);
	if (!strcmp(name, "enabled")) Conf->Enabled = atol(val);
	if (!strcmp(name, "roon_mode")) Conf->RoonMode = atol(val);
	if (!strcmp(name, "stop_receiver")) Conf->StopReceiver = atol(val);
	if (!strcmp(name, "codecs")) strcpy(sq_conf->codecs, val);
	if (!strcmp(name, "mode")) strcpy(sq_conf->mode, val);
	if (!strcmp(name, "sample_rate"))sq_conf->sample_rate = atol(val);
	if (!strcmp(name, "flac_header"))sq_conf->flac_header = atol(val);
	if (!strcmp(name, "volume_on_play")) Conf->VolumeOnPlay = atol(val);
	if (!strcmp(name, "media_volume")) Conf->MediaVolume = atof(val) / 100;
	if (!strcmp(name, "remove_timeout")) Conf->RemoveTimeout = atol(val);
	if (!strcmp(name, "auto_play")) Conf->AutoPlay = atol(val);
	if (!strcmp(name, "send_metadata")) Conf->SendMetaData = atol(val);
	if (!strcmp(name, "send_coverart")) Conf->SendCoverArt = atol(val);
	if (!strcmp(name, "name")) strcpy(sq_conf->name, val);
	if (!strcmp(name, "server")) strcpy(sq_conf->server, val);
	if (!strcmp(name, "mac"))  {
		unsigned mac[6];
		int i;
		// seems to be a Windows scanf buf, cannot support %hhx
		sscanf(val,"%2x:%2x:%2x:%2x:%2x:%2x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
		for (i = 0; i < 6; i++) sq_conf->mac[i] = mac[i];
	}
}

/*----------------------------------------------------------------------------*/
static void LoadGlobalItem(char *name, char *val)
{
	if (!val) return;

	// temporary to ensure parameter transfer from global to common
	if (!strcmp(name, "server")) strcpy(glDeviceParam.server, val);

	if (!strcmp(name, "upnp_socket")) strcpy(glUPnPSocket, val);
	if (!strcmp(name, "slimproto_log")) slimproto_loglevel = debug2level(val);
	if (!strcmp(name, "slimmain_log")) slimmain_loglevel = debug2level(val);
	if (!strcmp(name, "stream_log")) stream_loglevel = debug2level(val);
	if (!strcmp(name, "output_log")) output_loglevel = debug2level(val);
	if (!strcmp(name, "decode_log")) decode_loglevel = debug2level(val);
	if (!strcmp(name, "main_log")) main_loglevel = debug2level(val);
	if (!strcmp(name, "cast_log")) cast_loglevel = debug2level(val);
	if (!strcmp(name, "util_log")) util_loglevel = debug2level(val);
	if (!strcmp(name, "log_limit")) glLogLimit = atol(val);
 }


/*----------------------------------------------------------------------------*/
void *FindMRConfig(void *ref, char *UDN)
{
	IXML_Element *elm;
	IXML_Node	*device = NULL;
	IXML_NodeList *l1_node_list;
	IXML_Document *doc = (IXML_Document*) ref;
	char *v;
	unsigned i;

	elm = ixmlDocument_getElementById(doc, "squeeze2cast");
	l1_node_list = ixmlDocument_getElementsByTagName((IXML_Document*) elm, "udn");
	for (i = 0; i < ixmlNodeList_length(l1_node_list); i++) {
		IXML_Node *l1_node, *l1_1_node;
		l1_node = ixmlNodeList_item(l1_node_list, i);
		l1_1_node = ixmlNode_getFirstChild(l1_node);
		v = (char*) ixmlNode_getNodeValue(l1_1_node);
		if (v && !strcmp(v, UDN)) {
			device = ixmlNode_getParentNode(l1_node);
			break;
		}
	}
	if (l1_node_list) ixmlNodeList_free(l1_node_list);
	return device;
}

/*----------------------------------------------------------------------------*/
void *LoadMRConfig(void *ref, char *UDN, tMRConfig *Conf, sq_dev_param_t *sq_conf)
{
	IXML_NodeList *node_list;
	IXML_Document *doc = (IXML_Document*) ref;
	IXML_Node *node;
	char *n, *v;
	unsigned i;

	node = (IXML_Node*) FindMRConfig(doc, UDN);
	if (node) {
		node_list = ixmlNode_getChildNodes(node);
		for (i = 0; i < ixmlNodeList_length(node_list); i++) {
			IXML_Node *l1_node, *l1_1_node;
			l1_node = ixmlNodeList_item(node_list, i);
			n = (char*) ixmlNode_getNodeName(l1_node);
			l1_1_node = ixmlNode_getFirstChild(l1_node);
			v = (char*) ixmlNode_getNodeValue(l1_1_node);
			LoadConfigItem(Conf, sq_conf, n, v);
		}
		if (node_list) ixmlNodeList_free(node_list);
	}

	return node;
}

/*----------------------------------------------------------------------------*/
void *LoadConfig(char *name, tMRConfig *Conf, sq_dev_param_t *sq_conf)
{
	IXML_Element *elm;
	IXML_Document	*doc;

	doc = ixmlLoadDocument(name);
	if (!doc) return NULL;

	elm = ixmlDocument_getElementById(doc, "squeeze2cast");
	if (elm) {
		unsigned i;
		char *n, *v;
		IXML_NodeList *l1_node_list;
		l1_node_list = ixmlNode_getChildNodes((IXML_Node*) elm);
		for (i = 0; i < ixmlNodeList_length(l1_node_list); i++) {
			IXML_Node *l1_node, *l1_1_node;
			l1_node = ixmlNodeList_item(l1_node_list, i);
			n = (char*) ixmlNode_getNodeName(l1_node);
			l1_1_node = ixmlNode_getFirstChild(l1_node);
			v = (char*) ixmlNode_getNodeValue(l1_1_node);
			LoadGlobalItem(n, v);
		}
		if (l1_node_list) ixmlNodeList_free(l1_node_list);
	}

	elm = ixmlDocument_getElementById((IXML_Document	*)elm, "common");
	if (elm) {
		char *n, *v;
		IXML_NodeList *l1_node_list;
		unsigned i;
		l1_node_list = ixmlNode_getChildNodes((IXML_Node*) elm);
		for (i = 0; i < ixmlNodeList_length(l1_node_list); i++) {
			IXML_Node *l1_node, *l1_1_node;
			l1_node = ixmlNodeList_item(l1_node_list, i);
			n = (char*) ixmlNode_getNodeName(l1_node);
			l1_1_node = ixmlNode_getFirstChild(l1_node);
			v = (char*) ixmlNode_getNodeValue(l1_1_node);
			LoadConfigItem(&glMRConfig, &glDeviceParam, n, v);
		}
		if (l1_node_list) ixmlNodeList_free(l1_node_list);
	}

	return doc;
 }


/*----------------------------------------------------------------------------*/
char *uPNPEvent2String(Upnp_EventType S)
{
	switch (S) {
	/* Discovery */
	case UPNP_DISCOVERY_ADVERTISEMENT_ALIVE:
		return "UPNP_DISCOVERY_ADVERTISEMENT_ALIVE";
	case UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE:
		return "UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE";
	case UPNP_DISCOVERY_SEARCH_RESULT:
		return "UPNP_DISCOVERY_SEARCH_RESULT";
	case UPNP_DISCOVERY_SEARCH_TIMEOUT:
		return "UPNP_DISCOVERY_SEARCH_TIMEOUT";
	/* SOAP */
	case UPNP_CONTROL_ACTION_REQUEST:
		return "UPNP_CONTROL_ACTION_REQUEST";
	case UPNP_CONTROL_ACTION_COMPLETE:
		return "UPNP_CONTROL_ACTION_COMPLETE";
	case UPNP_CONTROL_GET_VAR_REQUEST:
		return "UPNP_CONTROL_GET_VAR_REQUEST";
	case UPNP_CONTROL_GET_VAR_COMPLETE:
		return "UPNP_CONTROL_GET_VAR_COMPLETE";
	case UPNP_EVENT_SUBSCRIPTION_REQUEST:
		return "UPNP_EVENT_SUBSCRIPTION_REQUEST";
	case UPNP_EVENT_RECEIVED:
		return "UPNP_EVENT_RECEIVED";
	case UPNP_EVENT_RENEWAL_COMPLETE:
		return "UPNP_EVENT_RENEWAL_COMPLETE";
	case UPNP_EVENT_SUBSCRIBE_COMPLETE:
		return "UPNP_EVENT_SUBSCRIBE_COMPLETE";
	case UPNP_EVENT_UNSUBSCRIBE_COMPLETE:
		return "UPNP_EVENT_UNSUBSCRIBE_COMPLETE";
	case UPNP_EVENT_AUTORENEWAL_FAILED:
		return "UPNP_EVENT_AUTORENEWAL_FAILED";
	case UPNP_EVENT_SUBSCRIPTION_EXPIRED:
		return "UPNP_EVENT_SUBSCRIPTION_EXPIRED";
	}

	return "";
}



