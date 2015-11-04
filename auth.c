/*	$KAME: auth.c,v 1.4 2004/09/07 05:03:02 jinmei Exp $	*/

/*
 * Copyright (C) 2004 WIDE Project.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Copyright (C) 2004  Internet Systems Consortium, Inc. ("ISC")
 * Copyright (C) 2000, 2001  Internet Software Consortium.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/queue.h>
#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif

#include <netinet/in.h>

#include <syslog.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#ifdef HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#endif

#include <dhcp6.h>
#include <config.h>
#include <common.h>
#include <auth.h>

#define PADLEN 64
#define IPAD 0x36
#define OPAD 0x5C

#define HMACMD5_KEYLENGTH 64

typedef struct {
	u_int32_t buf[4];
	u_int32_t bytes[2];
	u_int32_t in[16];
} md5_t;

typedef struct {
	md5_t md5ctx;
	unsigned char key[HMACMD5_KEYLENGTH];
} hmacmd5_t;

/* opaque placeholder of binary form of public key */
typedef struct pubkey_data {
	void *data;
	size_t len;
} pubkey_data_t;
typedef pubkey_data_t cert_data_t;

static void hmacmd5_init __P((hmacmd5_t *, const unsigned char *,
    unsigned int));
static void hmacmd5_invalidate __P((hmacmd5_t *));
static void hmacmd5_update __P((hmacmd5_t *, const unsigned char *,
    unsigned int));
static void hmacmd5_sign __P((hmacmd5_t *, unsigned char *));
static int hmacmd5_verify __P((hmacmd5_t *, unsigned char *));

static void md5_init __P((md5_t *));
static void md5_invalidate __P((md5_t *));
static void md5_final __P((md5_t *, unsigned char *));
static void md5_update __P((md5_t *, const unsigned char *, unsigned int));

#define UNUSED(x) ((x) = (x))

int
dhcp6_auth_init()
{
	static int initialized = 0; /* XXX: thread-unsafe */

	if (initialized)
		return (0);

#ifdef HAVE_OPENSSL
	ERR_load_crypto_strings();
#endif

	initialized = 1;
	return (0);
}

static int
read_key(int sig_alg, const char *key_file, void **keyp, int is_publickey)
{
	FILE *fp = NULL;
	int ret = -1;

	*keyp = NULL;
	if (sig_alg != DHCP6_SIGALG_RSASSA_PKCS1_V1_5) {
		dprint(LOG_ERR, FNAME, "unknown signing algorithm: %d",
		       sig_alg);
		return (-1);
	}
	fp = fopen(key_file, "r");
	if (fp == NULL) {
		dprint(LOG_ERR, FNAME, "failed to open key file (%s): %s",
		       key_file, strerror(errno));
		goto cleanup;
	}
#ifdef HAVE_OPENSSL
	else {
		RSA *rsa;
		if (is_publickey)
			rsa = PEM_read_RSA_PUBKEY(fp, NULL, NULL, NULL);
		else
			rsa = PEM_read_RSAPrivateKey(fp, NULL, NULL, NULL);
		if (rsa == NULL) {
			dprint(LOG_ERR, FNAME,
			       "failed to read key file (%s): %s", key_file,
			       ERR_reason_error_string(ERR_get_error()));
			goto cleanup;
		}
		*keyp = rsa;
	}
#else
	dprint(LOG_ERR, FNAME, "missing crypto library to read %s key",
	       is_publickey ? "public" : "private");
	goto cleanup;
#endif
	ret = 0;

  cleanup:
	if (fp != NULL)
		fclose(fp);
	return (ret);
}

int
dhcp6_read_pubkey(int sig_alg, const char *key_file, void **keyp)
{
	int ret;
	void *key = NULL;

	ret = read_key(sig_alg, key_file, &key, 1);
#ifdef HAVE_OPENSSL
	if (ret == 0) {
		RSA *rsa = (RSA *)key; /* Right now, this should be RSA key */
		unsigned char *pubkdata = NULL;
		pubkey_data_t *pubkey = NULL;
		int pubkey_len;

		ret = -1; 	/* reset return value */

		/* Extract and copy binary in-memory data of the public key */
		pubkey_len = i2d_RSA_PUBKEY(rsa, &pubkdata);
		if (pubkey_len < 0) {
			dprint(LOG_ERR, FNAME,
			       "failed to dump public key data: %s",
			       ERR_reason_error_string(ERR_get_error()));
			goto cleanup;
		}
		pubkey = malloc(sizeof(*pubkey));
		if (pubkey != NULL) {
			pubkey->data = malloc(pubkey_len);
			if (pubkey->data != NULL) {
				memcpy(pubkey->data, pubkdata, pubkey_len);
				pubkey->len = (size_t)pubkey_len;
				key = pubkey;
				ret = 0;
			}
		}
	  cleanup:
		if (ret != 0 && pubkey)
			OPENSSL_free(pubkey);
		RSA_free(rsa);
	}
#endif
	if (ret == 0)
		*keyp = key;

	return (ret);
}

