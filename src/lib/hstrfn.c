/*
 * Copyright (c) 2016 Raphael Manfredi
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup lib
 * @file
 *
 * String functions where memory is dynamically allocated via halloc().
 *
 * @author Raphael Manfredi
 * @date 2016
 */

#include "common.h"

#include "hstrfn.h"

#include "concat.h"
#include "halloc.h"
#include "mempcpy.h"
#include "misc.h"
#include "thread.h"
#include "unsigned.h"
#include "walloc.h"

#include "override.h"			/* Must be the last header included */

#ifndef TRACK_MALLOC
/**
 * A clone of strdup() using halloc().
 * The resulting string must be freed via hfree().
 *
 * @param str		the string to duplicate (can be NULL)
 *
 * @return a pointer to the new string.
 */
char *
h_strdup(const char *str)
{
	return str ? hcopy(str, 1 + strlen(str)) : NULL;
}

/**
 * A clone of strndup() using halloc().
 * The resulting string must be freed via hfree().
 *
 * @param str		the string to duplicate a part of (can be NULL)
 * @param n			the maximum number of characters to copy from string
 *
 * @return a pointer to the new string.
 */
char *
h_strndup(const char *str, size_t n)
{
	g_assert(size_is_non_negative(n));

	if (str != NULL) {
		size_t len = clamp_strlen(str, n);
		char *result = halloc(len + 1);
		char *p;

		/* Not hcopy() because we shouldn't even read the nth byte of src. */
		p = mempcpy(result, str, len);
		*p = '\0';

		return result;
	} else {
		return NULL;
	}
}

/**
 * Like h_strjoinv() but the length of the separator is given, meaning it can
 * contain embedded NUL characters and does not require to be NUL-terminated.
 *
 * @param separator		string to insert between each strings
 * @param seplen		amount of bytes in the separator string
 * @param str_array		a NULL-terminated array of strings to join
 *
 * @return a newly allocated string joining all the strings from the array,
 * with the separator between them.
 */
char *
h_strnjoinv(const char *separator, size_t seplen, char * const *str_array)
{
	const char *sep = separator;
	char *result;

	g_assert(str_array != NULL);
	g_assert(separator != NULL || 0 == seplen);

	if (str_array[0] != NULL) {
		size_t i, len, pos;

		len = size_saturate_add(1, strlen(str_array[0]));
		for (i = 1; str_array[i] != NULL; i++) {
			len = size_saturate_add(len, seplen);
			len = size_saturate_add(len, strlen(str_array[i]));
		}

		g_assert(len < SIZE_MAX);

		result = halloc(len);
		pos = strcpy_len(result, str_array[0]);

		/* We can freely add to pos, we know it cannot saturate now */

		for (i = 1; str_array[i] != NULL; i++) {
			memcpy(&result[pos], sep, seplen);
			pos += seplen;
			pos += strcpy_len(&result[pos], str_array[i]);
		}

		g_assert(pos + 1 == len);
	} else {
		result = h_strdup("");
	}

	return result;
}

/**
 * A clone of g_strjoinv() which uses halloc().
 * The resulting string must be freed via hfree().
 *
 * Joins a number of strings together to form one long string, with the
 * optional separator inserted between each of them.
 *
 * If separator is NULL, strings in the array are simply concatenated together.
 *
 * @param separator		string to insert between each strings, or NULL
 * @param str_array		a NULL-terminated array of strings to join
 *
 * @return a newly allocated string joining all the strings from the array,
 * with the separator between them.
 */
char *
h_strjoinv(const char *separator, char * const *str_array)
{
	if G_UNLIKELY(NULL == separator)
		return h_strnjoinv(NULL, 0, str_array);

	return h_strnjoinv(separator, strlen(separator), str_array);
}

/**
 * A clone of g_strfreev().
 *
 * Frees (via hfree()) a NULL-terminated array of strings, and the array itself.
 * If called on a NULL value, does nothing.
 */
void
h_strfreev(char **str_array)
{
	if (str_array != NULL) {
		size_t i;

		for (i = 0; str_array[i] != NULL; i++) {
			hfree(str_array[i]);
		}

		hfree(str_array);
	}
}

/**
 * The vectorized version of h_strconcat().
 */
char *
h_strconcat_v(const char *first, va_list ap)
{
	va_list ap2;
	size_t len, ret;
	char *dst;

	VA_COPY(ap2, ap);
	len = concat_strings_v(NULL, 0, first, ap2);
	va_end(ap2);

	len = size_saturate_add(len, 1);
	dst = halloc(len);
	ret = concat_strings_v(dst, len, first, ap);

	g_assert(ret == len - 1);		/* Do not count the trailing NUL */

	return dst;
}

/**
 * A clone of g_strconcat() using halloc().
 * The resulting string must be freed via hfree().
 *
 * Concatenates all of the given strings into one long string.
 *
 * @attention
 * The argument list must end with (void *) 0.
 */
char *
h_strconcat(const char *first, ...)
{
	va_list ap;
	char *result;

	va_start(ap, first);
	result = h_strconcat_v(first, ap);
	va_end(ap);

	return result;
}

