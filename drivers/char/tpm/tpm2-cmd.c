/*
 * Copyright (C) 2014, 2015 Intel Corporation
 *
 * Authors:
 * Jarkko Sakkinen <jarkko.sakkinen@linux.intel.com>
 *
 * Maintained by: <tpmdd-devel@lists.sourceforge.net>
 *
 * This file contains TPM2 protocol implementations of the commands
 * used by the kernel internally.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include "tpm.h"
#include <crypto/hash_info.h>
#include <keys/trusted-type.h>

enum tpm2_object_attributes {
	TPM2_OA_USER_WITH_AUTH		= BIT(6),
};

enum tpm2_session_attributes {
	TPM2_SA_CONTINUE_SESSION	= BIT(0),
};

struct tpm2_hash {
	unsigned int crypto_id;
	unsigned int tpm_id;
};

static struct tpm2_hash tpm2_hash_map[] = {
	{HASH_ALGO_SHA1, TPM2_ALG_SHA1},
	{HASH_ALGO_SHA256, TPM2_ALG_SHA256},
	{HASH_ALGO_SHA384, TPM2_ALG_SHA384},
	{HASH_ALGO_SHA512, TPM2_ALG_SHA512},
	{HASH_ALGO_SM3_256, TPM2_ALG_SM3_256},
};

/*
 * Array with one entry per ordinal defining the maximum amount
 * of time the chip could take to return the result. The values
 * of the SHORT, MEDIUM, and LONG durations are taken from the
 * PC Client Profile (PTP) specification.
 * LONG_LONG is for commands that generates keys which empirically
 * takes longer time on some systems.
 */
static const u8 tpm2_ordinal_duration[TPM2_CC_LAST - TPM2_CC_FIRST + 1] = {
	TPM_UNDEFINED,		/* 11F */
	TPM_UNDEFINED,		/* 120 */
	TPM_LONG,		/* 121 */
	TPM_UNDEFINED,		/* 122 */
	TPM_UNDEFINED,		/* 123 */
	TPM_UNDEFINED,		/* 124 */
	TPM_UNDEFINED,		/* 125 */
	TPM_UNDEFINED,		/* 126 */
	TPM_UNDEFINED,		/* 127 */
	TPM_UNDEFINED,		/* 128 */
	TPM_LONG,		/* 129 */
	TPM_UNDEFINED,		/* 12a */
	TPM_UNDEFINED,		/* 12b */
	TPM_UNDEFINED,		/* 12c */
	TPM_UNDEFINED,		/* 12d */
	TPM_UNDEFINED,		/* 12e */
	TPM_UNDEFINED,		/* 12f */
	TPM_UNDEFINED,		/* 130 */
	TPM_LONG_LONG,		/* 131 */
	TPM_UNDEFINED,		/* 132 */
	TPM_UNDEFINED,		/* 133 */
	TPM_UNDEFINED,		/* 134 */
	TPM_UNDEFINED,		/* 135 */
	TPM_UNDEFINED,		/* 136 */
	TPM_UNDEFINED,		/* 137 */
	TPM_UNDEFINED,		/* 138 */
	TPM_UNDEFINED,		/* 139 */
	TPM_UNDEFINED,		/* 13a */
	TPM_UNDEFINED,		/* 13b */
	TPM_UNDEFINED,		/* 13c */
	TPM_UNDEFINED,		/* 13d */
	TPM_MEDIUM,		/* 13e */
	TPM_UNDEFINED,		/* 13f */
	TPM_UNDEFINED,		/* 140 */
	TPM_UNDEFINED,		/* 141 */
	TPM_UNDEFINED,		/* 142 */
	TPM_LONG,		/* 143 */
	TPM_MEDIUM,		/* 144 */
	TPM_UNDEFINED,		/* 145 */
	TPM_UNDEFINED,		/* 146 */
	TPM_UNDEFINED,		/* 147 */
	TPM_UNDEFINED,		/* 148 */
	TPM_UNDEFINED,		/* 149 */
	TPM_UNDEFINED,		/* 14a */
	TPM_UNDEFINED,		/* 14b */
	TPM_UNDEFINED,		/* 14c */
	TPM_UNDEFINED,		/* 14d */
	TPM_LONG,		/* 14e */
	TPM_UNDEFINED,		/* 14f */
	TPM_UNDEFINED,		/* 150 */
	TPM_UNDEFINED,		/* 151 */
	TPM_UNDEFINED,		/* 152 */
	TPM_LONG_LONG,		/* 153 */
	TPM_UNDEFINED,		/* 154 */
	TPM_UNDEFINED,		/* 155 */
	TPM_UNDEFINED,		/* 156 */
	TPM_UNDEFINED,		/* 157 */
	TPM_UNDEFINED,		/* 158 */
	TPM_UNDEFINED,		/* 159 */
	TPM_UNDEFINED,		/* 15a */
	TPM_UNDEFINED,		/* 15b */
	TPM_MEDIUM,		/* 15c */
	TPM_UNDEFINED,		/* 15d */
	TPM_UNDEFINED,		/* 15e */
	TPM_UNDEFINED,		/* 15f */
	TPM_UNDEFINED,		/* 160 */
	TPM_UNDEFINED,		/* 161 */
	TPM_UNDEFINED,		/* 162 */
	TPM_UNDEFINED,		/* 163 */
	TPM_UNDEFINED,		/* 164 */
	TPM_UNDEFINED,		/* 165 */
	TPM_UNDEFINED,		/* 166 */
	TPM_UNDEFINED,		/* 167 */
	TPM_UNDEFINED,		/* 168 */
	TPM_UNDEFINED,		/* 169 */
	TPM_UNDEFINED,		/* 16a */
	TPM_UNDEFINED,		/* 16b */
	TPM_UNDEFINED,		/* 16c */
	TPM_UNDEFINED,		/* 16d */
	TPM_UNDEFINED,		/* 16e */
	TPM_UNDEFINED,		/* 16f */
	TPM_UNDEFINED,		/* 170 */
	TPM_UNDEFINED,		/* 171 */
	TPM_UNDEFINED,		/* 172 */
	TPM_UNDEFINED,		/* 173 */
	TPM_UNDEFINED,		/* 174 */
	TPM_UNDEFINED,		/* 175 */
	TPM_UNDEFINED,		/* 176 */
	TPM_LONG,		/* 177 */
	TPM_UNDEFINED,		/* 178 */
	TPM_UNDEFINED,		/* 179 */
	TPM_MEDIUM,		/* 17a */
	TPM_LONG,		/* 17b */
	TPM_UNDEFINED,		/* 17c */
	TPM_UNDEFINED,		/* 17d */
	TPM_UNDEFINED,		/* 17e */
	TPM_UNDEFINED,		/* 17f */
	TPM_UNDEFINED,		/* 180 */
	TPM_UNDEFINED,		/* 181 */
	TPM_MEDIUM,		/* 182 */
	TPM_UNDEFINED,		/* 183 */
	TPM_UNDEFINED,		/* 184 */
	TPM_MEDIUM,		/* 185 */
	TPM_MEDIUM,		/* 186 */
	TPM_UNDEFINED,		/* 187 */
	TPM_UNDEFINED,		/* 188 */
	TPM_UNDEFINED,		/* 189 */
	TPM_UNDEFINED,		/* 18a */
	TPM_UNDEFINED,		/* 18b */
	TPM_UNDEFINED,		/* 18c */
	TPM_UNDEFINED,		/* 18d */
	TPM_UNDEFINED,		/* 18e */
	TPM_UNDEFINED		/* 18f */
};