int
dhcp6_read_privkey(int sig_alg, const char *key_file, void **keyp)
{
	return (read_key(sig_alg, key_file, keyp, 0));
}

int
dhcp6_read_certificate(const char *cert_file, void **certificatep)
{
	int ret = -1;
	FILE *fp = NULL;
	void *certificate;

	fp = fopen(cert_file, "r");
	if (!fp) {
		dprint(LOG_ERR, FNAME, "failed to open certificate file (%s): "
		       "%s", cert_file, strerror(errno));
		return (-1);
	}
#ifdef HAVE_OPENSSL
	else {
		X509 *x509;
		unsigned char *certdata = NULL;
		cert_data_t *cert = NULL;
		int certlen;

		ret = -1; 	/* reset return value */

		x509 = PEM_read_X509(fp, NULL, NULL, NULL);
		if (!x509) {
			dprint(LOG_ERR, FNAME, "failed to read "
			       "certificate file (%s): %s", cert_file,
			       ERR_reason_error_string(ERR_get_error()));
			goto cleanup;
		}
		certlen =  i2d_X509(x509, &certdata);
		if (certlen < 0) {
			dprint(LOG_ERR, FNAME,
			       "failed to dump certificate data: %s",
			       ERR_reason_error_string(ERR_get_error()));
			goto cleanup;
		}
		cert = malloc(sizeof(*cert));
		if (cert) {
			cert->data = malloc(certlen);
			if (cert->data) {
				memcpy(cert->data, certdata, certlen);
				cert->len = (size_t)certlen;
				certificate = cert;
				ret = 0;
			}
		  cleanup:
			if (ret != 0 && cert)
				OPENSSL_free(cert);
			X509_free(x509);
		}
	}
#else
	dprint(LOG_ERR, FNAME, "missing crypto library to read certificate");
#endif
	if (ret == 0)
		*certificatep = certificate;
	fclose(fp);

	return (ret);
}

void
dhcp6_free_pubkey(void **keyp)
{
	if (*keyp != NULL) {
		pubkey_data_t *pubkey = (pubkey_data_t *)*keyp;

		free(pubkey->data);
		free(pubkey);
	}
	*keyp = NULL;
}

void
dhcp6_free_certificate(void **certp)
{
	dhcp6_free_pubkey(certp);
}

void
dhcp6_free_privkey(int sig_alg, void **keyp)
{
	if (sig_alg == DHCP6_SIGALG_RSASSA_PKCS1_V1_5) {
#ifdef HAVE_OPENSSL
		if (*keyp != NULL)
			RSA_free((RSA *)*keyp);
#endif
	}
	*keyp = NULL;
}

void
dhcp6_set_pubkey(void *key, struct dhcp6_vbuf *dst)
{
	pubkey_data_t *pubkey = (pubkey_data_t *)key;
	dst->dv_len = pubkey->len;
	dst->dv_buf = pubkey->data;
}

void
dhcp6_set_certificate(void *cert, struct dhcp6_vbuf *dst)
{
	dhcp6_set_pubkey(cert, dst);
}

void *
dhcp6_copy_pubkey(void *src)
{
	pubkey_data_t *dst;
	pubkey_data_t *pubkey = (pubkey_data_t *)src;

	dst = malloc(sizeof(*dst));
	if (!dst)
		return (dst);
	dst->data = malloc(pubkey->len);
	if (!dst->data) {
		free(dst);
		return (NULL);
	}
	dst->len = pubkey->len;
	memcpy(dst->data, pubkey->data, dst->len);

	return (dst);
}

void *
dhcp6_copy_certificate(void *src)
{
	/* the format is the same, so we can reuse it */
	return (dhcp6_copy_pubkey(src));
}

