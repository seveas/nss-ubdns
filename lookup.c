/*
 * Copyright (C) 2011 Robert S. Edmonds
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <unbound.h>

#include "nss-ubdns.h"

static struct ub_ctx *ctx = NULL;

static void
nss_ubdns_load_keys(void) {
	DIR *dirp;
	int dir_fd;
	struct dirent de;
	struct dirent *res;

	dirp = opendir(NSS_UBDNS_KEYDIR);
	if (dirp == NULL)
		return;

	dir_fd = dirfd(dirp);
	if (dir_fd == -1)
		return;

	while (readdir_r(dirp, &de, &res) == 0 && res != NULL) {
		FILE *fp;
		int fd;
		char *line = NULL;
		char *fn;
		size_t len = 0;
		size_t fnlen;
		ssize_t bytes_read;

		fn = de.d_name;
		fnlen = strlen(fn);
		if (fnlen < 5 ||
		     !(fn[fnlen - 4] == '.' &&
		       fn[fnlen - 3] == 'k' &&
		       fn[fnlen - 2] == 'e' &&
		       fn[fnlen - 1] == 'y'))
		{
			continue;
		}
		if (!isalnum(fn[0]))
			continue;

		fd = openat(dir_fd, de.d_name, O_RDONLY);
		if (fd == -1)
			continue;

		fp = fdopen(fd, "r");
		if (fp == NULL) {
			close(fd);
			continue;
		}

		while ((bytes_read = getline(&line, &len, fp)) != -1) {
			char *p = line;

			while (isspace(*p))
				p++;
			if (*p == '\0' || *p == ';')
				continue;

			ub_ctx_add_ta(ctx, p);
		}
		free(line);
		close(fd);
	}
	closedir(dirp);
}

static void
nss_ubdns_load_cfg(void) {
	ub_ctx_config(ctx, NSS_UBDNS_LUCONF);
}

static void
nss_ubdns_load_resolvconf(void) {
	struct stat sb;

	if (stat(NSS_UBDNS_RESOLVCONF, &sb) == 0) {
		ub_ctx_resolvconf(ctx, NSS_UBDNS_RESOLVCONF);
	} else {
		ub_ctx_set_fwd(ctx, "127.0.0.1");
	}
}

static void __attribute__((constructor))
nss_ubdns_init(void) {
	ctx = ub_ctx_create();
	if (ctx != NULL) {
		/* disable logging to stderr */
		/* the stub resolver must not generate any output to stdio */
		ub_ctx_debugout(ctx, NULL);

		nss_ubdns_load_resolvconf();
		nss_ubdns_load_keys();
		nss_ubdns_load_cfg();
	}
}

static void __attribute__((destructor))
nss_ubdns_finish(void) {
	ub_ctx_delete(ctx);
	ctx = NULL;
}

static bool
nss_ubdns_check_result(struct ub_result *res) {
	if (res->havedata == 0)
		return (false);

	if (res->bogus)
		return (false);

	return (true);
}

static int
nss_ubdns_add_result(struct address **_list, unsigned *_n_list, struct ub_result *res, int af) {
	struct address *list = *_list;
	unsigned n_list = *_n_list;
	int i;

	if (!nss_ubdns_check_result(res))
		return (0);

	for (i = 0; res->data[i] != NULL; i++) {
		if (res->len[i] == PROTO_ADDRESS_SIZE(af)) {
			list = realloc(list, (n_list + 1) * sizeof(struct address));
			if (!list)
				return (-1);

			list[n_list].family = af;
			list[n_list].scope = 0;
			memcpy(list[n_list].address, res->data[i], PROTO_ADDRESS_SIZE(af));
			n_list += 1;
		}
	}

	*_list = list;
	*_n_list = n_list;
	return (0);
}

int
nss_ubdns_lookup_forward(const char *hn, int af, struct address **_list, unsigned *_n_list) {
	struct address *list = NULL;
	unsigned n_list = 0;
	int r = 1;
	int ret;

	struct ub_result *res;

	if (ctx == NULL)
		goto err;

	if (af == AF_INET || af == AF_UNSPEC) {
		ret = ub_resolve(ctx, (char *) hn, NSS_UBDNS_TYPE_A, 1 /*IN*/, &res);
		if (ret != 0)
			goto err;

		ret = nss_ubdns_add_result(&list, &n_list, res, AF_INET);
		if (ret != 0)
			goto err;

		ub_resolve_free(res);
	}

	if (af == AF_INET6 || af == AF_UNSPEC) {
		ret = ub_resolve(ctx, (char *) hn, NSS_UBDNS_TYPE_AAAA, 1 /*IN*/, &res);
		if (ret != 0)
			goto err;

		ret = nss_ubdns_add_result(&list, &n_list, res, AF_INET6);
		if (ret != 0)
			goto err;

		ub_resolve_free(res);
	}

finish:
	if (r < 0) {
		free(list);
	} else {
		qsort(list, n_list, sizeof(struct address), address_compare);

		*_list = list;
		*_n_list = n_list;
	}

	return r;
err:
	r = 0;
	goto finish;
}

char *
nss_ubdns_lookup_reverse(const void *addr, int af) {
	struct ub_result *res = NULL;
	char *qname = NULL;
	int ret;

	if (ctx == NULL)
		return (NULL);

	if (af == AF_INET) {
		arpa_qname_ip4(addr, &qname);
	} else if (af == AF_INET6) {
		arpa_qname_ip6(addr, &qname);
	} else {
		return (NULL);
	}

	ret = ub_resolve(ctx, qname, NSS_UBDNS_TYPE_PTR, 1 /*IN*/, &res);

	if (ret == 0 &&
	    nss_ubdns_check_result(res) &&
	    res->data[0] != NULL)
	{
		char name[NSS_UBDNS_PRESLEN_NAME];
		domain_to_str((const uint8_t *) res->data[0], res->len[0], name);
		ub_resolve_free(res);
		return (strdup(name));
	}

	if (res != NULL)
		ub_resolve_free(res);

	return (NULL);
}