struct tpm2_pcr_read_out {
	__be32	update_cnt;
	__be32	pcr_selects_cnt;
	__be16	hash_alg;
	u8	pcr_select_size;
	u8	pcr_select[TPM2_PCR_SELECT_MIN];
	__be32	digests_cnt;
	__be16	digest_size;
	u8	digest[];
} __packed;

/**
 * tpm2_pcr_read() - read a PCR value
 * @chip:	TPM chip to use.
 * @pcr_idx:	index of the PCR to read.
 * @res_buf:	buffer to store the resulting hash.
 *
 * Return: Same as with tpm_transmit_cmd.
 */
int tpm2_pcr_read(struct tpm_chip *chip, int pcr_idx, u8 *res_buf)
{
	int rc;
	struct tpm_buf buf;
	struct tpm2_pcr_read_out *out;
	u8 pcr_select[TPM2_PCR_SELECT_MIN] = {0};

	if (pcr_idx >= TPM2_PLATFORM_PCR)
		return -EINVAL;

	rc = tpm_buf_init(&buf, TPM2_ST_NO_SESSIONS, TPM2_CC_PCR_READ);
	if (rc)
		return rc;

	pcr_select[pcr_idx >> 3] = 1 << (pcr_idx & 0x7);

	tpm_buf_append_u32(&buf, 1);
	tpm_buf_append_u16(&buf, TPM2_ALG_SHA1);
	tpm_buf_append_u8(&buf, TPM2_PCR_SELECT_MIN);
	tpm_buf_append(&buf, (const unsigned char *)pcr_select,
		       sizeof(pcr_select));

	rc = tpm_transmit_cmd(chip, NULL, buf.data, PAGE_SIZE, 0, 0,
			res_buf ? "attempting to read a pcr value" : NULL);
	if (rc == 0 && res_buf) {
		out = (struct tpm2_pcr_read_out *)&buf.data[TPM_HEADER_SIZE];
		memcpy(res_buf, out->digest, SHA1_DIGEST_SIZE);
	}

	tpm_buf_destroy(&buf);
	return rc;
}