void *
dhcp6_copy_privkey(int sig_alg, void *src)
{
	void *dst = NULL;

	if (sig_alg != DHCP6_SIGALG_RSASSA_PKCS1_V1_5) {
		dprint(LOG_ERR, FNAME, "unknown signing algorithm: %d",
		       sig_alg);
		return (dst);
	}
#ifdef HAVE_OPENSSL
	else {
		RSA *rsa = (RSA *)src;
		RSA *rsa_dst;
		unsigned char *keydata = NULL;
		const unsigned char *kd;
		int key_len;

		key_len = i2d_RSAPrivateKey(rsa, &keydata);
		if (key_len < 0) {
			dprint(LOG_ERR, FNAME,
			       "failed to dump private key data: %s",
			       ERR_reason_error_string(ERR_get_error()));
			goto cleanup;
		}
		kd = (const unsigned char *)keydata;
		rsa_dst = d2i_RSAPrivateKey(NULL, &kd, key_len);
		if (rsa_dst == NULL) {
			dprint(LOG_ERR, FNAME, "failed to get RSA data: %s",
			       ERR_reason_error_string(ERR_get_error()));
			goto cleanup;
		}
		dst = rsa_dst;

	  cleanup:
		if (keydata)
			OPENSSL_free(keydata);
	}
#else
	UNUSED(src);		/* silence compiler */
#endif

	return (dst);
}

size_t
dhcp6_get_sigsize(int sig_alg, void *priv_key)
{
	if (sig_alg != DHCP6_SIGALG_RSASSA_PKCS1_V1_5) {
		dprint(LOG_ERR, FNAME, "unknown signing algorithm: %d",
		       sig_alg);
		return (0);
	}
#ifdef HAVE_OPENSSL
	else if (priv_key)
		return (RSA_size((RSA *)priv_key));
#else
	UNUSED(priv_key);
#endif
	return (0);
}

int
dhcp6_sign_msg(unsigned char *buf, size_t len, size_t off,
	       struct authparam *authparam)
{
	if (authparam->authproto != DHCP6_AUTHPROTO_SEDHCPV6) {
		dprint(LOG_ERR, FNAME,
		       "assumption failure: invalid sign protocol",
		       authparam->authproto);
		return (-1);
	}
	if (authparam->sedhcpv6.sig_algorithm !=
	    DHCP6_SIGALG_RSASSA_PKCS1_V1_5) {
		dprint(LOG_ERR, FNAME, "unknown signing algorithm: %d",
		       authparam->sedhcpv6.sig_algorithm);
		return (-1);
	}
	if (authparam->sedhcpv6.hash_algorithm != DHCP6_HASHALG_SHA256) {
		dprint(LOG_ERR, FNAME, "unknown hash algorithm for sign: %d",
		       authparam->sedhcpv6.hash_algorithm);
		return (-1);
	}

#ifdef HAVE_OPENSSL
	{
		SHA256_CTX sha_ctx;
		unsigned char digest[SHA256_DIGEST_LENGTH];
		RSA *rsa = authparam->sedhcpv6.private_key;
		unsigned char *sig = buf + off;
		unsigned int siglen;

		if (off + RSA_size(rsa) > len) {
			/*
			 * should be assured by the caller, but check it here
			 * for safety.
			 */
			dprint(LOG_ERR, FNAME,
			       "assumption failure: short buffer (%u vs %u)",
			       off + RSA_size(rsa), len);
			return (-1);
		}

		/* digest the data */
		SHA256_Init(&sha_ctx);
		SHA256_Update(&sha_ctx, buf, len);
		SHA256_Final(digest, &sha_ctx);

		/* sign it */
		if (RSA_sign(NID_sha256, digest, sizeof(digest), sig,
			     &siglen, rsa) != 1) {
			dprint(LOG_ERR, FNAME, "failed to sign: %s",
			       ERR_reason_error_string(ERR_get_error()));
			return (-1);
		}
		if (siglen != (unsigned int)RSA_size(rsa)) {
			dprint(LOG_ERR, FNAME, "assumption failure: "
			       "inconsistent siglen: %u vs %u",
			       siglen, RSA_size(rsa));
			return (-1);
		}
	}
#else
	UNUSED(buf);
	UNUSED(len);
	UNUSED(off);
	dprint(LOG_ERR, FNAME, "missing crypto library for sign");
	return (-1);
#endif

	return (0);
}

