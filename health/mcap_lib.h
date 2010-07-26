/*
 *
 *  MCAP for BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2010 GSyC/LibreSoft, Universidad Rey Juan Carlos.
 *
 *  Authors:
 *  Santiago Carot-Nemesio <sancane at gmail.com>
 *  Jose Antonio Santos-Cadenas <santoscadenas at gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifndef __MCAP_LIB_H
#define __MCAP_LIB_H

#ifdef __cplusplus
extern "C" {
#endif

struct mcap_instance;
struct mcap_mcl;

typedef void (* mcap_mcl_event_cb) (struct mcap_mcl *mcl, gpointer data);

/* MCAP main operations */

struct mcap_instance *mcap_create_instance(bdaddr_t *src,
					BtIOSecLevel sec, uint16_t ccpsm,
					uint16_t dcpsm,
					mcap_mcl_event_cb mcl_connected,
					mcap_mcl_event_cb mcl_reconnected,
					mcap_mcl_event_cb mcl_disconnected,
					mcap_mcl_event_cb mcl_uncached,
					gpointer user_data,
					GError **gerr);

void mcap_release_instance(struct mcap_instance *mi);

#ifdef __cplusplus
}
#endif

#endif /* __MCAP_LIB_H */
