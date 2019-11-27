/*
 * Copyright (c) 2018 Yubico AB. All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the LICENSE file.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef USE_HIDAPI
#include <hidapi/hidapi.h>
#endif // USE_HIDAPI

#include "fido.h"

#if defined(_WIN32)
#include <windows.h>

#include <winternl.h>
#include <winerror.h>
#include <stdio.h>
#include <bcrypt.h>
#include <sal.h>

static int
obtain_nonce(uint64_t *nonce)
{
	NTSTATUS status;

	status = BCryptGenRandom(NULL, (unsigned char *)nonce, sizeof(*nonce),
	    BCRYPT_USE_SYSTEM_PREFERRED_RNG);

	if (!NT_SUCCESS(status))
		return (-1);

	return (0);
}
#elif defined(HAS_DEV_URANDOM)
static int
obtain_nonce(uint64_t *nonce)
{
	int	fd = -1;
	int	ok = -1;
	ssize_t	r;

	if ((fd = open(FIDO_RANDOM_DEV, O_RDONLY)) < 0)
		goto fail;
	if ((r = read(fd, nonce, sizeof(*nonce))) < 0 ||
	    (size_t)r != sizeof(*nonce))
		goto fail;

	ok = 0;
fail:
	if (fd != -1)
		close(fd);

	return (ok);
}
#else
#error "please provide an implementation of obtain_nonce() for your platform"
#endif /* _WIN32 */

typedef struct dev_manifest_func_node {
	dev_manifest_func_t manifest_func;
	struct dev_manifest_func_node *next;
} dev_manifest_func_node_t;

static dev_manifest_func_node_t* manifest_funcs = NULL;

static int
fido_dev_open_tx(fido_dev_t *dev, const char *path)
{
	const uint8_t cmd = CTAP_FRAME_INIT | CTAP_CMD_INIT;

	if (dev->io_handle != NULL) {
		fido_log_debug("%s: handle=%p", __func__, dev->io_handle);
		return (FIDO_ERR_INVALID_ARGUMENT);
	}

	if (dev->dev_info->io.open == NULL || dev->dev_info->io.close == NULL) {
		fido_log_debug("%s: NULL open/close", __func__);
		return (FIDO_ERR_INVALID_ARGUMENT);
	}

	if (obtain_nonce(&dev->nonce) < 0) {
		fido_log_debug("%s: obtain_nonce", __func__);
		return (FIDO_ERR_INTERNAL);
	}

	if ((dev->io_handle = dev->dev_info->io.open(path)) == NULL) {
		fido_log_debug("%s: dev->io.open", __func__);
		return (FIDO_ERR_INTERNAL);
	}

	if (fido_tx(dev, cmd, &dev->nonce, sizeof(dev->nonce)) < 0) {
		fido_log_debug("%s: fido_tx", __func__);
		dev->dev_info->io.close(dev->io_handle);
		dev->io_handle = NULL;
		return (FIDO_ERR_TX);
	}

	return (FIDO_OK);
}

static int
fido_dev_open_rx(fido_dev_t *dev, int ms)
{
	const uint8_t	cmd = CTAP_FRAME_INIT | CTAP_CMD_INIT;
	int		n;

	if ((n = fido_rx(dev, cmd, &dev->attr, sizeof(dev->attr), ms)) < 0) {
		fido_log_debug("%s: fido_rx", __func__);
		goto fail;
	}

#ifdef FIDO_FUZZ
	dev->attr.nonce = dev->nonce;
#endif

	if ((size_t)n != sizeof(dev->attr) || dev->attr.nonce != dev->nonce) {
		fido_log_debug("%s: invalid nonce", __func__);
		goto fail;
	}

	dev->cid = dev->attr.cid;

	return (FIDO_OK);
fail:
	dev->dev_info->io.close(dev->io_handle);
	dev->io_handle = NULL;

	return (FIDO_ERR_RX);
}