int
dhcp6_verify_msg(unsigned char *buf, size_t len, size_t offset, size_t sig_len,
		 int hash_alg, int sig_alg, const struct dhcp6_vbuf *pubkey)
{
	int ret = -1;

	/* sanity check: shouldn't happen unless the caller is buggy */
	if (len < offset + sig_len) {
		dprint(LOG_ERR, FNAME,
		       "assumption failure: short buffer (%u vs %u)",
			       offset + sig_len, len);
		return (-1);
	}

	if (sig_alg != DHCP6_SIGALG_RSASSA_PKCS1_V1_5) {
		dprint(LOG_ERR, FNAME, "unknown signing algorithm: %d",
		       sig_alg);
		return (-1);
	}
	if (hash_alg != DHCP6_HASHALG_SHA256) {
		dprint(LOG_ERR, FNAME, "unknown hash algorithm for sign: %d",
		       hash_alg);
		return (-1);
	}
#ifdef HAVE_OPENSSL
	{
		RSA *rsa = NULL;
		const unsigned char *p;
		unsigned char *sig_copy = NULL;
		SHA256_CTX sha_ctx;
		unsigned char digest[SHA256_DIGEST_LENGTH];

		p = (const unsigned char *)pubkey->dv_buf;
		rsa = d2i_RSA_PUBKEY(NULL, &p, pubkey->dv_len);
		if (!rsa) {
			dprint(LOG_ERR, FNAME,
			       "failed to build public key from data: %s",
			       ERR_reason_error_string(ERR_get_error()));
			goto cleanup;
		}

		/* XXX: see dhcp6_verify_mac */
		sig_copy = malloc(sig_len);
		if (!sig_copy) {
			dprint(LOG_ERR, FNAME, "memory allocation failure");
			goto cleanup;
		}
		memcpy(sig_copy, buf + offset, sig_len);
		memset(buf + offset, 0, sig_len);

		/* digest the data */
		SHA256_Init(&sha_ctx);
		SHA256_Update(&sha_ctx, buf, len);
		SHA256_Final(digest, &sha_ctx);

		/* verify signature */
		if (!RSA_verify(NID_sha256, digest, sizeof(digest), sig_copy,
				sig_len, rsa)) {
			dprint(LOG_ERR, FNAME, "failed to verify signature: %s",
			       ERR_reason_error_string(ERR_get_error()));
			goto cleanup;
		}
		ret = 0;

	  cleanup:
		if (sig_copy) {
			memcpy(buf + offset, sig_copy, sig_len);
			free(sig_copy);
		}
		if (rsa)
			RSA_free(rsa);
	}
#else
	UNUSED(buf);
	UNUSED(pubkey);
	dprint(LOG_INFO, FNAME,
	       "missing crypto library for Secure DHCPv6 signature");
#endif

	return (ret);
}

struct auth_peer *
dhcp6_create_authpeer(const struct duid *peer_id,
		      const struct dhcp6_vbuf *pubkey)
{
	struct auth_peer *peer = NULL;

	peer = malloc(sizeof(*peer));
	if (!peer)
		return (NULL);
	memset(peer, 0, sizeof(*peer));

	if (duidcpy(&peer->id, peer_id))
		goto fail;
	if (dhcp6_vbuf_copy(&peer->pubkey, pubkey))
		goto fail;
	dhcp6_timestamp_set_undef(&peer->ts_last);
	dhcp6_timestamp_set_undef(&peer->ts_rcv_last);

	return (peer);

  fail:
	duidfree(&peer->id);
	dhcp6_vbuf_free(&peer->pubkey);
	free(peer);
	return (NULL);
}

struct auth_peer *
dhcp6_find_authpeer(const struct dhcp6_auth_peerlist *peers,
		    const struct duid *peer_id)
{
	struct auth_peer *peer;

	for (peer = TAILQ_FIRST(peers); peer; peer = TAILQ_NEXT(peer, link)) {
		if (duidcmp(&peer->id, peer_id) == 0)
			return (peer);
	}
	return (NULL);
}

/*
 * Secure DHCPv6 timestamp check
 */

/* Pre-defined constants (not configurable at this moment) */
static const uint64_t ts_delta = 5000000ULL; /* 5sec in usec */
static const uint64_t ts_fuzz = 1000000ULL; /* 1sec in usec */
static const uint64_t ts_drift = 1; /* percent */

static uint64_t
tv2usec(const struct timeval *tv) {
	return ((uint64_t)tv->tv_sec * 1000000ULL + (uint64_t)tv->tv_usec);
}

