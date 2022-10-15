/*
 *  Squeeze2cast - Configuration access
 *
 *  (c) Philippe 2014, philippe_44@outlook.com
 *
 *  See LICENSE
 *
 */

#pragma once

#include "squeeze2cast.h"

void	  	SaveConfig(char *name, void *ref, bool full);
void	   	*LoadConfig(char *name, struct sMRConfig *Conf, sq_dev_param_t *sq_conf);
void	  	*FindMRConfig(void *ref, char *UDN);
void 	  	*LoadMRConfig(void *ref, char *UDN, struct sMRConfig *Conf, struct sq_dev_param_s *sq_conf);