struct tpm2_null_auth_area {
	__be32  handle;
	__be16  nonce_size;
	u8  attributes;
	__be16  auth_size;
} __packed;

/**
 * tpm2_pcr_extend() - extend a PCR value
 *
 * @chip:	TPM chip to use.
 * @pcr_idx:	index of the PCR.
 * @count:	number of digests passed.
 * @digests:	list of pcr banks and corresponding digest values to extend.
 *
 * Return: Same as with tpm_transmit_cmd.
 */
int tpm2_pcr_extend(struct tpm_chip *chip, int pcr_idx, u32 count,
		    struct tpm2_digest *digests)
{
	struct tpm_buf buf;
	struct tpm2_null_auth_area auth_area;
	int rc;
	int i;
	int j;

	if (count > ARRAY_SIZE(chip->active_banks))
		return -EINVAL;

	rc = tpm_buf_init(&buf, TPM2_ST_SESSIONS, TPM2_CC_PCR_EXTEND);
	if (rc)
		return rc;

	tpm_buf_append_u32(&buf, pcr_idx);

	auth_area.handle = cpu_to_be32(TPM2_RS_PW);
	auth_area.nonce_size = 0;
	auth_area.attributes = 0;
	auth_area.auth_size = 0;

	tpm_buf_append_u32(&buf, sizeof(struct tpm2_null_auth_area));
	tpm_buf_append(&buf, (const unsigned char *)&auth_area,
		       sizeof(auth_area));
	tpm_buf_append_u32(&buf, count);

	for (i = 0; i < count; i++) {
		for (j = 0; j < ARRAY_SIZE(tpm2_hash_map); j++) {
			if (digests[i].alg_id != tpm2_hash_map[j].tpm_id)
				continue;
			tpm_buf_append_u16(&buf, digests[i].alg_id);
			tpm_buf_append(&buf, (const unsigned char
					      *)&digests[i].digest,
			       hash_digest_size[tpm2_hash_map[j].crypto_id]);
		}
	}

	rc = tpm_transmit_cmd(chip, NULL, buf.data, PAGE_SIZE, 0, 0,
			      "attempting extend a PCR value");

	tpm_buf_destroy(&buf);

	return rc;
}


struct tpm2_get_random_out {
	__be16 size;
	u8 buffer[TPM_MAX_RNG_DATA];
} __packed;

/**
 * tpm2_get_random() - get random bytes from the TPM RNG
 *
 * @chip:	a &tpm_chip instance
 * @dest:	destination buffer
 * @max:	the max number of random bytes to pull
 *
 * Return:
 *   size of the buffer on success,
 *   -errno otherwise
 */
int tpm2_get_random(struct tpm_chip *chip, u8 *dest, size_t max)
{
	struct tpm2_get_random_out *out;
	struct tpm_buf buf;
	u32 recd;
	u32 num_bytes = max;
	int err;
	int total = 0;
	int retries = 5;
	u8 *dest_ptr = dest;

	if (!num_bytes || max > TPM_MAX_RNG_DATA)
		return -EINVAL;

	err = tpm_buf_init(&buf, 0, 0);
	if (err)
		return err;

	do {
		tpm_buf_reset(&buf, TPM2_ST_NO_SESSIONS, TPM2_CC_GET_RANDOM);
		tpm_buf_append_u16(&buf, num_bytes);
		err = tpm_transmit_cmd(chip, NULL, buf.data, PAGE_SIZE,
				       offsetof(struct tpm2_get_random_out,
						buffer),
				       0, "attempting get random");
		if (err)
			goto out;

		out = (struct tpm2_get_random_out *)
			&buf.data[TPM_HEADER_SIZE];
		recd = min_t(u32, be16_to_cpu(out->size), num_bytes);
		if (tpm_buf_length(&buf) <
		    TPM_HEADER_SIZE +
		    offsetof(struct tpm2_get_random_out, buffer) +
		    recd) {
			err = -EFAULT;
			goto out;
		}
		memcpy(dest_ptr, out->buffer, recd);

		dest_ptr += recd;
		total += recd;
		num_bytes -= recd;
	} while (retries-- && total < max);

	tpm_buf_destroy(&buf);
	return total ? total : -EIO;
out:
	tpm_buf_destroy(&buf);
	return err;
}

/**
 * tpm2_flush_context_cmd() - execute a TPM2_FlushContext command
 * @chip: TPM chip to use
 * @payload: the key data in clear and encrypted form
 * @options: authentication values and other options
 *
 * Return: same as with tpm_transmit_cmd
 */