int
dhcp6_check_timestamp(struct auth_peer *peer, const struct timeval *rcv_ts)
{
	struct timeval now;
	uint64_t now_us, rcv_ts_us, last_ts_us, last_rcv_ts_us;

	gettimeofday(&now, NULL);
	now_us = tv2usec(&now);
	rcv_ts_us = tv2usec(rcv_ts);

	if (dhcp6_timestamp_undef(&peer->ts_last)) {
		if ((now_us > rcv_ts_us && (now_us - rcv_ts_us) < ts_delta) ||
		    (now_us <= rcv_ts_us && (rcv_ts_us - now_us) < ts_delta)) {
			peer->ts_last = now;
			peer->ts_rcv_last = *rcv_ts;
			return (1);
		}
	} else {
		last_ts_us = tv2usec(&peer->ts_last);
		last_rcv_ts_us = tv2usec(&peer->ts_rcv_last);

		/* If newer received timestamp is older, the check fails*/
		if (rcv_ts_us < last_rcv_ts_us)
			return (0);

		/* Check the equality of the spec */
		if (now_us + ts_fuzz >
		    last_ts_us +
		    ((rcv_ts_us - last_rcv_ts_us) * (100 - ts_drift)) / 100
		    - ts_fuzz)
		{
			if (now_us > last_ts_us) {
				peer->ts_last = now;
				peer->ts_rcv_last = *rcv_ts;
			}
			return (1);
		}
	}

	return (0);
}

int
dhcp6_validate_key(key)
	struct keyinfo *key;
{
	time_t now;

	if (key->expire == 0)	/* never expire */
		return (0);

	if (time(&now) == -1)
		return (-1);	/* treat it as expiration (XXX) */

	if (now > key->expire)
		return (-1);

	return (0);
}

int
dhcp6_calc_mac(buf, len, proto, alg, off, key)
	unsigned char *buf;
	size_t len, off;
	int proto, alg;
	struct keyinfo *key;
{
	hmacmd5_t ctx;
	unsigned char digest[MD5_DIGESTLENGTH];

	/* right now, we don't care about the protocol */
	UNUSED(proto);		/* silence compiler */

	if (alg != DHCP6_AUTHALG_HMACMD5)
		return (-1);

	if (off + MD5_DIGESTLENGTH > len) {
		/*
		 * this should be assured by the caller, but check it here
		 * for safety.
		 */
		return (-1);
	}

	hmacmd5_init(&ctx, key->secret, key->secretlen);
	hmacmd5_update(&ctx, buf, len);
	hmacmd5_sign(&ctx, digest);

	memcpy(buf + off, digest, MD5_DIGESTLENGTH);

	return (0);
}

int
dhcp6_verify_mac(buf, len, proto, alg, off, key)
	unsigned char *buf;
	ssize_t len;
	int proto, alg;
	size_t off;
	struct keyinfo *key;
{
	hmacmd5_t ctx;
	unsigned char digest[MD5_DIGESTLENGTH];
	int result;

	/* right now, we don't care about the protocol */
	UNUSED(proto);		/* silence compiler */

	if (alg != DHCP6_AUTHALG_HMACMD5)
		return (-1);

	if ((ssize_t)off + MD5_DIGESTLENGTH > len)
		return (-1);

	/*
	 * Copy the MAC value and clear the field.
	 * XXX: should we make a local working copy?
	 */
	memcpy(digest, buf + off, sizeof(digest));
	memset(buf + off, 0, sizeof(digest));

	hmacmd5_init(&ctx, key->secret, key->secretlen);
	hmacmd5_update(&ctx, buf, len);
	result = hmacmd5_verify(&ctx, digest);

	/* copy back the digest value (XXX) */
	memcpy(buf + off, digest, sizeof(digest));

	return (result);
}

/*
 * This code implements the HMAC-MD5 keyed hash algorithm
 * described in RFC 2104.
 */
/*
 * Start HMAC-MD5 process.  Initialize an md5 context and digest the key.
 */
static void
hmacmd5_init(hmacmd5_t *ctx, const unsigned char *key, unsigned int len)
{
	unsigned char ipad[PADLEN];
	int i;

	memset(ctx->key, 0, sizeof(ctx->key));
	if (len > sizeof(ctx->key)) {
		md5_t md5ctx;
		md5_init(&md5ctx);
		md5_update(&md5ctx, key, len);
		md5_final(&md5ctx, ctx->key);
	} else
		memcpy(ctx->key, key, len);

	md5_init(&ctx->md5ctx);
	memset(ipad, IPAD, sizeof(ipad));
	for (i = 0; i < PADLEN; i++)
		ipad[i] ^= ctx->key[i];
	md5_update(&ctx->md5ctx, ipad, sizeof(ipad));
}

