/*	$OpenBSD: drm_mem_util.h,v 1.2 2014/07/12 18:48:52 tedu Exp $	*/
/*
 * Copyright © 2008 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *     Jesse Barnes <jbarnes@virtuousgeek.org>
 *
 */
#ifndef _DRM_MEM_UTIL_H_
#define _DRM_MEM_UTIL_H_

#include <sys/types.h>
#include <sys/malloc.h>

static __inline__ void *drm_calloc_large(size_t nmemb, size_t size)
{
	return drm_calloc(nmemb, size);
#ifdef notyet
	if (size != 0 && nmemb > SIZE_MAX / size)
		return NULL;

	if (size * nmemb <= PAGE_SIZE)
	    return kcalloc(nmemb, size, GFP_KERNEL);

	return __vmalloc(size * nmemb,
			 GFP_KERNEL | __GFP_HIGHMEM | __GFP_ZERO, PAGE_KERNEL);
#endif
}

/* Modeled after cairo's malloc_ab, it's like calloc but without the zeroing. */
static __inline__ void *drm_malloc_ab(size_t nmemb, size_t size)
{
	printf("%s stub\n", __func__);
	return NULL;
#ifdef notyet
	if (size != 0 && nmemb > SIZE_MAX / size)
		return NULL;

	if (size * nmemb <= PAGE_SIZE)
	    return malloc(nmemb * size, M_DRM, M_WAITOK);

	return __vmalloc(size * nmemb,
			 GFP_KERNEL | __GFP_HIGHMEM, PAGE_KERNEL);
#endif
}

static __inline void drm_free_large(void *ptr)
{
	free(ptr, M_DRM, 0);
#ifdef notyet
	if (!is_vmalloc_addr(ptr))
		return free(ptr, M_DRM, 0);

	vfree(ptr);
#endif
}

#endif