void tpm2_flush_context_cmd(struct tpm_chip *chip, u32 handle,
			    unsigned int flags)
{
	struct tpm_buf buf;
	int rc;

	rc = tpm_buf_init(&buf, TPM2_ST_NO_SESSIONS, TPM2_CC_FLUSH_CONTEXT);
	if (rc) {
		dev_warn(&chip->dev, "0x%08x was not flushed, out of memory\n",
			 handle);
		return;
	}

	tpm_buf_append_u32(&buf, handle);

	(void) tpm_transmit_cmd(chip, NULL, buf.data, PAGE_SIZE, 0, flags,
				"flushing context");

	tpm_buf_destroy(&buf);
}

/**
 * tpm_buf_append_auth() - append TPMS_AUTH_COMMAND to the buffer.
 *
 * @buf: an allocated tpm_buf instance
 * @session_handle: session handle
 * @nonce: the session nonce, may be NULL if not used
 * @nonce_len: the session nonce length, may be 0 if not used
 * @attributes: the session attributes
 * @hmac: the session HMAC or password, may be NULL if not used
 * @hmac_len: the session HMAC or password length, maybe 0 if not used
 */
static void tpm2_buf_append_auth(struct tpm_buf *buf, u32 session_handle,
				 const u8 *nonce, u16 nonce_len,
				 u8 attributes,
				 const u8 *hmac, u16 hmac_len)
{
	tpm_buf_append_u32(buf, 9 + nonce_len + hmac_len);
	tpm_buf_append_u32(buf, session_handle);
	tpm_buf_append_u16(buf, nonce_len);

	if (nonce && nonce_len)
		tpm_buf_append(buf, nonce, nonce_len);

	tpm_buf_append_u8(buf, attributes);
	tpm_buf_append_u16(buf, hmac_len);

	if (hmac && hmac_len)
		tpm_buf_append(buf, hmac, hmac_len);
}

/**
 * tpm2_seal_trusted() - seal the payload of a trusted key
 *
 * @chip: TPM chip to use
 * @payload: the key data in clear and encrypted form
 * @options: authentication values and other options
 *
 * Return: < 0 on error and 0 on success.
 */
int tpm2_seal_trusted(struct tpm_chip *chip,
		      struct trusted_key_payload *payload,
		      struct trusted_key_options *options)
{
	unsigned int blob_len;
	struct tpm_buf buf;
	u32 hash;
	int i;
	int rc;

	for (i = 0; i < ARRAY_SIZE(tpm2_hash_map); i++) {
		if (options->hash == tpm2_hash_map[i].crypto_id) {
			hash = tpm2_hash_map[i].tpm_id;
			break;
		}
	}

	if (i == ARRAY_SIZE(tpm2_hash_map))
		return -EINVAL;

	rc = tpm_buf_init(&buf, TPM2_ST_SESSIONS, TPM2_CC_CREATE);
	if (rc)
		return rc;

	tpm_buf_append_u32(&buf, options->keyhandle);
	tpm2_buf_append_auth(&buf, TPM2_RS_PW,
			     NULL /* nonce */, 0,
			     0 /* session_attributes */,
			     options->keyauth /* hmac */,
			     TPM_DIGEST_SIZE);

	/* sensitive */
	tpm_buf_append_u16(&buf, 4 + TPM_DIGEST_SIZE + payload->key_len + 1);

	tpm_buf_append_u16(&buf, TPM_DIGEST_SIZE);
	tpm_buf_append(&buf, options->blobauth, TPM_DIGEST_SIZE);
	tpm_buf_append_u16(&buf, payload->key_len + 1);
	tpm_buf_append(&buf, payload->key, payload->key_len);
	tpm_buf_append_u8(&buf, payload->migratable);

	/* public */
	tpm_buf_append_u16(&buf, 14 + options->policydigest_len);
	tpm_buf_append_u16(&buf, TPM2_ALG_KEYEDHASH);
	tpm_buf_append_u16(&buf, hash);

	/* policy */
	if (options->policydigest_len) {
		tpm_buf_append_u32(&buf, 0);
		tpm_buf_append_u16(&buf, options->policydigest_len);
		tpm_buf_append(&buf, options->policydigest,
			       options->policydigest_len);
	} else {
		tpm_buf_append_u32(&buf, TPM2_OA_USER_WITH_AUTH);
		tpm_buf_append_u16(&buf, 0);
	}

	/* public parameters */
	tpm_buf_append_u16(&buf, TPM2_ALG_NULL);
	tpm_buf_append_u16(&buf, 0);

	/* outside info */
	tpm_buf_append_u16(&buf, 0);

	/* creation PCR */
	tpm_buf_append_u32(&buf, 0);

	if (buf.flags & TPM_BUF_OVERFLOW) {
		rc = -E2BIG;
		goto out;
	}