static void
hmacmd5_invalidate(hmacmd5_t *ctx)
{
	md5_invalidate(&ctx->md5ctx);
	memset(ctx->key, 0, sizeof(ctx->key));
	memset(ctx, 0, sizeof(ctx));
}

/*
 * Update context to reflect the concatenation of another buffer full
 * of bytes.
 */
static void
hmacmd5_update(hmacmd5_t *ctx, const unsigned char *buf, unsigned int len)
{
	md5_update(&ctx->md5ctx, buf, len);
}

/*
 * Compute signature - finalize MD5 operation and reapply MD5.
 */
static void
hmacmd5_sign(hmacmd5_t *ctx, unsigned char *digest)
{
	unsigned char opad[PADLEN];
	int i;

	md5_final(&ctx->md5ctx, digest);

	memset(opad, OPAD, sizeof(opad));
	for (i = 0; i < PADLEN; i++)
		opad[i] ^= ctx->key[i];

	md5_init(&ctx->md5ctx);
	md5_update(&ctx->md5ctx, opad, sizeof(opad));
	md5_update(&ctx->md5ctx, digest, MD5_DIGESTLENGTH);
	md5_final(&ctx->md5ctx, digest);
	hmacmd5_invalidate(ctx);
}

/*
 * Verify signature - finalize MD5 operation and reapply MD5, then
 * compare to the supplied digest.
 */
static int
hmacmd5_verify(hmacmd5_t *ctx, unsigned char *digest) {
	unsigned char newdigest[MD5_DIGESTLENGTH];

	hmacmd5_sign(ctx, newdigest);
	return (memcmp(digest, newdigest, MD5_DIGESTLENGTH));
}

/*
 * This code implements the MD5 message-digest algorithm.
 * The algorithm is due to Ron Rivest.  This code was
 * written by Colin Plumb in 1993, no copyright is claimed.
 * This code is in the public domain; do with it what you wish.
 *
 * Equivalent code is available from RSA Data Security, Inc.
 * This code has been tested against that, and is equivalent,
 * except that you don't need to include two pages of legalese
 * with every copy.
 *
 * To compute the message digest of a chunk of bytes, declare an
 * MD5Context structure, pass it to MD5Init, call MD5Update as
 * needed on buffers full of bytes, and then call MD5Final, which
 * will fill a supplied 16-byte array with the digest.
 */

static void
byteSwap(u_int32_t *buf, unsigned words)
{
	unsigned char *p = (unsigned char *)buf;

	do {
		*buf++ = (u_int32_t)((unsigned)p[3] << 8 | p[2]) << 16 |
			((unsigned)p[1] << 8 | p[0]);
		p += 4;
	} while (--words);
}

/*
 * Start MD5 accumulation.  Set bit count to 0 and buffer to mysterious
 * initialization constants.
 */
static void
md5_init(md5_t *ctx)
{
	ctx->buf[0] = 0x67452301;
	ctx->buf[1] = 0xefcdab89;
	ctx->buf[2] = 0x98badcfe;
	ctx->buf[3] = 0x10325476;

	ctx->bytes[0] = 0;
	ctx->bytes[1] = 0;
}

static void
md5_invalidate(md5_t *ctx)
{
	memset(ctx, 0, sizeof(md5_t));
}

/* The four core functions - F1 is optimized somewhat */

/* #define F1(x, y, z) (x & y | ~x & z) */
#define F1(x, y, z) (z ^ (x & (y ^ z)))
#define F2(x, y, z) F1(z, x, y)
#define F3(x, y, z) (x ^ y ^ z)
#define F4(x, y, z) (y ^ (x | ~z))

/* This is the central step in the MD5 algorithm. */
#define MD5STEP(f,w,x,y,z,in,s) \
	 (w += f(x,y,z) + in, w = (w<<s | w>>(32-s)) + x)

/*
 * The core of the MD5 algorithm, this alters an existing MD5 hash to
 * reflect the addition of 16 longwords of new data.  MD5Update blocks
 * the data and converts bytes into longwords for this routine.
 */
