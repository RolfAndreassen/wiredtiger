/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "thread.h"

static void
file_create(void)
{
	WT_SESSION *session;
	int ret;
	char *p, *end, config[128];

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	p = config;
	end = config + sizeof(config);
	p += snprintf(p, (size_t)(end - p),
	    "key_format=%s,"
	    "internal_page_max=%d,"
	    "leaf_page_max=%d,",
	    ftype == ROW ? "u" : "r", 16 * 1024, 128 * 1024);
	if (ftype == FIX)
		(void)snprintf(p, (size_t)(end - p), ",value_format=3t");

	if ((ret = session->create(session, FNAME, config)) != 0)
		if (ret != EEXIST)
			die("session.create", ret);

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);
}

void
load(void)
{
	WT_CURSOR *cursor;
	WT_ITEM *key, _key, *value, _value;
	WT_SESSION *session;
	char keybuf[64], valuebuf[64];
	u_int keyno;
	int ret;

	file_create();

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	if ((ret =
	    session->open_cursor(session, FNAME, NULL, "bulk", &cursor)) != 0)
		die("cursor.open", ret);

	key = &_key;
	value = &_value;
	for (keyno = 1; keyno <= nkeys; ++keyno) {
		if (ftype == ROW) {
			key->data = keybuf;
			key->size = (uint32_t)
			    snprintf(keybuf, sizeof(keybuf), "%017u", keyno);
			cursor->set_key(cursor, key);
		} else
			cursor->set_key(cursor, (uint32_t)keyno);
		value->data = valuebuf;
		if (ftype == FIX)
			cursor->set_value(cursor, 0x01);
		else {
			value->size = (uint32_t)
			    snprintf(valuebuf, sizeof(valuebuf), "%37u", keyno);
			cursor->set_value(cursor, value);
		}
		if ((ret = cursor->insert(cursor)) != 0)
			die("cursor.insert", ret);
	}

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);
}