static int
fido_dev_open_wait_with_info(fido_dev_t *dev, int ms)
{
	int r;

	if ((r = fido_dev_open_tx(dev, dev->dev_info->path)) != FIDO_OK ||
	    (r = fido_dev_open_rx(dev, ms)) != FIDO_OK)
		return (r);

	return (FIDO_OK);
}

static int
fido_dev_open_wait(fido_dev_t *dev, const char *path, int ms)
{
	int r;

	if ((r = fido_dev_open_tx(dev, path)) != FIDO_OK ||
	    (r = fido_dev_open_rx(dev, ms)) != FIDO_OK)
		return (r);

	return (FIDO_OK);
}

static fido_dev_info_t*
copy_fido_dev_info(const fido_dev_info_t *dev_info)
{
	fido_dev_info_t *copied_dev_info = calloc(1, sizeof(fido_dev_info_t));
	if (copied_dev_info == NULL) {
		fido_log_debug("%s: calloc failed.\n", __func__);
		return NULL;
	}
	*copied_dev_info = *dev_info;
	if (dev_info->path != NULL)
		copied_dev_info->path = strdup(dev_info->path);
	if (dev_info->manufacturer != NULL)
		copied_dev_info->manufacturer = strdup(dev_info->manufacturer);
	if (dev_info->product != NULL)
		copied_dev_info->product = strdup(dev_info->product);
	return (copied_dev_info);
}

int
fido_dev_register_manifest_func(const dev_manifest_func_t func)
{
	dev_manifest_func_node_t *n = calloc(1, sizeof(dev_manifest_func_node_t));
	if (n == NULL) {
		fido_log_debug("%s: calloc failed.\n", __func__);
		return (FIDO_ERR_INTERNAL);
	}
	n->manifest_func = func;
	n->next = manifest_funcs;
	manifest_funcs = n;
	return (FIDO_OK);
}

int
fido_dev_info_manifest(fido_dev_info_t *devlist, size_t ilen, size_t *olen)
{
	dev_manifest_func_node_t *curr = NULL;
	*olen = 0;
	dev_manifest_func_t m_func;
	for (curr = manifest_funcs; curr != NULL; curr = curr->next) {
		size_t curr_olen;
		int r;
		m_func = curr->manifest_func;
		r = m_func(devlist + *olen, ilen - *olen, &curr_olen);
		if (r == FIDO_OK)
			*olen += curr_olen;
		else
			return (r);
		if (*olen == ilen)
			break;
	}
	return (FIDO_OK);
}

int
fido_dev_open_with_info(fido_dev_t *dev)
{
	return fido_dev_open_wait_with_info(dev, -1);
}

int
fido_dev_open(fido_dev_t *dev, const char *path)
{
	return fido_dev_open_wait(dev, path, -1);
}

int
fido_dev_close(fido_dev_t *dev)
{
	if (dev->io_handle == NULL || dev->dev_info->io.close == NULL)
		return (FIDO_ERR_INVALID_ARGUMENT);

	dev->dev_info->io.close(dev->io_handle);
	dev->io_handle = NULL;

	return (FIDO_OK);
}

int
fido_dev_cancel(fido_dev_t *dev)
{
	if (fido_tx(dev, CTAP_FRAME_INIT | CTAP_CMD_CANCEL, NULL, 0) < 0)
		return (FIDO_ERR_TX);

	return (FIDO_OK);
}

int
fido_dev_set_io_functions(fido_dev_t *dev, const fido_dev_io_t *io)
{
	if (dev->io_handle != NULL) {
		fido_log_debug("%s: NULL handle", __func__);
		return (FIDO_ERR_INVALID_ARGUMENT);
	}

	if (io == NULL || io->open == NULL || io->close == NULL ||
	    io->read == NULL || io->write == NULL) {
		fido_log_debug("%s: NULL function", __func__);
		return (FIDO_ERR_INVALID_ARGUMENT);
	}

	dev->dev_info->io = *io;

	return (FIDO_OK);
}