static void
transform(u_int32_t buf[4], u_int32_t const in[16]) {
	register u_int32_t a, b, c, d;

	a = buf[0];
	b = buf[1];
	c = buf[2];
	d = buf[3];

	MD5STEP(F1, a, b, c, d, in[0] + 0xd76aa478, 7);
	MD5STEP(F1, d, a, b, c, in[1] + 0xe8c7b756, 12);
	MD5STEP(F1, c, d, a, b, in[2] + 0x242070db, 17);
	MD5STEP(F1, b, c, d, a, in[3] + 0xc1bdceee, 22);
	MD5STEP(F1, a, b, c, d, in[4] + 0xf57c0faf, 7);
	MD5STEP(F1, d, a, b, c, in[5] + 0x4787c62a, 12);
	MD5STEP(F1, c, d, a, b, in[6] + 0xa8304613, 17);
	MD5STEP(F1, b, c, d, a, in[7] + 0xfd469501, 22);
	MD5STEP(F1, a, b, c, d, in[8] + 0x698098d8, 7);
	MD5STEP(F1, d, a, b, c, in[9] + 0x8b44f7af, 12);
	MD5STEP(F1, c, d, a, b, in[10] + 0xffff5bb1, 17);
	MD5STEP(F1, b, c, d, a, in[11] + 0x895cd7be, 22);
	MD5STEP(F1, a, b, c, d, in[12] + 0x6b901122, 7);
	MD5STEP(F1, d, a, b, c, in[13] + 0xfd987193, 12);
	MD5STEP(F1, c, d, a, b, in[14] + 0xa679438e, 17);
	MD5STEP(F1, b, c, d, a, in[15] + 0x49b40821, 22);

	MD5STEP(F2, a, b, c, d, in[1] + 0xf61e2562, 5);
	MD5STEP(F2, d, a, b, c, in[6] + 0xc040b340, 9);
	MD5STEP(F2, c, d, a, b, in[11] + 0x265e5a51, 14);
	MD5STEP(F2, b, c, d, a, in[0] + 0xe9b6c7aa, 20);
	MD5STEP(F2, a, b, c, d, in[5] + 0xd62f105d, 5);
	MD5STEP(F2, d, a, b, c, in[10] + 0x02441453, 9);
	MD5STEP(F2, c, d, a, b, in[15] + 0xd8a1e681, 14);
	MD5STEP(F2, b, c, d, a, in[4] + 0xe7d3fbc8, 20);
	MD5STEP(F2, a, b, c, d, in[9] + 0x21e1cde6, 5);
	MD5STEP(F2, d, a, b, c, in[14] + 0xc33707d6, 9);
	MD5STEP(F2, c, d, a, b, in[3] + 0xf4d50d87, 14);
	MD5STEP(F2, b, c, d, a, in[8] + 0x455a14ed, 20);
	MD5STEP(F2, a, b, c, d, in[13] + 0xa9e3e905, 5);
	MD5STEP(F2, d, a, b, c, in[2] + 0xfcefa3f8, 9);
	MD5STEP(F2, c, d, a, b, in[7] + 0x676f02d9, 14);
	MD5STEP(F2, b, c, d, a, in[12] + 0x8d2a4c8a, 20);

	MD5STEP(F3, a, b, c, d, in[5] + 0xfffa3942, 4);
	MD5STEP(F3, d, a, b, c, in[8] + 0x8771f681, 11);
	MD5STEP(F3, c, d, a, b, in[11] + 0x6d9d6122, 16);
	MD5STEP(F3, b, c, d, a, in[14] + 0xfde5380c, 23);
	MD5STEP(F3, a, b, c, d, in[1] + 0xa4beea44, 4);
	MD5STEP(F3, d, a, b, c, in[4] + 0x4bdecfa9, 11);
	MD5STEP(F3, c, d, a, b, in[7] + 0xf6bb4b60, 16);
	MD5STEP(F3, b, c, d, a, in[10] + 0xbebfbc70, 23);
	MD5STEP(F3, a, b, c, d, in[13] + 0x289b7ec6, 4);
	MD5STEP(F3, d, a, b, c, in[0] + 0xeaa127fa, 11);
	MD5STEP(F3, c, d, a, b, in[3] + 0xd4ef3085, 16);
	MD5STEP(F3, b, c, d, a, in[6] + 0x04881d05, 23);
	MD5STEP(F3, a, b, c, d, in[9] + 0xd9d4d039, 4);
	MD5STEP(F3, d, a, b, c, in[12] + 0xe6db99e5, 11);
	MD5STEP(F3, c, d, a, b, in[15] + 0x1fa27cf8, 16);
	MD5STEP(F3, b, c, d, a, in[2] + 0xc4ac5665, 23);

	MD5STEP(F4, a, b, c, d, in[0] + 0xf4292244, 6);
	MD5STEP(F4, d, a, b, c, in[7] + 0x432aff97, 10);
	MD5STEP(F4, c, d, a, b, in[14] + 0xab9423a7, 15);
	MD5STEP(F4, b, c, d, a, in[5] + 0xfc93a039, 21);
	MD5STEP(F4, a, b, c, d, in[12] + 0x655b59c3, 6);
	MD5STEP(F4, d, a, b, c, in[3] + 0x8f0ccc92, 10);
	MD5STEP(F4, c, d, a, b, in[10] + 0xffeff47d, 15);
	MD5STEP(F4, b, c, d, a, in[1] + 0x85845dd1, 21);
	MD5STEP(F4, a, b, c, d, in[8] + 0x6fa87e4f, 6);
	MD5STEP(F4, d, a, b, c, in[15] + 0xfe2ce6e0, 10);
	MD5STEP(F4, c, d, a, b, in[6] + 0xa3014314, 15);
	MD5STEP(F4, b, c, d, a, in[13] + 0x4e0811a1, 21);
	MD5STEP(F4, a, b, c, d, in[4] + 0xf7537e82, 6);
	MD5STEP(F4, d, a, b, c, in[11] + 0xbd3af235, 10);
	MD5STEP(F4, c, d, a, b, in[2] + 0x2ad7d2bb, 15);
	MD5STEP(F4, b, c, d, a, in[9] + 0xeb86d391, 21);

	buf[0] += a;
	buf[1] += b;
	buf[2] += c;
	buf[3] += d;
}