	rc = tpm_transmit_cmd(chip, NULL, buf.data, PAGE_SIZE, 4, 0,
			      "sealing data");
	if (rc)
		goto out;

	blob_len = be32_to_cpup((__be32 *) &buf.data[TPM_HEADER_SIZE]);
	if (blob_len > MAX_BLOB_SIZE) {
		rc = -E2BIG;
		goto out;
	}
	if (tpm_buf_length(&buf) < TPM_HEADER_SIZE + 4 + blob_len) {
		rc = -EFAULT;
		goto out;
	}

	memcpy(payload->blob, &buf.data[TPM_HEADER_SIZE + 4], blob_len);
	payload->blob_len = blob_len;

out:
	tpm_buf_destroy(&buf);

	if (rc > 0) {
		if (tpm2_rc_value(rc) == TPM2_RC_HASH)
			rc = -EINVAL;
		else
			rc = -EPERM;
	}

	return rc;
}

/**
 * tpm2_load_cmd() - execute a TPM2_Load command
 *
 * @chip: TPM chip to use
 * @payload: the key data in clear and encrypted form
 * @options: authentication values and other options
 * @blob_handle: returned blob handle
 * @flags: tpm transmit flags
 *
 * Return: 0 on success.
 *        -E2BIG on wrong payload size.
 *        -EPERM on tpm error status.
 *        < 0 error from tpm_transmit_cmd.
 */
static int tpm2_load_cmd(struct tpm_chip *chip,
			 struct trusted_key_payload *payload,
			 struct trusted_key_options *options,
			 u32 *blob_handle, unsigned int flags)
{
	struct tpm_buf buf;
	unsigned int private_len;
	unsigned int public_len;
	unsigned int blob_len;
	int rc;

	private_len = be16_to_cpup((__be16 *) &payload->blob[0]);
	if (private_len > (payload->blob_len - 2))
		return -E2BIG;

	public_len = be16_to_cpup((__be16 *) &payload->blob[2 + private_len]);
	blob_len = private_len + public_len + 4;
	if (blob_len > payload->blob_len)
		return -E2BIG;

	rc = tpm_buf_init(&buf, TPM2_ST_SESSIONS, TPM2_CC_LOAD);
	if (rc)
		return rc;

	tpm_buf_append_u32(&buf, options->keyhandle);
	tpm2_buf_append_auth(&buf, TPM2_RS_PW,
			     NULL /* nonce */, 0,
			     0 /* session_attributes */,
			     options->keyauth /* hmac */,
			     TPM_DIGEST_SIZE);

	tpm_buf_append(&buf, payload->blob, blob_len);

	if (buf.flags & TPM_BUF_OVERFLOW) {
		rc = -E2BIG;
		goto out;
	}

	rc = tpm_transmit_cmd(chip, NULL, buf.data, PAGE_SIZE, 4, flags,
			      "loading blob");
	if (!rc)
		*blob_handle = be32_to_cpup(
			(__be32 *) &buf.data[TPM_HEADER_SIZE]);

out:
	tpm_buf_destroy(&buf);

	if (rc > 0)
		rc = -EPERM;

	return rc;
}

/**
 * tpm2_unseal_cmd() - execute a TPM2_Unload command
 *
 * @chip: TPM chip to use
 * @payload: the key data in clear and encrypted form
 * @options: authentication values and other options
 * @blob_handle: blob handle
 * @flags: tpm_transmit_cmd flags
 *
 * Return: 0 on success
 *         -EPERM on tpm error status
 *         < 0 error from tpm_transmit_cmd
 */
static int tpm2_unseal_cmd(struct tpm_chip *chip,
			   struct trusted_key_payload *payload,
			   struct trusted_key_options *options,
			   u32 blob_handle, unsigned int flags)
{
	struct tpm_buf buf;
	u16 data_len;
	u8 *data;
	int rc;

	rc = tpm_buf_init(&buf, TPM2_ST_SESSIONS, TPM2_CC_UNSEAL);
	if (rc)
		return rc;

	tpm_buf_append_u32(&buf, blob_handle);
	tpm2_buf_append_auth(&buf,
			     options->policyhandle ?
			     options->policyhandle : TPM2_RS_PW,
			     NULL /* nonce */, 0,
			     TPM2_SA_CONTINUE_SESSION,
			     options->blobauth /* hmac */,
			     TPM_DIGEST_SIZE);

	rc = tpm_transmit_cmd(chip, NULL, buf.data, PAGE_SIZE, 6, flags,
			      "unsealing");
	if (rc > 0)
		rc = -EPERM;

	if (!rc) {
		data_len = be16_to_cpup(
			(__be16 *) &buf.data[TPM_HEADER_SIZE + 4]);
		if (data_len < MIN_KEY_SIZE ||  data_len > MAX_KEY_SIZE + 1) {
			rc = -EFAULT;
			goto out;
		}

		if (tpm_buf_length(&buf) < TPM_HEADER_SIZE + 6 + data_len) {
			rc = -EFAULT;
			goto out;
		}
		data = &buf.data[TPM_HEADER_SIZE + 6];

		memcpy(payload->key, data, data_len - 1);
		payload->key_len = data_len - 1;
		payload->migratable = data[data_len - 1];
	}

out:
	tpm_buf_destroy(&buf);
	return rc;
}