/**
 * Calculate the size of the buffer required to hold the string
 * resulting from vnsprintf().
 *
 * @param format The printf format string.
 * @param ap The argument list.
 *
 * @return The size of the buffer required to hold the resulting
 *         string including the terminating NUL.
 */
static size_t G_PRINTF(1, 0)
vprintf_get_size(const char *format, va_list ap)
{
	char *buf;
	size_t size;
	int ret;

	/**
	 * NOTE: ISO C99 ensures that vsnprintf(NULL, 0, ...) calculates the size
	 * of the required buffer but older vsnprintf() return an unspecified value
	 * less than 1 (one) if size is zero. That could be zero which is also
	 * returned for an empty string, so don't try that. Older vsnprintf()
	 * may silently truncate the string if the buffer is insufficient but
	 * don't return the required size.
	 */

	size = 1024;
	buf = walloc(size);

	for (;;) {
		va_list ap2;
		size_t old_size;

		VA_COPY(ap2, ap);
		ret = vsnprintf(buf, size, format, ap2);
		va_end(ap2);

		if (ret < 0) {
			/* Keep trying */
		} else if (UNSIGNED(ret) > size) {
			/* Assume conforming C99 vsnprintf() */
			break;
		} else if (size - ret > 1) {
			/* Only trust this if there's more than 1 byte left. */
			break;
		}

		/* Since vsnprintf() returns an int, INT_MAX is the limit */
		g_assert(size < UNSIGNED(INT_MAX));

		old_size = size;
		size = size_saturate_mult(size, 2);

		buf = wrealloc(buf, old_size, size);
	}

	WFREE_NULL(buf, size);
	return size_saturate_add(ret, 1);
}

/**
 * A clone of g_strdup_vprintf() using halloc().
 * The resulting string must be freed by hfree().
 */
char *
h_strdup_vprintf(const char *format, va_list ap)
{
	va_list ap2;
	char *buf;
	size_t size;
	int ret;

	VA_COPY(ap2, ap);
	size = vprintf_get_size(format, ap2);
	va_end(ap2);

	buf = halloc(size);
	ret = vsnprintf(buf, size, format, ap);

	g_assert(UNSIGNED(ret) == size - 1);

	return buf;
}

/**
 * Like h_strdup_vprintf() but returns the length of the generated string.
 * The resulting string must be freed by hfree().
 */
char *
h_strdup_len_vprintf(const char *format, va_list ap, size_t *len)
{
	va_list ap2;
	char *buf;
	size_t size;
	int ret;

	VA_COPY(ap2, ap);
	size = vprintf_get_size(format, ap2);
	va_end(ap2);

	buf = halloc(size);
	ret = vsnprintf(buf, size, format, ap);

	g_assert(UNSIGNED(ret) == size - 1);

	if (len != NULL)
		*len = UNSIGNED(ret);

	return buf;
}

/**
 * A clone of g_strdup_printf(), using halloc().
 * The resulting string must be freed by hfree().
 */
char *
h_strdup_printf(const char *format, ...)
{
	char *buf;
	va_list args;

	va_start(args, format);
	buf = h_strdup_vprintf(format, args);
	va_end(args);

	return buf;
}
#endif /* !TRACK_MALLOC */

enum hpriv_magic { HPRIV_MAGIC = 0x683e6188 };

struct hpriv {
	enum hpriv_magic magic;	/**< Magic number */
	void *p;				/**< Thread-private pointer (halloc-ed) */
};

static inline void
hpriv_check(const struct hpriv * const hp)
{
	g_assert(hp != NULL);
	g_assert(HPRIV_MAGIC == hp->magic);
}

/**
 * Reclaim a thread-private pointer when the thread is exiting.
 */
static void
h_private_reclaim(void *data, void *unused)
{
	struct hpriv *hp = data;

	(void) unused;

	hpriv_check(hp);

	HFREE_NULL(hp->p);
	hp->magic = 0;
	WFREE(hp);
}

/**
 * Record a pointer as being an halloc-ed() memory zone that needs to be freed
 * when the thread exists or when a new value is recorded.
 *
 * This is used to provide thread-private "lazy memory" whose lifetime does not
 * exceeed that of the next call to the routine that produces these pointers.
 *
 * @param key		the key to use to identify this pointer value
 * @param p			the allocated pointer
 *
 * @return the pointer, as a convenience.
 */
void *
h_private(const void *key, void *p)
{
	struct hpriv *hp;

	hp = thread_private_get(key);

	if G_LIKELY(hp != NULL) {
		hpriv_check(hp);

		HFREE_NULL(hp->p);		/* Free old pointer */
		return hp->p = p;		/* Will be freed next time or at thread exit */
	}

	/*
	 * Allocate a new thread-private pointer object, with a specialized free
	 * routine to be able to reclaim the allocated memory when the thread
	 * exits.
	 */

	WALLOC0(hp);
	hp->magic = HPRIV_MAGIC;

	thread_private_add_extended(key, hp, h_private_reclaim, NULL);

	return hp->p = p;
}

/* vi: set ts=4 sw=4 cindent: */