/*
 * Update context to reflect the concatenation of another buffer full
 * of bytes.
 */
static void
md5_update(md5_t *ctx, const unsigned char *buf, unsigned int len)
{
	u_int32_t t;

	/* Update byte count */

	t = ctx->bytes[0];
	if ((ctx->bytes[0] = t + len) < t)
		ctx->bytes[1]++;	/* Carry from low to high */

	t = 64 - (t & 0x3f);	/* Space available in ctx->in (at least 1) */
	if (t > len) {
		memcpy((unsigned char *)ctx->in + 64 - t, buf, len);
		return;
	}
	/* First chunk is an odd size */
	memcpy((unsigned char *)ctx->in + 64 - t, buf, t);
	byteSwap(ctx->in, 16);
	transform(ctx->buf, ctx->in);
	buf += t;
	len -= t;

	/* Process data in 64-byte chunks */
	while (len >= 64) {
		memcpy(ctx->in, buf, 64);
		byteSwap(ctx->in, 16);
		transform(ctx->buf, ctx->in);
		buf += 64;
		len -= 64;
	}

	/* Handle any remaining bytes of data. */
	memcpy(ctx->in, buf, len);
}

/*
 * Final wrapup - pad to 64-byte boundary with the bit pattern
 * 1 0* (64-bit count of bits processed, MSB-first)
 */
static void
md5_final(md5_t *ctx, unsigned char *digest)
{
	int count = ctx->bytes[0] & 0x3f;    /* Number of bytes in ctx->in */
	unsigned char *p = (unsigned char *)ctx->in + count;

	/* Set the first char of padding to 0x80.  There is always room. */
	*p++ = 0x80;

	/* Bytes of padding needed to make 56 bytes (-8..55) */
	count = 56 - 1 - count;

	if (count < 0) {	/* Padding forces an extra block */
		memset(p, 0, count + 8);
		byteSwap(ctx->in, 16);
		transform(ctx->buf, ctx->in);
		p = (unsigned char *)ctx->in;
		count = 56;
	}
	memset(p, 0, count);
	byteSwap(ctx->in, 14);

	/* Append length in bits and transform */
	ctx->in[14] = ctx->bytes[0] << 3;
	ctx->in[15] = ctx->bytes[1] << 3 | ctx->bytes[0] >> 29;
	transform(ctx->buf, ctx->in);

	byteSwap(ctx->buf, 4);
	memcpy(digest, ctx->buf, 16);
	memset(ctx, 0, sizeof(md5_t));	/* In case it's sensitive */
}