/**
 * tpm2_unseal_trusted() - unseal the payload of a trusted key
 *
 * @chip: TPM chip to use
 * @payload: the key data in clear and encrypted form
 * @options: authentication values and other options
 *
 * Return: Same as with tpm_transmit_cmd.
 */
int tpm2_unseal_trusted(struct tpm_chip *chip,
			struct trusted_key_payload *payload,
			struct trusted_key_options *options)
{
	u32 blob_handle;
	int rc;

	mutex_lock(&chip->tpm_mutex);
	rc = tpm2_load_cmd(chip, payload, options, &blob_handle,
			   TPM_TRANSMIT_UNLOCKED);
	if (rc)
		goto out;

	rc = tpm2_unseal_cmd(chip, payload, options, blob_handle,
			     TPM_TRANSMIT_UNLOCKED);
	tpm2_flush_context_cmd(chip, blob_handle, TPM_TRANSMIT_UNLOCKED);
out:
	mutex_unlock(&chip->tpm_mutex);
	return rc;
}

struct tpm2_get_cap_out {
	u8 more_data;
	__be32 subcap_id;
	__be32 property_cnt;
	__be32 property_id;
	__be32 value;
} __packed;

/**
 * tpm2_get_tpm_pt() - get value of a TPM_CAP_TPM_PROPERTIES type property
 * @chip:		a &tpm_chip instance
 * @property_id:	property ID.
 * @value:		output variable.
 * @desc:		passed to tpm_transmit_cmd()
 *
 * Return:
 *   0 on success,
 *   -errno or a TPM return code otherwise
 */
ssize_t tpm2_get_tpm_pt(struct tpm_chip *chip, u32 property_id,  u32 *value,
			const char *desc)
{
	struct tpm2_get_cap_out *out;
	struct tpm_buf buf;
	int rc;

	rc = tpm_buf_init(&buf, TPM2_ST_NO_SESSIONS, TPM2_CC_GET_CAPABILITY);
	if (rc)
		return rc;
	tpm_buf_append_u32(&buf, TPM2_CAP_TPM_PROPERTIES);
	tpm_buf_append_u32(&buf, property_id);
	tpm_buf_append_u32(&buf, 1);
	rc = tpm_transmit_cmd(chip, NULL, buf.data, PAGE_SIZE, 0, 0, NULL);
	if (!rc) {
		out = (struct tpm2_get_cap_out *)
			&buf.data[TPM_HEADER_SIZE];
		/*
		 * To prevent failing boot up of some systems, Infineon TPM2.0
		 * returns SUCCESS on TPM2_Startup in field upgrade mode. Also
		 * the TPM2_Getcapability command returns a zero length list
		 * in field upgrade mode.
		 */
		if (be32_to_cpu(out->property_cnt) > 0)
			*value = be32_to_cpu(out->value);
		else
			rc = -ENODATA;
	}
	tpm_buf_destroy(&buf);
	return rc;
}
EXPORT_SYMBOL_GPL(tpm2_get_tpm_pt);

/**
 * tpm2_shutdown() - send a TPM shutdown command
 *
 * Sends a TPM shutdown command. The shutdown command is used in call
 * sites where the system is going down. If it fails, there is not much
 * that can be done except print an error message.
 *
 * @chip:		a &tpm_chip instance
 * @shutdown_type:	TPM_SU_CLEAR or TPM_SU_STATE.
 */
void tpm2_shutdown(struct tpm_chip *chip, u16 shutdown_type)
{
	struct tpm_buf buf;
	int rc;

	rc = tpm_buf_init(&buf, TPM2_ST_NO_SESSIONS, TPM2_CC_SHUTDOWN);
	if (rc)
		return;
	tpm_buf_append_u16(&buf, shutdown_type);
	tpm_transmit_cmd(chip, NULL, buf.data, PAGE_SIZE, 0, 0,
			 "stopping the TPM");
	tpm_buf_destroy(&buf);
}

/*
 * tpm2_calc_ordinal_duration() - maximum duration for a command
 *
 * @chip:	TPM chip to use.
 * @ordinal:	command code number.
 *
 * Return: maximum duration for a command
 */
