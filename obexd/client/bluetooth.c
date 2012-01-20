/*
 *
 *  OBEX Client
 *
 *  Copyright (C) 2012 Intel Corporation
 *
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <inttypes.h>

#include <glib.h>
#include <gdbus.h>
#include <btio.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include "log.h"
#include "transport.h"
#include "bluetooth.h"

#define BT_BUS_NAME		"org.bluez"
#define BT_PATH			"/"
#define BT_ADAPTER_IFACE	"org.bluez.Adapter"
#define BT_MANAGER_IFACE	"org.bluez.Manager"

#define OBC_BT_ERROR obc_bt_error_quark()

struct bluetooth_session {
	guint id;
	bdaddr_t src;
	bdaddr_t dst;
	uint16_t port;
	DBusConnection *conn_system; /* system bus connection */
	sdp_session_t *sdp;
	GIOChannel *io;
	GSList *pending_calls;
	char *adapter;
	char *service;
	obc_transport_func func;
	void *user_data;
};

struct pending_req {
	DBusPendingCall *call;
	void *user_data;
};

static GSList *sessions = NULL;

static GQuark obc_bt_error_quark(void)
{
	return g_quark_from_static_string("obc-bluetooth-error-quark");
}

static struct pending_req *send_method_call(DBusConnection *connection,
				const char *dest, const char *path,
				const char *interface, const char *method,
				DBusPendingCallNotifyFunction cb,
				void *user_data, int type, ...)
{
	DBusMessage *msg;
	DBusPendingCall *call;
	va_list args;
	struct pending_req *req;

	msg = dbus_message_new_method_call(dest, path, interface, method);
	if (!msg) {
		error("Unable to allocate new D-Bus %s message", method);
		return NULL;
	}

	va_start(args, type);

	if (!dbus_message_append_args_valist(msg, type, args)) {
		dbus_message_unref(msg);
		va_end(args);
		return NULL;
	}

	va_end(args);

	if (!cb) {
		g_dbus_send_message(connection, msg);
		return 0;
	}

	if (!dbus_connection_send_with_reply(connection, msg, &call, -1)) {
		error("Sending %s failed", method);
		dbus_message_unref(msg);
		return NULL;
	}

	dbus_pending_call_set_notify(call, cb, user_data, NULL);

	req = g_new0(struct pending_req, 1);
	req->call = call;
	req->user_data = user_data;

	dbus_message_unref(msg);

	return req;
}

static void pending_req_finalize(struct pending_req *req)
{
	if (!dbus_pending_call_get_completed(req->call))
		dbus_pending_call_cancel(req->call);

	dbus_pending_call_unref(req->call);
	g_free(req);
}

static void session_destroy(struct bluetooth_session *session)
{
	GSList *l;

	DBG("%p", session);

	if (g_slist_find(sessions, session) == NULL)
		return;

	sessions = g_slist_remove(sessions, session);

	if (session->adapter)
		send_method_call(session->conn_system,
				BT_BUS_NAME, session->adapter,
				BT_ADAPTER_IFACE, "ReleaseSession",
				NULL, NULL,
				DBUS_TYPE_INVALID);

	l = session->pending_calls;

	while (l) {
		struct pending_req *req = l->data;
		l = l->next;

		session->pending_calls = g_slist_remove(session->pending_calls, req);
		pending_req_finalize(req);
	}

	if (session->io != NULL) {
		g_io_channel_shutdown(session->io, TRUE, NULL);
		g_io_channel_unref(session->io);
	}

	if (session->conn_system)
		dbus_connection_unref(session->conn_system);

	g_free(session->service);
	g_free(session->adapter);
	g_free(session);
}

static void rfcomm_callback(GIOChannel *io, GError *err, gpointer user_data)
{
	struct bluetooth_session *session = user_data;

	DBG("");

	if (session->func)
		session->func(io, err, session->user_data);

	if (err != NULL)
		session_destroy(session);
}

