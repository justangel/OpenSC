/*
 * card-setec.c: Support for PKI cards by Setec
 *
 * Copyright (C) 2001  Juha Yrj�l� <juha.yrjola@iki.fi>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sc-internal.h"
#include "sc-log.h"

static const char *setec_atrs[] = {
	/* the current FINEID card has this ATR: */
	"3B:9F:94:40:1E:00:67:11:43:46:49:53:45:10:52:66:FF:81:90:00",
	/* this is from a Nokia branded SC */
	"3B:1F:11:00:67:80:42:46:49:53:45:10:52:66:FF:81:90:00",
	/* RSA SecurID 3100 */
	"3B:9F:94:40:1E:00:67:16:43:46:49:53:45:10:52:66:FF:81:90:00",
	NULL
};

static struct sc_card_operations setec_ops;
static const struct sc_card_driver setec_drv = {
	NULL,
	"Setec smartcards",
	"setec",
	&setec_ops
};

static int setec_finish(struct sc_card *card)
{
	return 0;
}

static int setec_match_card(struct sc_card *card)
{
	int i, match = -1;

	for (i = 0; setec_atrs[i] != NULL; i++) {
		u8 defatr[SC_MAX_ATR_SIZE];
		size_t len = sizeof(defatr);
		const char *atrp = setec_atrs[i];

		if (sc_hex_to_bin(atrp, defatr, &len))
			continue;
		if (len != card->atr_len)
			continue;
		if (memcmp(card->atr, defatr, len) != 0)
			continue;
		match = i;
		break;
	}
	if (match == -1)
		return 0;

	return 1;
}

static int setec_init(struct sc_card *card)
{
	card->ops_data = NULL;
	card->cla = 0x00;

	return 0;
}

static const struct sc_card_operations *iso_ops = NULL;

static u8 acl_to_byte(const struct sc_acl_entry *e)
{
	switch (e->method) {
	case SC_AC_NONE:
		return 0x00;
	case SC_AC_CHV:
		switch (e->key_ref) {
		case 1:
			return 0x01;
			break;
		case 2:
			return 0x02;
			break;
		default:
			return 0x00;
		}
		break;
	case SC_AC_TERM:
		return 0x04;
	case SC_AC_NEVER:
		return 0x0F;
	}
	return 0x00;
}

static int setec_create_file(struct sc_card *card, struct sc_file *file)
{
	if (file->prop_attr_len == 0) {
		memcpy(file->prop_attr, "\x03\x00\x00", 3);
		file->prop_attr_len = 3;
	}
	if (file->sec_attr_len == 0) {
		int idx[6], i;
		u8 buf[6];

		if (file->type == SC_FILE_TYPE_DF) {
			const int df_idx[6] = {
				SC_AC_OP_SELECT, SC_AC_OP_LOCK, SC_AC_OP_DELETE,
				SC_AC_OP_CREATE, SC_AC_OP_REHABILITATE,
				SC_AC_OP_INVALIDATE
			};
			for (i = 0; i < 6; i++)
				idx[i] = df_idx[i];
		} else {
			const int ef_idx[6] = {
				SC_AC_OP_READ, SC_AC_OP_UPDATE, SC_AC_OP_WRITE,
				SC_AC_OP_ERASE, SC_AC_OP_REHABILITATE,
				SC_AC_OP_INVALIDATE
			};
			for (i = 0; i < 6; i++)
				idx[i] = ef_idx[i];
		}
		for (i = 0; i < 6; i++)
			buf[i] = acl_to_byte(file->acl[idx[i]]);

		memcpy(file->sec_attr, buf, 6);
		file->sec_attr_len = 6;
	}

	return iso_ops->create_file(card, file);
}

static int setec_set_security_env(struct sc_card *card,
				  const struct sc_security_env *env,
				  int se_num)
{
	if (env->flags & SC_SEC_ENV_ALG_PRESENT) {
		struct sc_security_env tmp;

		tmp = *env;
                tmp.flags &= ~SC_SEC_ENV_ALG_PRESENT;
		tmp.flags |= SC_SEC_ENV_ALG_REF_PRESENT;
		if (tmp.algorithm != SC_ALGORITHM_RSA) {
			error(card->ctx, "Only RSA algorithm supported.\n");
			return SC_ERROR_NOT_SUPPORTED;
		}
                tmp.algorithm_ref = 0x00;
		if (tmp.algorithm_flags & SC_ALGORITHM_RSA_PKCS1_PAD)
			tmp.algorithm_ref = 0x02;
		if (tmp.algorithm_flags & SC_ALGORITHM_RSA_HASH_SHA1)
                        tmp.algorithm_ref |= 0x10;
                return iso_ops->set_security_env(card, &tmp, se_num);

	}
        return iso_ops->set_security_env(card, env, se_num);
}

static void add_acl_entry(struct sc_file *file, int op, u8 byte)
{
	unsigned int method, key_ref = SC_AC_KEY_REF_NONE;

	switch (byte >> 4) {
	case 0:
		method = SC_AC_NONE;
		break;
	case 1:
		method = SC_AC_CHV;
		key_ref = 1;
		break;
	case 2:
		method = SC_AC_CHV;
		key_ref = 2;
		break;
	case 4:
		method = SC_AC_TERM;
		break;
	case 15:
		method = SC_AC_NEVER;
		break;
	default:
		method = SC_AC_UNKNOWN;
		break;
	}
	sc_file_add_acl_entry(file, op, method, key_ref);
}

static void parse_sec_attr(struct sc_file *file, const u8 *buf, size_t len)
{
	int i;
	int idx[6];

	if (len < 6)
		return;
	if (file->type == SC_FILE_TYPE_DF) {
		const int df_idx[6] = {
			SC_AC_OP_SELECT, SC_AC_OP_LOCK, SC_AC_OP_DELETE,
			SC_AC_OP_CREATE, SC_AC_OP_REHABILITATE,
			SC_AC_OP_INVALIDATE
		};
		for (i = 0; i < 6; i++)
			idx[i] = df_idx[i];
	} else {
		const int ef_idx[6] = {
			SC_AC_OP_READ, SC_AC_OP_UPDATE, SC_AC_OP_WRITE,
			SC_AC_OP_ERASE, SC_AC_OP_REHABILITATE,
			SC_AC_OP_INVALIDATE
		};
		for (i = 0; i < 6; i++)
			idx[i] = ef_idx[i];
	}
	for (i = 0; i < 6; i++)
		add_acl_entry(file, idx[i], buf[i]);
}

static int setec_select_file(struct sc_card *card,
			       const struct sc_path *in_path,
			       struct sc_file **file)
{
	int r;
	
	r = iso_ops->select_file(card, in_path, file);
	if (r)
		return r;
	if (file != NULL)
		parse_sec_attr(*file, (*file)->sec_attr, (*file)->sec_attr_len);
	return 0;
}

static const struct sc_card_driver * sc_get_driver(void)
{
	const struct sc_card_driver *iso_drv = sc_get_iso7816_driver();

	setec_ops = *iso_drv->ops;
	setec_ops.match_card = setec_match_card;
	setec_ops.init = setec_init;
        setec_ops.finish = setec_finish;
	if (iso_ops == NULL)
                iso_ops = iso_drv->ops;
	setec_ops.create_file = setec_create_file;
	setec_ops.set_security_env = setec_set_security_env;
	setec_ops.select_file = setec_select_file;
	
        return &setec_drv;
}

#if 1
const struct sc_card_driver * sc_get_setec_driver(void)
{
	return sc_get_driver();
}
#endif