unsigned long tpm2_calc_ordinal_duration(struct tpm_chip *chip, u32 ordinal)
{
	int index = TPM_UNDEFINED;
	int duration = 0;

	if (ordinal >= TPM2_CC_FIRST && ordinal <= TPM2_CC_LAST)
		index = tpm2_ordinal_duration[ordinal - TPM2_CC_FIRST];

	if (index != TPM_UNDEFINED)
		duration = chip->duration[index];

	if (duration <= 0)
		duration = msecs_to_jiffies(TPM2_DURATION_DEFAULT);

	return duration;
}
EXPORT_SYMBOL_GPL(tpm2_calc_ordinal_duration);

/**
 * tpm2_do_selftest() - ensure that all self tests have passed
 *
 * @chip: TPM chip to use
 *
 * Return: Same as with tpm_transmit_cmd.
 *
 * The TPM can either run all self tests synchronously and then return
 * RC_SUCCESS once all tests were successful. Or it can choose to run the tests
 * asynchronously and return RC_TESTING immediately while the self tests still
 * execute in the background. This function handles both cases and waits until
 * all tests have completed.
 */
static int tpm2_do_selftest(struct tpm_chip *chip)
{
	struct tpm_buf buf;
	int full;
	int rc;

	for (full = 0; full < 2; full++) {
		rc = tpm_buf_init(&buf, TPM2_ST_NO_SESSIONS, TPM2_CC_SELF_TEST);
		if (rc)
			return rc;

		tpm_buf_append_u8(&buf, full);
		rc = tpm_transmit_cmd(chip, NULL, buf.data, PAGE_SIZE, 0, 0,
				      "attempting the self test");
		tpm_buf_destroy(&buf);

		if (rc == TPM2_RC_TESTING)
			rc = TPM2_RC_SUCCESS;
		if (rc == TPM2_RC_INITIALIZE || rc == TPM2_RC_SUCCESS)
			return rc;
	}

	return rc;
}

/**
 * tpm2_probe() - probe for the TPM 2.0 protocol
 * @chip:	a &tpm_chip instance
 *
 * Send an idempotent TPM 2.0 command and see whether there is TPM2 chip in the
 * other end based on the response tag. The flag TPM_CHIP_FLAG_TPM2 is set by
 * this function if this is the case.
 *
 * Return:
 *   0 on success,
 *   -errno otherwise
 */
int tpm2_probe(struct tpm_chip *chip)
{
	struct tpm_output_header *out;
	struct tpm_buf buf;
	int rc;

	rc = tpm_buf_init(&buf, TPM2_ST_NO_SESSIONS, TPM2_CC_GET_CAPABILITY);
	if (rc)
		return rc;
	tpm_buf_append_u32(&buf, TPM2_CAP_TPM_PROPERTIES);
	tpm_buf_append_u32(&buf, TPM_PT_TOTAL_COMMANDS);
	tpm_buf_append_u32(&buf, 1);
	rc = tpm_transmit_cmd(chip, NULL, buf.data, PAGE_SIZE, 0, 0, NULL);
	/* We ignore TPM return codes on purpose. */
	if (rc >=  0) {
		out = (struct tpm_output_header *)buf.data;
		if (be16_to_cpu(out->tag) == TPM2_ST_NO_SESSIONS)
			chip->flags |= TPM_CHIP_FLAG_TPM2;
	}
	tpm_buf_destroy(&buf);
	return 0;
}
EXPORT_SYMBOL_GPL(tpm2_probe);

struct tpm2_pcr_selection {
	__be16  hash_alg;
	u8  size_of_select;
	u8  pcr_select[3];
} __packed;

static ssize_t tpm2_get_pcr_allocation(struct tpm_chip *chip)
{
	struct tpm2_pcr_selection pcr_selection;
	struct tpm_buf buf;
	void *marker;
	void *end;
	void *pcr_select_offset;
	unsigned int count;
	u32 sizeof_pcr_selection;
	u32 rsp_len;
	int rc;
	int i = 0;

	rc = tpm_buf_init(&buf, TPM2_ST_NO_SESSIONS, TPM2_CC_GET_CAPABILITY);
	if (rc)
		return rc;

	tpm_buf_append_u32(&buf, TPM2_CAP_PCRS);
	tpm_buf_append_u32(&buf, 0);
	tpm_buf_append_u32(&buf, 1);

	rc = tpm_transmit_cmd(chip, NULL, buf.data, PAGE_SIZE, 9, 0,
			      "get tpm pcr allocation");
	if (rc)
		goto out;

	count = be32_to_cpup(
		(__be32 *)&buf.data[TPM_HEADER_SIZE + 5]);

	if (count > ARRAY_SIZE(chip->active_banks)) {
		rc = -ENODEV;
		goto out;
	}

	marker = &buf.data[TPM_HEADER_SIZE + 9];

	rsp_len = be32_to_cpup((__be32 *)&buf.data[2]);
	end = &buf.data[rsp_len];

	for (i = 0; i < count; i++) {
		pcr_select_offset = marker +
			offsetof(struct tpm2_pcr_selection, size_of_select);
		if (pcr_select_offset >= end) {
			rc = -EFAULT;
			break;
		}

		memcpy(&pcr_selection, marker, sizeof(pcr_selection));
		chip->active_banks[i] = be16_to_cpu(pcr_selection.hash_alg);
		sizeof_pcr_selection = sizeof(pcr_selection.hash_alg) +
			sizeof(pcr_selection.size_of_select) +
			pcr_selection.size_of_select;
		marker = marker + sizeof_pcr_selection;
	}

out:
	if (i < ARRAY_SIZE(chip->active_banks))
		chip->active_banks[i] = TPM2_ALG_ERROR;

	tpm_buf_destroy(&buf);

	return rc;
}