static GIOChannel *rfcomm_connect(const bdaddr_t *src, const bdaddr_t *dst,
					uint8_t channel, BtIOConnect function,
					gpointer user_data)
{
	GIOChannel *io;
	GError *err = NULL;

	DBG("");

	io = bt_io_connect(BT_IO_RFCOMM, function, user_data, NULL, &err,
				BT_IO_OPT_SOURCE_BDADDR, src,
				BT_IO_OPT_DEST_BDADDR, dst,
				BT_IO_OPT_CHANNEL, channel,
				BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_LOW,
				BT_IO_OPT_INVALID);
	if (io != NULL)
		return io;

	error("%s", err->message);
	g_error_free(err);
	return NULL;
}

static void search_callback(uint8_t type, uint16_t status,
			uint8_t *rsp, size_t size, void *user_data)
{
	struct bluetooth_session *session = user_data;
	unsigned int scanned, bytesleft = size;
	int seqlen = 0;
	uint8_t dataType, channel = 0;
	GError *gerr = NULL;

	if (status || type != SDP_SVC_SEARCH_ATTR_RSP)
		goto failed;

	scanned = sdp_extract_seqtype(rsp, bytesleft, &dataType, &seqlen);
	if (!scanned || !seqlen)
		goto failed;

	rsp += scanned;
	bytesleft -= scanned;
	do {
		sdp_record_t *rec;
		sdp_list_t *protos;
		int recsize, ch = -1;

		recsize = 0;
		rec = sdp_extract_pdu(rsp, bytesleft, &recsize);
		if (!rec)
			break;

		if (!recsize) {
			sdp_record_free(rec);
			break;
		}

		if (!sdp_get_access_protos(rec, &protos)) {
			ch = sdp_get_proto_port(protos, RFCOMM_UUID);
			sdp_list_foreach(protos,
					(sdp_list_func_t) sdp_list_free, NULL);
			sdp_list_free(protos, NULL);
			protos = NULL;
		}

		sdp_record_free(rec);

		if (ch > 0) {
			channel = ch;
			break;
		}

		scanned += recsize;
		rsp += recsize;
		bytesleft -= recsize;
	} while (scanned < size && bytesleft > 0);

	if (channel == 0)
		goto failed;

	session->port = channel;

	g_io_channel_set_close_on_unref(session->io, FALSE);
	g_io_channel_unref(session->io);

	session->io = rfcomm_connect(&session->src, &session->dst, channel,
					rfcomm_callback, session);
	if (session->io != NULL) {
		sdp_close(session->sdp);
		session->sdp = NULL;
		return;
	}

failed:
	g_io_channel_shutdown(session->io, TRUE, NULL);
	g_io_channel_unref(session->io);
	session->io = NULL;

	g_set_error(&gerr, OBC_BT_ERROR, -EIO,
					"Unable to find service record");
	if (session->func)
		session->func(session->io, gerr, session->user_data);
	g_clear_error(&gerr);

	session_destroy(session);
}

static gboolean process_callback(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct bluetooth_session *session = user_data;

	if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))
		return FALSE;

	if (sdp_process(session->sdp) < 0)
		return FALSE;

	return TRUE;
}

static int bt_string2uuid(uuid_t *uuid, const char *string)
{
	uint32_t data0, data4;
	uint16_t data1, data2, data3, data5;

	if (sscanf(string, "%08x-%04hx-%04hx-%04hx-%08x%04hx",
				&data0, &data1, &data2, &data3, &data4, &data5) == 6) {
		uint8_t val[16];

		data0 = g_htonl(data0);
		data1 = g_htons(data1);
		data2 = g_htons(data2);
		data3 = g_htons(data3);
		data4 = g_htonl(data4);
		data5 = g_htons(data5);

		memcpy(&val[0], &data0, 4);
		memcpy(&val[4], &data1, 2);
		memcpy(&val[6], &data2, 2);
		memcpy(&val[8], &data3, 2);
		memcpy(&val[10], &data4, 4);
		memcpy(&val[14], &data5, 2);

		sdp_uuid128_create(uuid, val);

		return 0;
	}

	return -EINVAL;
}

