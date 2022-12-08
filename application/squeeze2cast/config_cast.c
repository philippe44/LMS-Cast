/*
 *  Squeeze2cast - Coniguration
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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "ixmlextra.h"
#include "squeeze2cast.h"
#include "config_cast.h"
#include "cross_log.h"

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
extern bool 		log_cmdline;

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
extern log_level 	util_loglevel;
static log_level __attribute__((unused)) * loglevel = &util_loglevel;

static void *MigrateConfig(IXML_Document *doc);
static void *MigrateMRConfig(IXML_Node *device);

/*----------------------------------------------------------------------------*/
void SaveConfig(char *name, void *ref, bool full) {
	struct sMR *p;
	IXML_Document *doc = ixmlDocument_createDocument();
	IXML_Document *old_doc = ref;
	IXML_Node	 *root, *common;
	IXML_NodeList *list;
	IXML_Element *old_root;

	old_root = ixmlDocument_getElementById(old_doc, "squeeze2cast");

	if (!full && old_doc) {
		ixmlDocument_importNode(doc, (IXML_Node*) old_root, true, &root);
		ixmlNode_appendChild((IXML_Node*) doc, root);

		list = ixmlDocument_getElementsByTagName((IXML_Document*) root, "device");
		for (int i = 0; i < (int) ixmlNodeList_length(list); i++) {
			IXML_Node *device = ixmlNodeList_item(list, i);
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

	XMLUpdateNode(doc, root, false, "binding", glBinding);
	// do not save loglevel when set from cmd line
	if (!log_cmdline) {
		XMLUpdateNode(doc, root, false, "slimproto_log", level2debug(slimproto_loglevel));
		XMLUpdateNode(doc, root, false, "slimmain_log", level2debug(slimmain_loglevel));
		XMLUpdateNode(doc, root, false, "stream_log", level2debug(stream_loglevel));
		XMLUpdateNode(doc, root, false, "output_log", level2debug(output_loglevel));
		XMLUpdateNode(doc, root, false, "decode_log", level2debug(decode_loglevel));
		XMLUpdateNode(doc, root, false, "main_log",level2debug(main_loglevel));
		XMLUpdateNode(doc, root, false, "cast_log",level2debug(cast_loglevel));
		XMLUpdateNode(doc, root, false, "util_log",level2debug(util_loglevel));
	}
	XMLUpdateNode(doc, root, false, "log_limit", "%d", (int32_t) glLogLimit);
	XMLUpdateNode(doc, common, false, "streambuf_size", "%d", (uint32_t) glDeviceParam.streambuf_size);
	XMLUpdateNode(doc, common, false, "output_size", "%d", (uint32_t) glDeviceParam.outputbuf_size);
	XMLUpdateNode(doc, common, false, "stream_length", "%d", (int32_t) glDeviceParam.stream_length);
	XMLUpdateNode(doc, common, false, "enabled", "%d", (int) glMRConfig.Enabled);
	XMLUpdateNode(doc, common, false, "stop_receiver", "%d", (int) glMRConfig.StopReceiver);
	XMLUpdateNode(doc, common, false, "mode", glDeviceParam.mode);
	XMLUpdateNode(doc, common, false, "codecs", glDeviceParam.codecs);
	XMLUpdateNode(doc, common, false, "sample_rate", "%d", (int) glDeviceParam.sample_rate);
	XMLUpdateNode(doc, common, false, "flac_header", "%d", (int) glDeviceParam.flac_header);
	XMLUpdateNode(doc, common, false, "roon_mode", "%d", (int) glDeviceParam.roon_mode);
	XMLUpdateNode(doc, common, false, "send_icy", "%d", (int) glDeviceParam.send_icy);
	XMLUpdateNode(doc, common, false, "volume_on_play", "%d", (int) glMRConfig.VolumeOnPlay);
	XMLUpdateNode(doc, common, false, "volume_feedback", "%d", (int) glMRConfig.VolumeFeedback);
	XMLUpdateNode(doc, common, false, "media_volume", "%d", (int) (glMRConfig.MediaVolume * 100));
	XMLUpdateNode(doc, common, false, "remove_timeout", "%d", (int) glMRConfig.RemoveTimeout);
	XMLUpdateNode(doc, common, false, "send_metadata", "%d", (int) glMRConfig.SendMetaData);
	XMLUpdateNode(doc, common, false, "send_coverart", "%d", (int) glMRConfig.SendCoverArt);
	XMLUpdateNode(doc, common, false, "auto_play", "%d", (int) glMRConfig.AutoPlay);
	XMLUpdateNode(doc, common, false, "server", glDeviceParam.server);

	for (int i = 0; i < MAX_RENDERERS; i++) {
		IXML_Node *dev_node;

		if (!glMRDevices[i].Running) continue;
		else p = &glMRDevices[i];

		// existing device, keep param and update "name" if LMS has requested it
		if (old_doc && ((dev_node = (IXML_Node*) FindMRConfig(old_doc, p->UDN)) != NULL)) {
			ixmlDocument_importNode(doc, dev_node, true, &dev_node);
			ixmlNode_appendChild((IXML_Node*) root, dev_node);

			XMLUpdateNode(doc, dev_node, false, "friendly_name", p->FriendlyName);
			XMLUpdateNode(doc, dev_node, true, "name", p->sq_config.name);
			if (*p->sq_config.set_server) XMLUpdateNode(doc, dev_node, true, "server", p->sq_config.set_server);
		}
		// new device, add nodes
		else {
			dev_node = XMLAddNode(doc, root, "device", NULL);
			XMLAddNode(doc, dev_node, "udn", p->UDN);
			XMLAddNode(doc, dev_node, "name", p->FriendlyName);
			XMLAddNode(doc, dev_node, "friendly_name", p->FriendlyName);
			if (*p->sq_config.set_server) XMLAddNode(doc, dev_node, "server", p->sq_config.set_server);
			XMLAddNode(doc, dev_node, "mac", "%02x:%02x:%02x:%02x:%02x:%02x", p->sq_config.mac[0],
						p->sq_config.mac[1], p->sq_config.mac[2], p->sq_config.mac[3], p->sq_config.mac[4], p->sq_config.mac[5]);
			XMLAddNode(doc, dev_node, "enabled", "%d", (int) p->Config.Enabled);
		}
	}

	// add devices in old XML file that has not been discovered
	list = ixmlDocument_getElementsByTagName((IXML_Document*) old_root, "device");
	for (int i = 0; i < (int) ixmlNodeList_length(list); i++) {
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

	FILE* file = fopen(name, "wb");
	char* s = ixmlDocumenttoString(doc);
	fwrite(s, 1, strlen(s), file);
	fclose(file);
	free(s);

	ixmlDocument_free(doc);
}


/*----------------------------------------------------------------------------*/
static void LoadConfigItem(tMRConfig *Conf, sq_dev_param_t *sq_conf, char *name, char *val) {
	if (!val) return;

	if (!strcmp(name, "stream_length")) sq_conf->stream_length = atol(val);
	if (!strcmp(name, "streambuf_size")) sq_conf->streambuf_size = atol(val);
	if (!strcmp(name, "output_size")) sq_conf->outputbuf_size = atol(val);
	if (!strcmp(name, "send_icy")) sq_conf->send_icy = atol(val);
	if (!strcmp(name, "enabled")) Conf->Enabled = atol(val);
	if (!strcmp(name, "roon_mode")) sq_conf->roon_mode = atol(val);
	if (!strcmp(name, "store_prefix")) strcpy(sq_conf->store_prefix, val);			//RO
	if (!strcmp(name, "stop_receiver")) Conf->StopReceiver = atol(val);
	if (!strcmp(name, "codecs")) strcpy(sq_conf->codecs, val);
	if (!strcmp(name, "mode")) strcpy(sq_conf->mode, val);
	if (!strcmp(name, "sample_rate"))sq_conf->sample_rate = atol(val);
	if (!strcmp(name, "flac_header"))sq_conf->flac_header = atol(val);
	if (!strcmp(name, "volume_on_play")) Conf->VolumeOnPlay = atol(val);
	if (!strcmp(name, "volume_feedback")) Conf->VolumeFeedback = atol(val);
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
static void LoadGlobalItem(char *name, char *val) {
	if (!val) return;

	if (!strcmp(name, "binding")) strcpy(glBinding, val);
	if (!strcmp(name, "slimproto_log")) slimproto_loglevel = debug2level(val);
	if (!strcmp(name, "slimmain_log")) slimmain_loglevel = debug2level(val);
	if (!strcmp(name, "stream_log")) stream_loglevel = debug2level(val);
	if (!strcmp(name, "output_log")) output_loglevel = debug2level(val);
	if (!strcmp(name, "decode_log")) decode_loglevel = debug2level(val);
	if (!strcmp(name, "main_log")) main_loglevel = debug2level(val);
	if (!strcmp(name, "cast_log")) cast_loglevel = debug2level(val);
	if (!strcmp(name, "util_log")) util_loglevel = debug2level(val);
	if (!strcmp(name, "log_limit")) glLogLimit = atol(val);

	// deprecated
	if (!strcmp(name, "upnp_socket")) strcpy(glBinding, val);
 }


/*----------------------------------------------------------------------------*/
void *FindMRConfig(void *ref, char *UDN) {
	IXML_Node	*device = NULL;
	IXML_Document *doc = (IXML_Document*) ref;
	IXML_Element* elm = ixmlDocument_getElementById(doc, "squeeze2cast");
	IXML_NodeList* l1_node_list = ixmlDocument_getElementsByTagName((IXML_Document*) elm, "udn");

	for (int i = 0; i < ixmlNodeList_length(l1_node_list); i++) {
		IXML_Node* l1_node = ixmlNodeList_item(l1_node_list, i);
		IXML_Node* l1_1_node = ixmlNode_getFirstChild(l1_node);
		char* v = (char*) ixmlNode_getNodeValue(l1_1_node);
		if (v && !strcmp(v, UDN)) {
			device = ixmlNode_getParentNode(l1_node);
			break;
		}
	}
	if (l1_node_list) ixmlNodeList_free(l1_node_list);
	return device;
}

/*----------------------------------------------------------------------------*/
void *LoadMRConfig(void *ref, char *UDN, tMRConfig *Conf, sq_dev_param_t *sq_conf) {
	IXML_Document *doc = (IXML_Document*) ref;
	IXML_Node* node = (IXML_Node*) FindMRConfig(doc, UDN);

	if (node) {
		IXML_NodeList* node_list = ixmlNode_getChildNodes(node);
		for (int i = 0; i < ixmlNodeList_length(node_list); i++) {
			IXML_Node* l1_node = ixmlNodeList_item(node_list, i);
			char* n = (char*) ixmlNode_getNodeName(l1_node);
			IXML_Node* l1_1_node = ixmlNode_getFirstChild(l1_node);
			char* v = (char*) ixmlNode_getNodeValue(l1_1_node);
			LoadConfigItem(Conf, sq_conf, n, v);
		}
		if (node_list) ixmlNodeList_free(node_list);
	}

	return MigrateMRConfig(node);
}


/*----------------------------------------------------------------------------*/
void *LoadConfig(char *name, tMRConfig *Conf, sq_dev_param_t *sq_conf) {
	IXML_Document* doc = ixmlLoadDocument(name);
	if (!doc) return NULL;

	IXML_Element* elm = ixmlDocument_getElementById(doc, "squeeze2cast");

	if (elm) {
		IXML_NodeList *l1_node_list = ixmlNode_getChildNodes((IXML_Node*) elm);
		for (int i = 0; i < ixmlNodeList_length(l1_node_list); i++) {
			IXML_Node* l1_node = ixmlNodeList_item(l1_node_list, i);
			char* n = (char*) ixmlNode_getNodeName(l1_node);
			IXML_Node* l1_1_node = ixmlNode_getFirstChild(l1_node);
			char* v = (char*) ixmlNode_getNodeValue(l1_1_node);
			LoadGlobalItem(n, v);
		}
		if (l1_node_list) ixmlNodeList_free(l1_node_list);
	}

	elm = ixmlDocument_getElementById((IXML_Document	*)elm, "common");

	if (elm) {
		IXML_NodeList* l1_node_list = ixmlNode_getChildNodes((IXML_Node*) elm);
		for (int i = 0; i < ixmlNodeList_length(l1_node_list); i++) {
			IXML_Node* l1_node = ixmlNodeList_item(l1_node_list, i);
			char* n = (char*) ixmlNode_getNodeName(l1_node);
			IXML_Node* l1_1_node = ixmlNode_getFirstChild(l1_node);
			char* v = (char*) ixmlNode_getNodeValue(l1_1_node);
			LoadConfigItem(&glMRConfig, &glDeviceParam, n, v);
		}
		if (l1_node_list) ixmlNodeList_free(l1_node_list);
	}

	return MigrateConfig(doc);
}


/*---------------------------------------------------------------------------*/
static void *MigrateConfig(IXML_Document *doc) {
	if (!doc) return NULL;

	// change "upnp_socket" into "binding"
	char* value = XMLDelNode((IXML_Node*) doc, "upnp_socket");
	if (value) {
		IXML_Node* node = XMLUpdateNode(doc, (IXML_Node*) doc, false, "binding", "%s", value);
		if (!node) strcpy(glBinding, value);
		free(value);
	}

	return doc;
}

/*---------------------------------------------------------------------------*/
static void *MigrateMRConfig(IXML_Node *device) {
	return device;
}