void
fido_init(int flags)
{
	if (flags & FIDO_DEBUG || getenv("FIDO_DEBUG") != NULL)
		fido_log_init();
#if defined(LINUX)
#ifdef USE_HIDAPI
	fido_dev_register_manifest_func(hidapi_dev_info_manifest);
	if (hid_init() != 0)
		errx(1, "hid_init failed");
#else
	fido_dev_register_manifest_func(fido_dev_info_manifest_linux);
#endif // USE_HIDAPI
#elif defined(OSX)
	fido_dev_register_manifest_func(fido_dev_info_manifest_osx);
#elif defined(WIN)
	fido_dev_register_manifest_func(fido_dev_info_manifest_win);
#elif defined(OPENBSD)
	fido_dev_register_manifest_func(fido_dev_info_manifest_openbsd);
#endif // LINUX|OSX|WIN|OPENBSD
}

void
fido_exit(void)
{
#ifdef USE_HIDAPI
	hid_exit();
#endif
	dev_manifest_func_node_t *curr = manifest_funcs;
	dev_manifest_func_node_t *next = NULL;
	while (curr != NULL) {
		next = curr->next;
		free(curr);
		curr = next;
	}
	manifest_funcs = NULL;
}

fido_dev_t *
fido_dev_new()
{
	fido_dev_t	*dev;

	if ((dev = calloc(1, sizeof(*dev))) == NULL)
		return (NULL);

	dev->cid = CTAP_CID_BROADCAST;

	dev->dev_info = fido_dev_info_new(1);

	if (dev->dev_info == NULL) {
		fido_log_debug("%s: dev_info cannot be allocated", __func__);
		fido_dev_free(&dev);
		return (NULL);
	}

	dev->dev_info->io = (fido_dev_io_t) {
		&fido_hid_open,
		&fido_hid_close,
		&fido_hid_read,
		&fido_hid_write
	};

	return (dev);
}

fido_dev_t *
fido_dev_new_with_info(const fido_dev_info_t *dev_info)
{
	fido_dev_t	*dev;

	if ((dev = calloc(1, sizeof(*dev))) == NULL)
		return (NULL);

	dev->cid = CTAP_CID_BROADCAST;

	if (dev_info->io.open == NULL || dev_info->io.close == NULL ||
	    dev_info->io.read == NULL || dev_info->io.write == NULL) {
		fido_log_debug("%s: NULL function", __func__);
		fido_dev_free(&dev);
		return (NULL);
	}
	dev->dev_info = copy_fido_dev_info(dev_info);

	if (dev->dev_info == NULL) {
		fido_log_debug("%s: dev_info not copied correctly", __func__);
		fido_dev_free(&dev);
		return (NULL);
	}

	return (dev);
}

void
fido_dev_free(fido_dev_t **dev_p)
{
	fido_dev_t *dev;

	if (dev_p == NULL || (dev = *dev_p) == NULL)
		return;

	fido_dev_info_free((fido_dev_info_t**) &dev->dev_info, 1);
	free(dev);

	*dev_p = NULL;
}

uint8_t
fido_dev_protocol(const fido_dev_t *dev)
{
	return (dev->attr.protocol);
}

uint8_t
fido_dev_major(const fido_dev_t *dev)
{
	return (dev->attr.major);
}

uint8_t
fido_dev_minor(const fido_dev_t *dev)
{
	return (dev->attr.minor);
}

uint8_t
fido_dev_build(const fido_dev_t *dev)
{
	return (dev->attr.build);
}

uint8_t
fido_dev_flags(const fido_dev_t *dev)
{
	return (dev->attr.flags);
}

bool
fido_dev_is_fido2(const fido_dev_t *dev)
{
	return (dev->attr.flags & FIDO_CAP_CBOR);
}

void
fido_dev_force_u2f(fido_dev_t *dev)
{
	dev->attr.flags &= ~FIDO_CAP_CBOR;
}

void
fido_dev_force_fido2(fido_dev_t *dev)
{
	dev->attr.flags |= FIDO_CAP_CBOR;
}