static gboolean service_callback(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct bluetooth_session *session = user_data;
	sdp_list_t *search, *attrid;
	uint32_t range = 0x0000ffff;
	GError *gerr = NULL;
	uuid_t uuid;

	if (cond & G_IO_NVAL)
		return FALSE;

	if (cond & G_IO_ERR)
		goto failed;

	if (sdp_set_notify(session->sdp, search_callback, session) < 0)
		goto failed;

	if (bt_string2uuid(&uuid, session->service) < 0)
		goto failed;

	search = sdp_list_append(NULL, &uuid);
	attrid = sdp_list_append(NULL, &range);

	if (sdp_service_search_attr_async(session->sdp,
				search, SDP_ATTR_REQ_RANGE, attrid) < 0) {
		sdp_list_free(attrid, NULL);
		sdp_list_free(search, NULL);
		goto failed;
	}

	sdp_list_free(attrid, NULL);
	sdp_list_free(search, NULL);

	g_io_add_watch(io, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
						process_callback, session);

	return FALSE;

failed:
	g_io_channel_shutdown(session->io, TRUE, NULL);
	g_io_channel_unref(session->io);
	session->io = NULL;

	g_set_error(&gerr, OBC_BT_ERROR, -EIO,
					"Unable to find service record");
	if (session->func)
		session->func(session->io, gerr, session->user_data);
	g_clear_error(&gerr);

	session_destroy(session);
	return FALSE;
}

static sdp_session_t *service_connect(const bdaddr_t *src, const bdaddr_t *dst,
					GIOFunc function, gpointer user_data)
{
	struct bluetooth_session *session = user_data;
	sdp_session_t *sdp;
	GIOChannel *io;

	sdp = sdp_connect(src, dst, SDP_NON_BLOCKING);
	if (sdp == NULL)
		return NULL;

	io = g_io_channel_unix_new(sdp_get_socket(sdp));
	if (io == NULL) {
		sdp_close(sdp);
		return NULL;
	}

	g_io_add_watch(io, G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
							function, user_data);

	session->io = io;

	return sdp;
}

static int session_connect(struct bluetooth_session *session)
{
	int err;

	if (session->port > 0) {
		session->io = rfcomm_connect(&session->src, &session->dst,
							session->port,
							rfcomm_callback,
							session);
		err = (session->io == NULL) ? -EINVAL : 0;
	} else {
		session->sdp = service_connect(&session->src, &session->dst,
						service_callback, session);
		err = (session->sdp == NULL) ? -ENOMEM : 0;
	}

	return err;
}

static struct pending_req *find_session_request(
				const struct bluetooth_session *session,
				const DBusPendingCall *call)
{
	GSList *l;

	for (l = session->pending_calls; l; l = l->next) {
		struct pending_req *req = l->data;

		if (req->call == call)
			return req;
	}

	return NULL;
}

static void adapter_reply(DBusPendingCall *call, void *user_data)
{
	DBusError err;
	DBusMessage *reply;
	struct bluetooth_session *session = user_data;
	struct pending_req *req = find_session_request(session, call);
	GError *gerr = NULL;

	reply = dbus_pending_call_steal_reply(call);

	session->pending_calls = g_slist_remove(session->pending_calls, req);
	pending_req_finalize(req);

	dbus_error_init(&err);
	if (dbus_set_error_from_message(&err, reply)) {
		error("manager replied with an error: %s, %s",
				err.name, err.message);
		dbus_error_free(&err);

		goto failed;
	}

	if (session_connect(session) < 0)
		goto failed;

	goto proceed;

failed:
	g_set_error(&gerr, OBC_BT_ERROR, -EINVAL,
					"Unable to request session");
	if (session->func)
		session->func(session->io, gerr, session->user_data);
	g_clear_error(&gerr);

	session_destroy(session);

proceed:
	dbus_message_unref(reply);
}