static int tpm2_get_cc_attrs_tbl(struct tpm_chip *chip)
{
	struct tpm_buf buf;
	u32 nr_commands;
	__be32 *attrs;
	u32 cc;
	int i;
	int rc;

	rc = tpm2_get_tpm_pt(chip, TPM_PT_TOTAL_COMMANDS, &nr_commands, NULL);
	if (rc)
		goto out;

	if (nr_commands > 0xFFFFF) {
		rc = -EFAULT;
		goto out;
	}

	chip->cc_attrs_tbl = devm_kcalloc(&chip->dev, 4, nr_commands,
					  GFP_KERNEL);
	if (!chip->cc_attrs_tbl) {
		rc = -ENOMEM;
		goto out;
	}

	rc = tpm_buf_init(&buf, TPM2_ST_NO_SESSIONS, TPM2_CC_GET_CAPABILITY);
	if (rc)
		goto out;

	tpm_buf_append_u32(&buf, TPM2_CAP_COMMANDS);
	tpm_buf_append_u32(&buf, TPM2_CC_FIRST);
	tpm_buf_append_u32(&buf, nr_commands);

	rc = tpm_transmit_cmd(chip, NULL, buf.data, PAGE_SIZE,
			      9 + 4 * nr_commands, 0, NULL);
	if (rc) {
		tpm_buf_destroy(&buf);
		goto out;
	}

	if (nr_commands !=
	    be32_to_cpup((__be32 *)&buf.data[TPM_HEADER_SIZE + 5])) {
		rc = -EFAULT;
		tpm_buf_destroy(&buf);
		goto out;
	}

	chip->nr_commands = nr_commands;

	attrs = (__be32 *)&buf.data[TPM_HEADER_SIZE + 9];
	for (i = 0; i < nr_commands; i++, attrs++) {
		chip->cc_attrs_tbl[i] = be32_to_cpup(attrs);
		cc = chip->cc_attrs_tbl[i] & 0xFFFF;

		if (cc == TPM2_CC_CONTEXT_SAVE || cc == TPM2_CC_FLUSH_CONTEXT) {
			chip->cc_attrs_tbl[i] &=
				~(GENMASK(2, 0) << TPM2_CC_ATTR_CHANDLES);
			chip->cc_attrs_tbl[i] |= 1 << TPM2_CC_ATTR_CHANDLES;
		}
	}

	tpm_buf_destroy(&buf);

out:
	if (rc > 0)
		rc = -ENODEV;
	return rc;
}

/**
 * tpm2_auto_startup - Perform the standard automatic TPM initialization
 *                     sequence
 * @chip: TPM chip to use
 *
 * Returns 0 on success, < 0 in case of fatal error.
 */
int tpm2_auto_startup(struct tpm_chip *chip)
{
	int rc;

	rc = tpm_get_timeouts(chip);
	if (rc)
		goto out;

	rc = tpm2_do_selftest(chip);
	if (rc && rc != TPM2_RC_INITIALIZE)
		goto out;

	if (rc == TPM2_RC_INITIALIZE) {
		rc = tpm_startup(chip);
		if (rc)
			goto out;

		rc = tpm2_do_selftest(chip);
		if (rc)
			goto out;
	}

	rc = tpm2_get_pcr_allocation(chip);
	if (rc)
		goto out;

	rc = tpm2_get_cc_attrs_tbl(chip);

out:
	if (rc > 0)
		rc = -ENODEV;
	return rc;
}

int tpm2_find_cc(struct tpm_chip *chip, u32 cc)
{
	int i;

	for (i = 0; i < chip->nr_commands; i++)
		if (cc == (chip->cc_attrs_tbl[i] & GENMASK(15, 0)))
			return i;

	return -1;
}