static void manager_reply(DBusPendingCall *call, void *user_data)
{
	DBusError err;
	DBusMessage *reply;
	char *adapter;
	struct bluetooth_session *session = user_data;
	struct pending_req *req = find_session_request(session, call);
	GError *gerr = NULL;

	reply = dbus_pending_call_steal_reply(call);

	session->pending_calls = g_slist_remove(session->pending_calls, req);
	pending_req_finalize(req);

	dbus_error_init(&err);
	if (dbus_set_error_from_message(&err, reply)) {
		error("manager replied with an error: %s, %s",
				err.name, err.message);
		dbus_error_free(&err);

		goto failed;
	}

	if (dbus_message_get_args(reply, NULL,
				DBUS_TYPE_OBJECT_PATH, &adapter,
				DBUS_TYPE_INVALID)) {
		DBG("adapter path %s", adapter);

		session->adapter = g_strdup(adapter);
		req = send_method_call(session->conn_system,
					BT_BUS_NAME, adapter,
					BT_ADAPTER_IFACE, "RequestSession",
					adapter_reply, session,
					DBUS_TYPE_INVALID);
		if (!req)
			goto failed;

		session->pending_calls = g_slist_prepend(session->pending_calls,
									req);
	} else
		goto failed;

	goto proceed;

failed:
	g_set_error(&gerr, OBC_BT_ERROR, -EINVAL, "No adapter found");
	if (session->func)
		session->func(session->io, gerr, session->user_data);
	g_clear_error(&gerr);

	session_destroy(session);

proceed:
	dbus_message_unref(reply);
}

static guint bluetooth_connect(const char *source, const char *destination,
				const char *service, uint16_t port,
				obc_transport_func func, void *user_data)
{
	struct bluetooth_session *session;
	struct pending_req *req;
	static guint id = 0;

	DBG("");

	if (destination == NULL)
		return 0;

	session = g_try_malloc0(sizeof(*session));
	if (session == NULL)
		return 0;

	session->id = ++id;
	session->func = func;
	session->user_data = user_data;

	session->conn_system = g_dbus_setup_bus(DBUS_BUS_SYSTEM, NULL, NULL);
	if (session->conn_system == NULL) {
		g_free(session);
		return 0;
	}

	session->service = g_strdup(service);
	str2ba(destination, &session->dst);

	if (source == NULL) {
		bacpy(&session->src, BDADDR_ANY);
		req = send_method_call(session->conn_system,
				BT_BUS_NAME, BT_PATH,
				BT_MANAGER_IFACE, "DefaultAdapter",
				manager_reply, session,
				DBUS_TYPE_INVALID);
	} else {
		str2ba(source, &session->src);
		req = send_method_call(session->conn_system,
				BT_BUS_NAME, BT_PATH,
				BT_MANAGER_IFACE, "FindAdapter",
				manager_reply, session,
				DBUS_TYPE_STRING, &source,
				DBUS_TYPE_INVALID);
	}

	if (req == NULL) {
		g_free(session);
		return 0;
	}

	session->pending_calls = g_slist_prepend(session->pending_calls, req);
	sessions = g_slist_prepend(sessions, session);

	return session->id;
}

static void bluetooth_disconnect(guint id)
{
	GSList *l;

	DBG("");

	for (l = sessions; l; l = l->next) {
		struct bluetooth_session *session = l->data;

		if (session->id == id) {
			session_destroy(session);
			return;
		}
	}
}

static struct obc_transport bluetooth = {
	.name = "Bluetooth",
	.connect = bluetooth_connect,
	.disconnect = bluetooth_disconnect,
};

int bluetooth_init(void)
{
	DBG("");

	return obc_transport_register(&bluetooth);
}

void bluetooth_exit(void)
{
	DBG("");

	obc_transport_unregister(&bluetooth);
}
